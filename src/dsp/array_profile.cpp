#include "dsp/array_profile.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>

#include "spatial/angles.hpp"

namespace sonitude::dsp
{
namespace
{
constexpr std::uint64_t kFnvOffset = 14695981039346656037ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

std::uint64_t Fnv1a64(const std::uint8_t* data, const std::size_t n)
{
  std::uint64_t h = kFnvOffset;
  for (std::size_t i = 0; i < n; ++i)
  {
    h ^= data[i];
    h *= kFnvPrime;
  }
  return h;
}

std::uint16_t ReadU16(const std::uint8_t* p)
{
  return static_cast<std::uint16_t>(p[0] | (static_cast<std::uint16_t>(p[1]) << 8U));
}

std::uint32_t ReadU32(const std::uint8_t* p)
{
  return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8U) |
         (static_cast<std::uint32_t>(p[2]) << 16U) | (static_cast<std::uint32_t>(p[3]) << 24U);
}

float ReadF32(const std::uint8_t* p)
{
  std::uint32_t bits = ReadU32(p);
  float v = 0.0F;
  std::memcpy(&v, &bits, sizeof(v));
  return v;
}

void WriteU16(std::uint8_t* p, const std::uint16_t v)
{
  p[0] = static_cast<std::uint8_t>(v & 0xffU);
  p[1] = static_cast<std::uint8_t>((v >> 8U) & 0xffU);
}

void WriteU32(std::uint8_t* p, const std::uint32_t v)
{
  p[0] = static_cast<std::uint8_t>(v & 0xffU);
  p[1] = static_cast<std::uint8_t>((v >> 8U) & 0xffU);
  p[2] = static_cast<std::uint8_t>((v >> 16U) & 0xffU);
  p[3] = static_cast<std::uint8_t>((v >> 24U) & 0xffU);
}

void WriteF32(std::uint8_t* p, const float v)
{
  std::uint32_t bits = 0;
  std::memcpy(&bits, &v, sizeof(bits));
  WriteU32(p, bits);
}

std::size_t PayloadBytes(const ArrayProfile& profile)
{
  const std::size_t dirs = profile.direction_count;
  const std::size_t bins = profile.bin_count;
  const std::size_t mics = profile.mic_count;
  const std::size_t cpx = dirs * bins * mics * 2U * sizeof(float);
  const std::size_t valid = dirs * bins;
  const std::size_t valid_pad = (valid + 3U) & ~std::size_t{3};
  const std::size_t scale = bins * sizeof(float);
  const std::size_t az = dirs * sizeof(float);
  return (2U * cpx) + valid_pad + scale + az;
}

bool FiniteSpan(const std::vector<float>& v)
{
  return std::all_of(v.begin(), v.end(), [](const float x) { return std::isfinite(x); });
}
}  // namespace

