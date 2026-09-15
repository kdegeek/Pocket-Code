#include "device_identity.hpp"

#include <algorithm>
#include <cctype>
#include <limits>

namespace t3::companion {
namespace {

void put_u16(std::vector<std::uint8_t>& bytes, std::uint16_t value) {
  bytes.push_back(static_cast<std::uint8_t>(value & 0xffU));
  bytes.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xffU));
}

void put_u64(std::vector<std::uint8_t>& bytes, std::uint64_t value) {
  for (unsigned shift = 0; shift < 64U; shift += 8U) {
    bytes.push_back(static_cast<std::uint8_t>((value >> shift) & 0xffU));
  }
}

[[nodiscard]] std::uint16_t get_u16(std::span<const std::uint8_t> bytes, std::size_t offset) {
  return static_cast<std::uint16_t>(bytes[offset]) |
         static_cast<std::uint16_t>(bytes[offset + 1U] << 8U);
}

[[nodiscard]] std::uint64_t get_u64(std::span<const std::uint8_t> bytes, std::size_t offset) {
  std::uint64_t value = 0;
  for (unsigned shift = 0; shift < 64U; shift += 8U) {
    value |= static_cast<std::uint64_t>(bytes[offset + shift / 8U]) << shift;
  }
  return value;
}

void put_string(std::vector<std::uint8_t>& bytes, std::string_view value) {
  put_u16(bytes, static_cast<std::uint16_t>(value.size()));
  bytes.insert(bytes.end(), value.begin(), value.end());
}

[[nodiscard]] bool read_string(std::span<const std::uint8_t> bytes, std::size_t& cursor,
                               std::size_t max_bytes, std::string& value) {
  if (cursor + 2U > bytes.size()) {
    return false;
  }
  const auto size = get_u16(bytes, cursor);
  cursor += 2U;
  if (size > max_bytes || cursor + size > bytes.size()) {
    return false;
  }
  value.assign(reinterpret_cast<const char*>(bytes.data() + cursor), size);
  cursor += size;
  return true;
}

}  // namespace

DeviceIdentityStore::DeviceIdentityStore(SlotStorage& storage)
    : DeviceIdentityStore(&storage) {}

DeviceIdentityStore::DeviceIdentityStore(SlotStorage* storage) : storage_(storage) {
  if (storage_ != nullptr) {
    journal_.emplace(*storage_, kIdentityJournalMaxBytes);
    (void)load();
  }
}

bool DeviceIdentityStore::valid(const DeviceIdentity& identity) {
  const auto scheme = identity.gateway_url.find("://");
  const bool gateway_empty = identity.gateway_url.empty();
  const bool scheme_ok = gateway_empty ||
                         ((identity.gateway_url.starts_with("http://") ||
                           identity.gateway_url.starts_with("https://")) &&
                          scheme != std::string::npos && scheme + 3U < identity.gateway_url.size());
  const bool host_ok = gateway_empty ||
                       (scheme_ok && std::all_of(
                                          identity.gateway_url.begin() +
                                              static_cast<std::ptrdiff_t>(scheme + 3U),
                                          identity.gateway_url.end(), [](unsigned char byte) {
                                            return !std::isspace(byte) && byte >= 0x21U && byte != 0x7fU;
                                          }));
  const bool token_empty = identity.scoped_token.empty();
  // An unpaired identity (device id only), a gateway-selected enrollment
  // identity (gateway without token), and a fully paired identity are all
  // durable states. A token without a gateway is never valid.
  return !identity.device_id.empty() && identity.device_id.size() <= kMaxDeviceIdBytes &&
         identity.scoped_token.size() <= kMaxScopedTokenBytes &&
         identity.gateway_url.size() <= kMaxGatewayUrlBytes && host_ok &&
         (token_empty || !gateway_empty);
}

std::vector<std::uint8_t> DeviceIdentityStore::encode(const DeviceIdentity& identity) {
  std::vector<std::uint8_t> bytes;
  bytes.reserve(8U + identity.device_id.size() + identity.scoped_token.size() +
                identity.gateway_url.size());
  put_string(bytes, identity.device_id);
  put_string(bytes, identity.scoped_token);
  put_string(bytes, identity.gateway_url);
  put_u64(bytes, identity.adapter_sequence);
  return bytes;
}

bool DeviceIdentityStore::decode(std::span<const std::uint8_t> bytes,
                                 DeviceIdentity& identity) {
  std::size_t cursor = 0;
  DeviceIdentity decoded;
  if (!read_string(bytes, cursor, kMaxDeviceIdBytes, decoded.device_id) ||
      !read_string(bytes, cursor, kMaxScopedTokenBytes, decoded.scoped_token) ||
      !read_string(bytes, cursor, kMaxGatewayUrlBytes, decoded.gateway_url) ||
      cursor + 8U != bytes.size()) {
    return false;
  }
  decoded.adapter_sequence = get_u64(bytes, cursor);
  if (!valid(decoded)) {
    return false;
  }
  identity = std::move(decoded);
  return true;
}

bool DeviceIdentityStore::load() {
  if (!journal_.has_value()) {
    return has_value_;
  }
  std::vector<std::uint8_t> bytes;
  if (!journal_->load(bytes)) {
    value_ = {};
    has_value_ = false;
    return false;
  }
  DeviceIdentity decoded;
  if (!decode(bytes, decoded)) {
    value_ = {};
    has_value_ = false;
    return false;
  }
  value_ = std::move(decoded);
  has_value_ = true;
  return true;
}

bool DeviceIdentityStore::save(const DeviceIdentity& identity) {
  if (!valid(identity)) {
    return false;
  }
  const auto bytes = encode(identity);
  if (journal_.has_value() && !journal_->commit(bytes)) {
    return false;
  }
  value_ = identity;
  has_value_ = true;
  return true;
}

bool DeviceIdentityStore::update_cursor(std::uint64_t adapter_sequence) {
  if (!has_value_) {
    return false;
  }
  DeviceIdentity next = value_;
  next.adapter_sequence = adapter_sequence;
  return save(next);
}

bool DeviceIdentityStore::set_gateway(std::string_view gateway_url) {
  if (!has_value_) {
    return false;
  }
  DeviceIdentity next = value_;
  next.gateway_url.assign(gateway_url.data(), gateway_url.size());
  return save(next);
}

}  // namespace t3::companion
