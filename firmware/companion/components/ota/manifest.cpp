#include "manifest.hpp"

#include <algorithm>
#include <array>
#include <cstring>

namespace t3::companion {
namespace {

constexpr std::array<std::uint32_t, 64> kRoundConstants = {
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
    0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
    0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
    0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
    0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
    0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
    0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
    0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
    0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
    0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
    0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
    0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};

constexpr std::array<std::uint32_t, 8> kInitialState = {
    0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
    0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};

constexpr std::uint32_t rotr(std::uint32_t value, std::uint32_t amount) {
  return (value >> amount) | (value << (32U - amount));
}

constexpr std::uint32_t load_be32(const std::uint8_t* bytes) {
  return (static_cast<std::uint32_t>(bytes[0]) << 24U) |
         (static_cast<std::uint32_t>(bytes[1]) << 16U) |
         (static_cast<std::uint32_t>(bytes[2]) << 8U) |
         static_cast<std::uint32_t>(bytes[3]);
}

void append_u64(std::string& output, std::uint64_t value) {
  output += std::to_string(value);
}

void append_field(std::string& output, std::string_view value) {
  output += std::to_string(value.size());
  output.push_back(':');
  output.append(value.data(), value.size());
  output.push_back('|');
}

char hex_digit(std::uint8_t value) {
  return value < 10 ? static_cast<char>('0' + value)
                    : static_cast<char>('a' + (value - 10));
}

std::string fixture_signature(std::string_view payload) {
  // FNV-1a is intentionally only a deterministic fixture marker. It is not a
  // cryptographic signature and is never accepted by a production verifier.
  constexpr std::string_view prefix =
      "t3-companion-fixture-public-key-v1|";
  std::uint64_t hash = 1469598103934665603ULL;
  for (const auto value : {prefix, payload}) {
    for (const unsigned char byte : value) {
      hash ^= byte;
      hash *= 1099511628211ULL;
    }
  }
  std::string output(16, '0');
  for (std::size_t index = 0; index < output.size(); ++index) {
    const auto shift = static_cast<unsigned>((15U - index) * 4U);
    output[index] = hex_digit(static_cast<std::uint8_t>((hash >> shift) & 0x0fU));
  }
  return output;
}

}  // namespace

std::string OtaManifest::signed_payload() const {
  std::string payload;
  payload.reserve(256 + chip.size() + image_sha256.size() + key_id.size());
  append_u64(payload, schema_version);
  payload.push_back('|');
  append_u64(payload, version);
  payload.push_back('|');
  append_u64(payload, protocol_min);
  payload.push_back('|');
  append_u64(payload, protocol_max);
  payload.push_back('|');
  append_field(payload, chip);
  append_u64(payload, flash_size_bytes);
  payload.push_back('|');
  append_u64(payload, slot_size_bytes);
  payload.push_back('|');
  append_u64(payload, image_size_bytes);
  payload.push_back('|');
  append_u64(payload, storage_offset);
  payload.push_back('|');
  append_u64(payload, storage_size_bytes);
  payload.push_back('|');
  append_field(payload, image_sha256);
  append_field(payload, key_id);
  return payload;
}

bool FixtureSignatureVerifier::verify(std::string_view key_id,
                                      std::string_view signed_payload,
                                      std::string_view signature) const {
  return key_id == kKeyId && signature == signature_for_test(signed_payload);
}

std::string FixtureSignatureVerifier::signature_for_test(
    std::string_view signed_payload) {
  return fixture_signature(signed_payload);
}

OtaManifestValidation validate_manifest(const OtaManifest& manifest,
                                        const OtaDeviceProfile& device,
                                        const SignatureVerifier& verifier) {
  if (manifest.schema_version != 1) {
    return OtaManifestValidation::failure(
        OtaManifestErrorCode::InvalidSchema, "unsupported OTA manifest schema");
  }
  if (manifest.version == 0) {
    return OtaManifestValidation::failure(
        OtaManifestErrorCode::InvalidVersion, "OTA version must be non-zero");
  }
  if (manifest.protocol_min == 0 || manifest.protocol_min > manifest.protocol_max) {
    return OtaManifestValidation::failure(
        OtaManifestErrorCode::InvalidProtocolRange, "invalid manifest protocol range");
  }
  if (manifest.protocol_max < device.protocol_min ||
      manifest.protocol_min > device.protocol_max) {
    return OtaManifestValidation::failure(
        OtaManifestErrorCode::ProtocolMismatch,
        "manifest protocol range does not intersect device support");
  }
  if (manifest.chip.empty() || manifest.chip != device.chip) {
    return OtaManifestValidation::failure(OtaManifestErrorCode::ChipMismatch,
                                          "manifest chip does not match device");
  }
  if (manifest.flash_size_bytes == 0 ||
      manifest.flash_size_bytes != device.flash_size_bytes ||
      manifest.slot_size_bytes == 0) {
    return OtaManifestValidation::failure(
        OtaManifestErrorCode::FlashGeometryMismatch,
        "manifest flash geometry does not match device");
  }
  if (manifest.storage_offset != device.storage_offset ||
      manifest.storage_size_bytes != device.storage_size_bytes) {
    return OtaManifestValidation::failure(
        OtaManifestErrorCode::StorageGeometryMismatch,
        "OTA cannot change the durable storage partition");
  }
  if (manifest.image_size_bytes == 0 ||
      manifest.image_size_bytes > manifest.slot_size_bytes) {
    return OtaManifestValidation::failure(OtaManifestErrorCode::ImageTooLarge,
                                          "OTA image exceeds its inactive slot");
  }
  if (!is_sha256_hex(manifest.image_sha256)) {
    return OtaManifestValidation::failure(OtaManifestErrorCode::InvalidHash,
                                          "image hash must be 64 lowercase hex bytes");
  }
  if (manifest.key_id.empty()) {
    return OtaManifestValidation::failure(OtaManifestErrorCode::UnknownKey,
                                          "manifest has no signing key identifier");
  }
  if (!verifier.verify(manifest.key_id, manifest.signed_payload(),
                       manifest.signature)) {
    return OtaManifestValidation::failure(
        OtaManifestErrorCode::SignatureMismatch,
        "manifest signature does not verify against canonical metadata");
  }
  if (!device.allow_downgrade && manifest.version <= device.current_version) {
    return OtaManifestValidation::failure(
        OtaManifestErrorCode::Downgrade,
        "anti-rollback policy rejects a non-increasing firmware version");
  }
  return OtaManifestValidation::success();
}

Sha256Hasher::Sha256Hasher() { reset(); }

void Sha256Hasher::reset() {
  state_ = kInitialState;
  buffer_.fill(0);
  buffer_size_ = 0;
  bit_count_ = 0;
}

void Sha256Hasher::transform(const std::uint8_t* block) {
  std::array<std::uint32_t, 64> words{};
  for (std::size_t index = 0; index < 16; ++index) {
    words[index] = load_be32(block + index * 4);
  }
  for (std::size_t index = 16; index < words.size(); ++index) {
    const std::uint32_t s0 = rotr(words[index - 15], 7) ^
                             rotr(words[index - 15], 18) ^
                             (words[index - 15] >> 3);
    const std::uint32_t s1 = rotr(words[index - 2], 17) ^
                             rotr(words[index - 2], 19) ^
                             (words[index - 2] >> 10);
    words[index] = words[index - 16] + s0 + words[index - 7] + s1;
  }
  std::uint32_t a = state_[0];
  std::uint32_t b = state_[1];
  std::uint32_t c = state_[2];
  std::uint32_t d = state_[3];
  std::uint32_t e = state_[4];
  std::uint32_t f = state_[5];
  std::uint32_t g = state_[6];
  std::uint32_t h = state_[7];
  for (std::size_t index = 0; index < words.size(); ++index) {
    const std::uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
    const std::uint32_t choose = (e & f) ^ ((~e) & g);
    const std::uint32_t temp1 = h + s1 + choose + kRoundConstants[index] +
                                words[index];
    const std::uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
    const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
    const std::uint32_t temp2 = s0 + majority;
    h = g;
    g = f;
    f = e;
    e = d + temp1;
    d = c;
    c = b;
    b = a;
    a = temp1 + temp2;
  }
  state_[0] += a;
  state_[1] += b;
  state_[2] += c;
  state_[3] += d;
  state_[4] += e;
  state_[5] += f;
  state_[6] += g;
  state_[7] += h;
}

void Sha256Hasher::update(std::span<const std::uint8_t> bytes) {
  bit_count_ += static_cast<std::uint64_t>(bytes.size()) * 8U;
  while (!bytes.empty()) {
    const std::size_t available = buffer_.size() - buffer_size_;
    const std::size_t copied = std::min(available, bytes.size());
    std::memcpy(buffer_.data() + buffer_size_, bytes.data(), copied);
    buffer_size_ += copied;
    bytes = bytes.subspan(copied);
    if (buffer_size_ == buffer_.size()) {
      transform(buffer_.data());
      buffer_size_ = 0;
    }
  }
}

std::array<std::uint8_t, 32> Sha256Hasher::digest() const {
  Sha256Hasher copy = *this;
  copy.buffer_[copy.buffer_size_++] = 0x80U;
  if (copy.buffer_size_ > 56) {
    while (copy.buffer_size_ < 64) copy.buffer_[copy.buffer_size_++] = 0;
    copy.transform(copy.buffer_.data());
    copy.buffer_size_ = 0;
  }
  while (copy.buffer_size_ < 56) copy.buffer_[copy.buffer_size_++] = 0;
  for (std::size_t index = 0; index < 8; ++index) {
    copy.buffer_[56 + index] = static_cast<std::uint8_t>(
        (copy.bit_count_ >> ((7U - static_cast<unsigned>(index)) * 8U)) & 0xffU);
  }
  copy.transform(copy.buffer_.data());

  std::array<std::uint8_t, 32> result{};
  for (std::size_t index = 0; index < copy.state_.size(); ++index) {
    result[index * 4] = static_cast<std::uint8_t>(copy.state_[index] >> 24U);
    result[index * 4 + 1] = static_cast<std::uint8_t>(copy.state_[index] >> 16U);
    result[index * 4 + 2] = static_cast<std::uint8_t>(copy.state_[index] >> 8U);
    result[index * 4 + 3] = static_cast<std::uint8_t>(copy.state_[index]);
  }
  return result;
}

std::string Sha256Hasher::finalize_hex() const {
  const auto bytes = digest();
  std::string output;
  output.reserve(bytes.size() * 2);
  for (const auto byte : bytes) {
    output.push_back(hex_digit(static_cast<std::uint8_t>(byte >> 4U)));
    output.push_back(hex_digit(static_cast<std::uint8_t>(byte & 0x0fU)));
  }
  return output;
}

std::string sha256_hex(std::span<const std::uint8_t> bytes) {
  Sha256Hasher hasher;
  hasher.update(bytes);
  return hasher.finalize_hex();
}

bool is_sha256_hex(std::string_view value) {
  if (value.size() != 64) return false;
  for (const unsigned char byte : value) {
    if (!((byte >= '0' && byte <= '9') ||
          (byte >= 'a' && byte <= 'f'))) {
      return false;
    }
  }
  return true;
}

}  // namespace t3::companion
