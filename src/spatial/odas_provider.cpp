#include "spatial/odas_provider.hpp"

#include <cstdio>
#include <cerrno>
#include <chrono>
#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "spatial/odas_message_parser.hpp"

#if defined(__linux__)
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace sonitude::spatial
{
namespace
{
class OdasProviderImpl final : public IDoaProvider
{
 public:
  explicit OdasProviderImpl(std::string endpoint) : endpoint_(std::move(endpoint)) {}
  ~OdasProviderImpl() override
  {
#if defined(__linux__)
    if (fd_ >= 0)
    {
      ::close(fd_);
      fd_ = -1;
    }
#endif
  }

  bool poll(std::vector<SourceObservation>& out) override
  {
    out.clear();
#if defined(__linux__)
    if (!ensureConnected())
    {
      healthy_ = false;
      return false;
    }

    char buf[4096];
    const ssize_t n = ::recv(fd_, buf, sizeof(buf), MSG_DONTWAIT);
    if (n > 0)
    {
      healthy_ = true;
      auto parsed = parser_.feed(std::string_view(buf, static_cast<std::size_t>(n)));
      out.insert(out.end(), parsed.begin(), parsed.end());
      return !out.empty();
    }
    if (n == 0)
    {
      closeAndArmReconnect();
      return false;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
    {
      return false;
    }
    closeAndArmReconnect();
    return false;
#else
    (void)out;
    healthy_ = false;
    return false;
#endif
  }

  bool isHealthy() const override
  {
    return healthy_;
  }

 private:
#if defined(__linux__)
  static constexpr int kConnectTimeoutMs = 250;
  static constexpr int kBackoffMinMs = 100;
  static constexpr int kBackoffMaxMs = 5000;

  bool setNonBlocking(const int fd) const
  {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0)
    {
      return false;
    }
    return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
  }

  bool connectWithTimeout(const int fd, const sockaddr* addr, const socklen_t addr_len) const
  {
    if (!setNonBlocking(fd))
    {
      return false;
    }
    if (::connect(fd, addr, addr_len) == 0)
    {
      return true;
    }
    if (errno != EINPROGRESS)
    {
      return false;
    }
    pollfd pfd{};
    pfd.fd = fd;
    pfd.events = POLLOUT;
    const int poll_rc = ::poll(&pfd, 1, kConnectTimeoutMs);
    if (poll_rc <= 0)
    {
      return false;
    }
    int so_error = 0;
    socklen_t len = sizeof(so_error);
    if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &len) != 0 || so_error != 0)
    {
      return false;
    }
    return true;
  }

  void armReconnectBackoff()
  {
    next_reconnect_tp_ =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(reconnect_backoff_ms_);
    reconnect_backoff_ms_ = std::min(reconnect_backoff_ms_ * 2, kBackoffMaxMs);
  }

  void closeAndArmReconnect()
  {
    healthy_ = false;
    if (fd_ >= 0)
    {
      ::close(fd_);
      fd_ = -1;
    }
    armReconnectBackoff();
  }

  bool ensureConnected()
  {
    if (fd_ >= 0)
    {
      return true;
    }
    if (std::chrono::steady_clock::now() < next_reconnect_tp_)
    {
      return false;
    }
    if (endpoint_.rfind("unix://", 0) == 0)
    {
      const std::string path = endpoint_.substr(7);
      const int s = ::socket(AF_UNIX, SOCK_STREAM, 0);
      if (s < 0)
      {
        armReconnectBackoff();
        return false;
      }
      sockaddr_un addr{};
      addr.sun_family = AF_UNIX;
      if (path.size() >= sizeof(addr.sun_path))
      {
        ::close(s);
        armReconnectBackoff();
        return false;
      }
      std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path.c_str());
      if (!connectWithTimeout(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)))
      {
        ::close(s);
        armReconnectBackoff();
        return false;
      }
      fd_ = s;
      reconnect_backoff_ms_ = kBackoffMinMs;
      next_reconnect_tp_ = std::chrono::steady_clock::time_point::min();
      return true;
    }
    if (endpoint_.rfind("tcp://", 0) == 0)
    {
      const std::string host_port = endpoint_.substr(6);
      const auto colon = host_port.rfind(':');
      if (colon == std::string::npos)
      {
        return false;
      }
      const std::string host = host_port.substr(0, colon);
      const std::string port = host_port.substr(colon + 1);

      addrinfo hints{};
      hints.ai_family = AF_UNSPEC;
      hints.ai_socktype = SOCK_STREAM;
      addrinfo* res = nullptr;
      if (::getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0)
      {
        armReconnectBackoff();
        return false;
      }

      for (addrinfo* p = res; p != nullptr; p = p->ai_next)
      {
        const int s = ::socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (s < 0)
        {
          continue;
        }
        if (connectWithTimeout(s, p->ai_addr, static_cast<socklen_t>(p->ai_addrlen)))
        {
          fd_ = s;
          break;
        }
        ::close(s);
      }
      ::freeaddrinfo(res);
      if (fd_ < 0)
      {
        armReconnectBackoff();
        return false;
      }
      reconnect_backoff_ms_ = kBackoffMinMs;
      next_reconnect_tp_ = std::chrono::steady_clock::time_point::min();
      return true;
    }
    armReconnectBackoff();
    return false;
  }
#endif

  std::string endpoint_;
  bool healthy_ = false;
  OdasMessageParser parser_{};
#if defined(__linux__)
  int fd_ = -1;
  int reconnect_backoff_ms_ = kBackoffMinMs;
  std::chrono::steady_clock::time_point next_reconnect_tp_{
      std::chrono::steady_clock::time_point::min()};
#endif
};
}  // namespace

std::unique_ptr<IDoaProvider> CreateOdasProvider(const std::string& endpoint)
{
  return std::make_unique<OdasProviderImpl>(endpoint);
}
}  // namespace sonitude::spatial
