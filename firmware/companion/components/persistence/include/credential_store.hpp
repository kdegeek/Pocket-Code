#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace t3::companion {

// The 0x6000-byte NVS partition is shared with ESP-IDF bookkeeping. Five
// entries (32-byte SSID + 64-byte passphrase + 16-byte metadata each) fit in a
// bounded <= 1 KiB journal payload, leaving room for the identity and cache
// journals. A normal add never clears this list.
inline constexpr std::size_t kMaxRememberedNetworks = 5;
inline constexpr std::size_t kMaxSsidBytes = 32;
inline constexpr std::size_t kMaxPasswordBytes = 64;
inline constexpr std::size_t kCredentialJournalMaxBytes = 1'024;
inline constexpr std::uint32_t kJournalMagic = 0x54334A31U;  // "T3J1"
inline constexpr std::uint8_t kJournalVersion = 1;

// A slot is the only storage seam used by the firmware. The host tests supply
// an in-memory fake; the ESP-IDF adapter can map these two slots to NVS blobs.
class SlotStorage {
 public:
  virtual ~SlotStorage() = default;
  [[nodiscard]] virtual std::optional<std::vector<std::uint8_t>> read_slot(
      std::size_t slot) const = 0;
  [[nodiscard]] virtual bool write_slot(std::size_t slot,
                                        std::span<const std::uint8_t> bytes) = 0;
};

// Two-slot journal with a sequence, payload length, and CRC-32. A failed
// alternate-slot write never replaces the previous valid record. Committing an
// identical payload is a wear-limited no-op.
class TwoSlotJournal {
 public:
  explicit TwoSlotJournal(SlotStorage& storage,
                          std::size_t max_payload_bytes = kCredentialJournalMaxBytes);

  [[nodiscard]] bool load(std::vector<std::uint8_t>& payload);
  [[nodiscard]] bool commit(std::span<const std::uint8_t> payload);
  [[nodiscard]] bool reset();
  [[nodiscard]] std::size_t active_slot() const { return active_slot_; }
  [[nodiscard]] std::uint32_t sequence() const { return sequence_; }
  [[nodiscard]] bool has_value() const { return has_value_; }

 private:
  [[nodiscard]] bool decode_slot(std::size_t slot, std::uint32_t& sequence,
                                 std::vector<std::uint8_t>& payload) const;
  [[nodiscard]] std::vector<std::uint8_t> encode_record(
      std::uint32_t sequence, std::span<const std::uint8_t> payload) const;

  SlotStorage& storage_;
  std::size_t max_payload_bytes_;
  std::size_t active_slot_ = 0;
  std::uint32_t sequence_ = 0;
  bool has_value_ = false;
  std::vector<std::uint8_t> payload_;
};

struct RememberedNetwork {
  std::string ssid;
  std::string password;
  std::uint32_t insertion_order = 0;
  bool current = false;
};

enum class CredentialMutation { Added, Updated, Rejected, Reset };

struct CredentialMutationResult {
  bool ok = false;
  CredentialMutation kind = CredentialMutation::Rejected;
  std::optional<std::string> evicted_ssid;
  std::string error;
};

// Ordered, bounded remembered-network policy. The current successful network
// is returned first for roaming, while non-current entries retain append order.
// Mutations are encoded and committed before the in-memory list is replaced.
class CredentialStore {
 public:
  explicit CredentialStore(SlotStorage& storage) : CredentialStore(&storage) {}
  explicit CredentialStore(SlotStorage* storage = nullptr);

  [[nodiscard]] bool load();
  [[nodiscard]] CredentialMutationResult add_or_update(std::string_view ssid,
                                                       std::string_view password);
  [[nodiscard]] bool mark_successful(std::string_view ssid);
  [[nodiscard]] CredentialMutationResult factory_reset();
  [[nodiscard]] CredentialMutationResult reset() { return factory_reset(); }

  [[nodiscard]] std::size_t capacity() const { return kMaxRememberedNetworks; }
  [[nodiscard]] bool empty() const { return entries_.empty(); }
  [[nodiscard]] std::size_t size() const { return entries_.size(); }
  [[nodiscard]] bool reset_requested() const { return reset_requested_; }
  [[nodiscard]] const std::vector<RememberedNetwork>& entries() const { return entries_; }
  [[nodiscard]] std::vector<RememberedNetwork> fallback_sequence() const;
  [[nodiscard]] std::optional<RememberedNetwork> current() const;
  [[nodiscard]] std::optional<RememberedNetwork> find(std::string_view ssid) const;

 private:
  [[nodiscard]] bool persist(const std::vector<RememberedNetwork>& next);
  [[nodiscard]] static bool valid_credential(std::string_view ssid,
                                             std::string_view password,
                                             std::string& error);
  [[nodiscard]] static std::vector<std::uint8_t> encode(
      const std::vector<RememberedNetwork>& entries);
  [[nodiscard]] static bool decode(std::span<const std::uint8_t> bytes,
                                   std::vector<RememberedNetwork>& entries);

  SlotStorage* storage_ = nullptr;
  std::optional<TwoSlotJournal> journal_;
  std::vector<RememberedNetwork> entries_;
  std::uint32_t next_insertion_order_ = 1;
  bool reset_requested_ = false;
};

}  // namespace t3::companion

namespace t3::companion::persistence {
using ::t3::companion::CredentialMutation;
using ::t3::companion::CredentialMutationResult;
using ::t3::companion::CredentialStore;
using ::t3::companion::RememberedNetwork;
using ::t3::companion::SlotStorage;
using ::t3::companion::TwoSlotJournal;
}  // namespace t3::companion::persistence