std::vector<std::uint8_t> SerializeArrayProfile(const ArrayProfile& profile)
{
  if (profile.mic_count != audio::kMicChannels || profile.bin_count == 0 ||
      profile.direction_count == 0)
  {
    throw std::runtime_error("SerializeArrayProfile: invalid dimensions");
  }
  const std::size_t payload = PayloadBytes(profile);
  if (payload / 2U > (std::numeric_limits<std::uint32_t>::max() / 2U))
  {
    throw std::runtime_error("SerializeArrayProfile: payload too large");
  }
  std::vector<std::uint8_t> out(kArrayProfileHeaderBytes + payload, 0);
  std::memcpy(out.data(), kArrayProfileMagic, 4);
  WriteU16(out.data() + 4, kArrayProfileFormatVersion);
  out[6] = 1;
  out[7] = 1;
  WriteU32(out.data() + 8, profile.sample_rate_hz);
  WriteU16(out.data() + 12, profile.fft_size);
  WriteU16(out.data() + 14, profile.hop_size);
  out[16] = profile.mic_count;
  out[17] = profile.bin_count;
  WriteU16(out.data() + 18, profile.direction_count);
  out[20] = profile.reference_mic;
  out[21] = profile.left_ear_mic;
  out[22] = profile.right_ear_mic;
  out[23] = profile.transform_sign;
  WriteU32(out.data() + 24, static_cast<std::uint32_t>(payload));
  const std::size_t geom_n = std::min<std::size_t>(31, profile.identity.geometry_id.size());
  std::memcpy(out.data() + 32, profile.identity.geometry_id.data(), geom_n);
  std::memcpy(out.data() + 64, profile.identity.conditioning_hash.data(), 16);
  out[96] = profile.identity.synthetic ? 1 : 0;
  out[97] = static_cast<std::uint8_t>(profile.identity.noise_model);
  WriteF32(out.data() + 100, profile.reference_gain);

  std::uint8_t* p = out.data() + kArrayProfileHeaderBytes;
  const auto append_f32 = [&](const std::vector<float>& v) {
    for (const float x : v)
    {
      WriteF32(p, x);
      p += 4;
    }
  };
  append_f32(profile.weights_ri);
  append_f32(profile.steering_ri);
  const std::size_t valid_n = static_cast<std::size_t>(profile.direction_count) * profile.bin_count;
  if (profile.valid.size() != valid_n ||
      profile.weights_ri.size() != valid_n * profile.mic_count * 2U ||
      profile.steering_ri.size() != profile.weights_ri.size() ||
      profile.dominance_scale.size() != profile.bin_count ||
      profile.azimuth_deg.size() != profile.direction_count)
  {
    throw std::runtime_error("SerializeArrayProfile: array size mismatch");
  }
  std::memcpy(p, profile.valid.data(), valid_n);
  p += (valid_n + 3U) & ~std::size_t{3};
  append_f32(profile.dominance_scale);
  append_f32(profile.azimuth_deg);

  const std::uint64_t hash = Fnv1a64(out.data() + kArrayProfileHeaderBytes, payload);
  WriteU32(out.data() + 80, static_cast<std::uint32_t>(hash & 0xffffffffULL));
  WriteU32(out.data() + 84, static_cast<std::uint32_t>(hash >> 32U));
  return out;
}

ArrayProfile LoadArrayProfileFromBytes(const std::span<const std::uint8_t> bytes)
{
  if (bytes.size() < kArrayProfileHeaderBytes)
  {
    throw std::runtime_error("array profile truncated header");
  }
  if (std::memcmp(bytes.data(), kArrayProfileMagic, 4) != 0)
  {
    throw std::runtime_error("array profile magic mismatch");
  }
  if (ReadU16(bytes.data() + 4) != kArrayProfileFormatVersion)
  {
    throw std::runtime_error("unsupported array profile format version");
  }
  if (bytes[6] != 1 || bytes[7] != 1)
  {
    throw std::runtime_error("array profile must be little-endian float32");
  }
  ArrayProfile profile;
  profile.sample_rate_hz = ReadU32(bytes.data() + 8);
  profile.fft_size = ReadU16(bytes.data() + 12);
  profile.hop_size = ReadU16(bytes.data() + 14);
  profile.mic_count = bytes[16];
  profile.bin_count = bytes[17];
  profile.direction_count = ReadU16(bytes.data() + 18);
  profile.reference_mic = bytes[20];
  profile.left_ear_mic = bytes[21];
  profile.right_ear_mic = bytes[22];
  profile.transform_sign = bytes[23];
  const std::uint32_t payload = ReadU32(bytes.data() + 24);
  if (payload > bytes.size() - kArrayProfileHeaderBytes)
  {
    throw std::runtime_error("array profile payload truncated");
  }
  if (static_cast<std::size_t>(profile.mic_count) * profile.bin_count * profile.direction_count >
      (std::numeric_limits<std::size_t>::max() / 16U))
  {
    throw std::runtime_error("array profile dimensions overflow");
  }
  std::string geom;
  for (std::size_t i = 0; i < 32; ++i)
  {
    const char c = static_cast<char>(bytes.data()[32 + i]);
    if (c == '\0')
    {
      break;
    }
    geom.push_back(c);
  }
  profile.identity.geometry_id = geom;
  std::memcpy(profile.identity.conditioning_hash.data(), bytes.data() + 64, 16);
  profile.identity.synthetic = bytes[96] != 0;
  profile.identity.noise_model = static_cast<ArrayNoiseModel>(bytes[97]);
  profile.reference_gain = ReadF32(bytes.data() + 100);

  const std::size_t expected = PayloadBytes(profile);
  if (expected != payload)
  {
    throw std::runtime_error("array profile payload size mismatch");
  }
  const std::uint64_t stored =
      static_cast<std::uint64_t>(ReadU32(bytes.data() + 80)) |
      (static_cast<std::uint64_t>(ReadU32(bytes.data() + 84)) << 32U);
  const std::uint64_t hash = Fnv1a64(bytes.data() + kArrayProfileHeaderBytes, payload);
  if (stored != hash)
  {
    throw std::runtime_error("array profile content hash mismatch");
  }

  const std::uint8_t* p = bytes.data() + kArrayProfileHeaderBytes;
  const std::size_t cpx_n =
      static_cast<std::size_t>(profile.direction_count) * profile.bin_count * profile.mic_count * 2U;
  profile.weights_ri.resize(cpx_n);
  profile.steering_ri.resize(cpx_n);
  for (std::size_t i = 0; i < cpx_n; ++i)
  {
    profile.weights_ri[i] = ReadF32(p);
    p += 4;
  }
  for (std::size_t i = 0; i < cpx_n; ++i)
  {
    profile.steering_ri[i] = ReadF32(p);
    p += 4;
  }
  const std::size_t valid_n = static_cast<std::size_t>(profile.direction_count) * profile.bin_count;
  profile.valid.assign(p, p + valid_n);
  p += (valid_n + 3U) & ~std::size_t{3};
  profile.dominance_scale.resize(profile.bin_count);
  for (std::size_t i = 0; i < profile.bin_count; ++i)
  {
    profile.dominance_scale[i] = ReadF32(p);
    p += 4;
  }
  profile.azimuth_deg.resize(profile.direction_count);
  for (std::size_t i = 0; i < profile.direction_count; ++i)
  {
    profile.azimuth_deg[i] = ReadF32(p);
    p += 4;
  }
  if (!FiniteSpan(profile.weights_ri) || !FiniteSpan(profile.steering_ri) ||
      !FiniteSpan(profile.dominance_scale) || !FiniteSpan(profile.azimuth_deg))
  {
    throw std::runtime_error("array profile contains non-finite values");
  }
  return profile;
}

