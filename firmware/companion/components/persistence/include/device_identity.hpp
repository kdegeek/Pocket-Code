#pragma once

#include "credential_store.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace t3::companion {

inline constexpr std::size_t kMaxDeviceIdBytes = 64;
inline constexpr std::size_t kMaxScopedTokenBytes = 128;
inline constexpr std::size_t kMaxGatewayUrlBytes = 256;
inline constexpr std::size_t kIdentityJournalMaxBytes = 512;

// Only the locally generated identity, the companion-scoped token, gateway,
// and replay cursor are durable. General T3/provider credentials are not part
// of this value type and therefore have no persistence path.
struct DeviceIdentity {
  std::string device_id;
  std::string scoped_token;
  std::string gateway_url;
  std::uint64_t adapter_sequence = 0;
};

class DeviceIdentityStore {
 public:
  explicit DeviceIdentityStore(SlotStorage& storage);
  explicit DeviceIdentityStore(SlotStorage* storage = nullptr);

  [[nodiscard]] bool load();
  [[nodiscard]] bool save(const DeviceIdentity& identity);
  [[nodiscard]] bool update_cursor(std::uint64_t adapter_sequence);
  [[nodiscard]] bool set_gateway(std::string_view gateway_url);
  [[nodiscard]] const DeviceIdentity& value() const { return value_; }
  [[nodiscard]] bool has_value() const { return has_value_; }

 private:
  [[nodiscard]] static bool valid(const DeviceIdentity& identity);
  [[nodiscard]] static std::vector<std::uint8_t> encode(const DeviceIdentity& identity);
  [[nodiscard]] static bool decode(std::span<const std::uint8_t> bytes,
                                   DeviceIdentity& identity);

  SlotStorage* storage_ = nullptr;
  std::optional<TwoSlotJournal> journal_;
  DeviceIdentity value_;
  bool has_value_ = false;
};

}  // namespace t3::companion

namespace t3::companion::persistence {
using ::t3::companion::DeviceIdentity;
using ::t3::companion::DeviceIdentityStore;
}  // namespace t3::companion::persistence
