#include "spatial/odas_provider.hpp"

#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "spatial/odas_message_parser.hpp"

#if defined(__linux__)
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
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
      healthy_ = false;
      ::close(fd_);
      fd_ = -1;
      return false;
    }
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
  bool ensureConnected()
  {
    if (fd_ >= 0)
    {
      return true;
    }
    if (endpoint_.rfind("unix://", 0) == 0)
    {
      const std::string path = endpoint_.substr(7);
      fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
      if (fd_ < 0)
      {
        return false;
      }
      sockaddr_un addr{};
      addr.sun_family = AF_UNIX;
      if (path.size() >= sizeof(addr.sun_path))
      {
        ::close(fd_);
        fd_ = -1;
        return false;
      }
      std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path.c_str());
      if (::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
      {
        ::close(fd_);
        fd_ = -1;
        return false;
      }
      setNonBlocking();
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
        return false;
      }

      for (addrinfo* p = res; p != nullptr; p = p->ai_next)
      {
        int s = ::socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (s < 0)
        {
          continue;
        }
        if (::connect(s, p->ai_addr, p->ai_addrlen) == 0)
        {
          fd_ = s;
          break;
        }
        ::close(s);
      }
      ::freeaddrinfo(res);
      if (fd_ < 0)
      {
        return false;
      }
      setNonBlocking();
      return true;
    }
    return false;
  }

  void setNonBlocking() const
  {
    const int flags = ::fcntl(fd_, F_GETFL, 0);
    if (flags >= 0)
    {
      (void)::fcntl(fd_, F_SETFL, flags | O_NONBLOCK);
    }
  }
#endif

  std::string endpoint_;
  bool healthy_ = false;
  OdasMessageParser parser_{};
#if defined(__linux__)
  int fd_ = -1;
#endif
};
}  // namespace

std::unique_ptr<IDoaProvider> CreateOdasProvider(const std::string& endpoint)
{
  return std::make_unique<OdasProviderImpl>(endpoint);
}
}  // namespace sonitude::spatial