ArrayProfile LoadArrayProfileFromFile(const std::string& path)
{
  std::ifstream in(path, std::ios::binary);
  if (!in)
  {
    throw std::runtime_error("failed to open array profile: " + path);
  }
  in.seekg(0, std::ios::end);
  const std::streamoff n = in.tellg();
  if (n < 0)
  {
    throw std::runtime_error("failed to size array profile: " + path);
  }
  in.seekg(0, std::ios::beg);
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(n));
  in.read(reinterpret_cast<char*>(bytes.data()), n);
  if (!in)
  {
    throw std::runtime_error("failed to read array profile: " + path);
  }
  return LoadArrayProfileFromBytes(bytes);
}

void ValidateArrayProfile(const ArrayProfile& profile,
                          const std::uint32_t sample_rate_hz,
                          const std::uint16_t fft_size,
                          const std::uint16_t hop_size)
{
  if (profile.sample_rate_hz != sample_rate_hz)
  {
    throw std::runtime_error("array profile sample rate mismatch");
  }
  if (profile.fft_size != fft_size || profile.hop_size != hop_size)
  {
    throw std::runtime_error("array profile FFT/hop mismatch");
  }
  if (profile.mic_count != audio::kMicChannels)
  {
    throw std::runtime_error("array profile microphone count mismatch");
  }
  if (profile.reference_mic >= profile.mic_count || profile.left_ear_mic >= profile.mic_count ||
      profile.right_ear_mic >= profile.mic_count)
  {
    throw std::runtime_error("array profile ear/reference index out of range");
  }
  if (profile.transform_sign != 0)
  {
    throw std::runtime_error("array profile transform sign is not the runtime forward DFT");
  }
}

std::size_t NearestAzimuthIndex(const ArrayProfile& profile, const float azimuth_deg)
{
  if (profile.azimuth_deg.empty())
  {
    throw std::runtime_error("array profile has no azimuth table");
  }
  const double az = spatial::NormalizeAzimuthDeg(static_cast<double>(azimuth_deg));
  std::size_t best = 0;
  double best_d = 1.0e9;
  for (std::size_t i = 0; i < profile.azimuth_deg.size(); ++i)
  {
    const double d = std::fabs(spatial::NormalizeAzimuthDeg(
        az - static_cast<double>(profile.azimuth_deg[i])));
    if (d < best_d)
    {
      best_d = d;
      best = i;
    }
  }
  return best;
}
}  // namespace sonitude::dsp
