#include "credential_store.hpp"

#include <algorithm>
#include <array>
#include <limits>

namespace t3::companion {
namespace {

constexpr std::size_t kRecordHeaderBytes = 18;

void put_u32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
  bytes.push_back(static_cast<std::uint8_t>(value & 0xffU));
  bytes.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xffU));
  bytes.push_back(static_cast<std::uint8_t>((value >> 16U) & 0xffU));
  bytes.push_back(static_cast<std::uint8_t>((value >> 24U) & 0xffU));
}

[[nodiscard]] std::uint32_t get_u32(std::span<const std::uint8_t> bytes, std::size_t offset) {
  return static_cast<std::uint32_t>(bytes[offset]) |
         (static_cast<std::uint32_t>(bytes[offset + 1]) << 8U) |
         (static_cast<std::uint32_t>(bytes[offset + 2]) << 16U) |
         (static_cast<std::uint32_t>(bytes[offset + 3]) << 24U);
}

[[nodiscard]] std::uint32_t crc32(std::span<const std::uint8_t> bytes) {
  std::uint32_t crc = 0xffffffffU;
  for (const std::uint8_t byte : bytes) {
    crc ^= byte;
    for (unsigned bit = 0; bit < 8U; ++bit) {
      const std::uint32_t mask = (crc & 1U) == 0U ? 0U : 0xedb88320U;
      crc = (crc >> 1U) ^ mask;
    }
  }
  return ~crc;
}

[[nodiscard]] std::vector<std::uint8_t> crc_input(std::uint8_t version,
                                                  std::uint32_t sequence,
                                                  std::uint32_t payload_size,
                                                  std::span<const std::uint8_t> payload) {
  std::vector<std::uint8_t> bytes;
  bytes.reserve(10U + payload.size());
  bytes.push_back(version);
  bytes.push_back(0U);
  put_u32(bytes, sequence);
  put_u32(bytes, payload_size);
  bytes.insert(bytes.end(), payload.begin(), payload.end());
  return bytes;
}

}  // namespace

TwoSlotJournal::TwoSlotJournal(SlotStorage& storage, std::size_t max_payload_bytes)
    : storage_(storage), max_payload_bytes_(max_payload_bytes) {}

std::vector<std::uint8_t> TwoSlotJournal::encode_record(
    std::uint32_t sequence, std::span<const std::uint8_t> payload) const {
  std::vector<std::uint8_t> bytes;
  bytes.reserve(kRecordHeaderBytes + payload.size());
  put_u32(bytes, kJournalMagic);
  bytes.push_back(kJournalVersion);
  bytes.push_back(0U);
  put_u32(bytes, sequence);
  put_u32(bytes, static_cast<std::uint32_t>(payload.size()));
  const auto checksum = crc32(crc_input(kJournalVersion, sequence,
                                        static_cast<std::uint32_t>(payload.size()), payload));
  put_u32(bytes, checksum);
  bytes.insert(bytes.end(), payload.begin(), payload.end());
  return bytes;
}

bool TwoSlotJournal::decode_slot(std::size_t slot, std::uint32_t& sequence,
                                 std::vector<std::uint8_t>& payload) const {
  const auto record = storage_.read_slot(slot);
  if (!record.has_value() || record->size() < kRecordHeaderBytes) {
    return false;
  }
  const std::span<const std::uint8_t> bytes(record->data(), record->size());
  if (get_u32(bytes, 0) != kJournalMagic || bytes[4] != kJournalVersion || bytes[5] != 0U) {
    return false;
  }
  sequence = get_u32(bytes, 6);
  const auto payload_size = get_u32(bytes, 10);
  if (payload_size > max_payload_bytes_ ||
      bytes.size() != kRecordHeaderBytes + static_cast<std::size_t>(payload_size)) {
    return false;
  }
  payload.assign(bytes.begin() + static_cast<std::ptrdiff_t>(kRecordHeaderBytes), bytes.end());
  const auto expected = get_u32(bytes, 14);
  const auto actual = crc32(crc_input(kJournalVersion, sequence, payload_size, payload));
  if (expected != actual) {
    payload.clear();
    return false;
  }
  return true;
}

bool TwoSlotJournal::load(std::vector<std::uint8_t>& payload) {
  std::uint32_t first_sequence = 0;
  std::uint32_t second_sequence = 0;
  std::vector<std::uint8_t> first_payload;
  std::vector<std::uint8_t> second_payload;
  const bool first_valid = decode_slot(0, first_sequence, first_payload);
  const bool second_valid = decode_slot(1, second_sequence, second_payload);
  if (!first_valid && !second_valid) {
    active_slot_ = 0;
    sequence_ = 0;
    has_value_ = false;
    payload_.clear();
    payload.clear();
    return false;
  }
  if (second_valid && (!first_valid || second_sequence > first_sequence)) {
    active_slot_ = 1;
    sequence_ = second_sequence;
    payload_ = std::move(second_payload);
  } else {
    active_slot_ = 0;
    sequence_ = first_sequence;
    payload_ = std::move(first_payload);
  }
  has_value_ = true;
  payload = payload_;
  return true;
}

bool TwoSlotJournal::commit(std::span<const std::uint8_t> payload) {
  if (payload.size() > max_payload_bytes_) {
    return false;
  }
  if (!has_value_) {
    std::vector<std::uint8_t> ignored;
    (void)load(ignored);
  }
  if (has_value_ && payload.size() == payload_.size() &&
      std::equal(payload.begin(), payload.end(), payload_.begin())) {
    return true;
  }
  const std::size_t target = has_value_ ? (active_slot_ == 0U ? 1U : 0U) : 0U;
  const auto next_sequence = has_value_ ? sequence_ + 1U : 1U;
  const auto record = encode_record(next_sequence, payload);
  if (!storage_.write_slot(target, record)) {
    return false;
  }
  active_slot_ = target;
  sequence_ = next_sequence;
  has_value_ = true;
  payload_.assign(payload.begin(), payload.end());
  return true;
}

bool TwoSlotJournal::reset() {
  const std::span<const std::uint8_t> empty;
  return commit(empty);
}

CredentialStore::CredentialStore(SlotStorage* storage) : storage_(storage) {
  if (storage_ != nullptr) {
    journal_.emplace(*storage_);
    (void)load();
  }
}

bool CredentialStore::load() {
  if (!journal_.has_value()) {
    return true;
  }
  std::vector<std::uint8_t> bytes;
  if (!journal_->load(bytes)) {
    entries_.clear();
    next_insertion_order_ = 1;
    reset_requested_ = false;
    return true;
  }
  std::vector<RememberedNetwork> loaded;
  if (!decode(bytes, loaded)) {
    return false;
  }
  entries_ = std::move(loaded);
  next_insertion_order_ = 1;
  for (const auto& entry : entries_) {
    if (entry.insertion_order >= next_insertion_order_ &&
        entry.insertion_order < std::numeric_limits<std::uint32_t>::max()) {
      next_insertion_order_ = entry.insertion_order + 1U;
    }
  }
  reset_requested_ = false;
  return true;
}

bool CredentialStore::valid_credential(std::string_view ssid, std::string_view password,
                                       std::string& error) {
  if (ssid.empty() || ssid.size() > kMaxSsidBytes) {
    error = "SSID must be 1-32 bytes";
    return false;
  }
  if (password.size() > kMaxPasswordBytes) {
    error = "password exceeds 64 bytes";
    return false;
  }
  return true;
}

std::vector<std::uint8_t> CredentialStore::encode(
    const std::vector<RememberedNetwork>& entries) {
  std::vector<std::uint8_t> bytes;
  bytes.reserve(kCredentialJournalMaxBytes);
  bytes.push_back(static_cast<std::uint8_t>(entries.size()));
  for (const auto& entry : entries) {
    if (entry.ssid.size() > kMaxSsidBytes || entry.password.size() > kMaxPasswordBytes) {
      return {};
    }
    bytes.push_back(static_cast<std::uint8_t>(entry.ssid.size()));
    bytes.push_back(static_cast<std::uint8_t>(entry.password.size()));
    bytes.push_back(entry.current ? 1U : 0U);
    put_u32(bytes, entry.insertion_order);
    bytes.insert(bytes.end(), entry.ssid.begin(), entry.ssid.end());
    bytes.insert(bytes.end(), entry.password.begin(), entry.password.end());
  }
  return bytes;
}

bool CredentialStore::decode(std::span<const std::uint8_t> bytes,
                             std::vector<RememberedNetwork>& entries) {
  entries.clear();
  if (bytes.empty() || bytes[0] > kMaxRememberedNetworks) {
    return bytes.empty();
  }
  std::size_t cursor = 1;
  bool current_seen = false;
  for (std::size_t index = 0; index < bytes[0]; ++index) {
    if (cursor + 7U > bytes.size()) {
      return false;
    }
    const auto ssid_size = bytes[cursor++];
    const auto password_size = bytes[cursor++];
    const bool current = bytes[cursor++] != 0U;
    const auto insertion_order = get_u32(bytes, cursor);
    cursor += 4U;
    if (ssid_size == 0U || ssid_size > kMaxSsidBytes || password_size > kMaxPasswordBytes ||
        cursor + ssid_size + password_size > bytes.size() || (current && current_seen)) {
      return false;
    }
    RememberedNetwork entry;
    entry.ssid.assign(reinterpret_cast<const char*>(bytes.data() + cursor), ssid_size);
    cursor += ssid_size;
    entry.password.assign(reinterpret_cast<const char*>(bytes.data() + cursor), password_size);
    cursor += password_size;
    entry.current = current;
    entry.insertion_order = insertion_order;
    entries.push_back(std::move(entry));
    current_seen = current_seen || current;
  }
  return cursor == bytes.size();
}

bool CredentialStore::persist(const std::vector<RememberedNetwork>& next) {
  if (next.size() > kMaxRememberedNetworks) {
    return false;
  }
  const auto bytes = encode(next);
  if (bytes.empty() && !next.empty()) {
    return false;
  }
  return !journal_.has_value() || journal_->commit(bytes);
}

CredentialMutationResult CredentialStore::add_or_update(std::string_view ssid,
                                                        std::string_view password) {
  CredentialMutationResult result;
  std::string error;
  if (!valid_credential(ssid, password, error)) {
    result.error = std::move(error);
    return result;
  }
  std::vector<RememberedNetwork> next = entries_;
  const auto found = std::find_if(next.begin(), next.end(), [ssid](const auto& entry) {
    return entry.ssid == ssid;
  });
  if (found != next.end()) {
    found->password.assign(password.data(), password.size());
    if (!persist(next)) {
      result.error = "credential journal write failed";
      return result;
    }
    entries_ = std::move(next);
    result.ok = true;
    result.kind = CredentialMutation::Updated;
    return result;
  }

  if (next.size() == kMaxRememberedNetworks) {
    const auto evict = std::min_element(
        next.begin(), next.end(), [](const auto& left, const auto& right) {
          if (left.current != right.current) {
            return !left.current;
          }
          return left.insertion_order < right.insertion_order;
        });
    if (evict == next.end() || evict->current) {
      result.error = "all credential slots are current";
      return result;
    }
    result.evicted_ssid = evict->ssid;
    next.erase(evict);
  }
  RememberedNetwork entry;
  entry.ssid.assign(ssid.data(), ssid.size());
  entry.password.assign(password.data(), password.size());
  entry.insertion_order = next_insertion_order_++;
  next.push_back(std::move(entry));
  if (!persist(next)) {
    --next_insertion_order_;
    result.error = "credential journal write failed";
    return result;
  }
  entries_ = std::move(next);
  result.ok = true;
  result.kind = CredentialMutation::Added;
  return result;
}

bool CredentialStore::mark_successful(std::string_view ssid) {
  std::vector<RememberedNetwork> next = entries_;
  bool found = false;
  for (auto& entry : next) {
    const bool selected = entry.ssid == ssid;
    entry.current = selected;
    found = found || selected;
  }
  if (!found || !persist(next)) {
    return false;
  }
  entries_ = std::move(next);
  return true;
}

CredentialMutationResult CredentialStore::factory_reset() {
  CredentialMutationResult result;
  const std::vector<RememberedNetwork> empty;
  if (!persist(empty)) {
    result.error = "credential journal reset failed";
    return result;
  }
  entries_.clear();
  next_insertion_order_ = 1;
  reset_requested_ = true;
  result.ok = true;
  result.kind = CredentialMutation::Reset;
  return result;
}

std::vector<RememberedNetwork> CredentialStore::fallback_sequence() const {
  auto result = entries_;
  std::stable_sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
    if (left.current != right.current) {
      return left.current;
    }
    return left.insertion_order < right.insertion_order;
  });
  return result;
}

std::optional<RememberedNetwork> CredentialStore::current() const {
  const auto found = std::find_if(entries_.begin(), entries_.end(),
                                  [](const auto& entry) { return entry.current; });
  if (found == entries_.end()) {
    return std::nullopt;
  }
  return *found;
}

std::optional<RememberedNetwork> CredentialStore::find(std::string_view ssid) const {
  const auto found = std::find_if(entries_.begin(), entries_.end(), [ssid](const auto& entry) {
    return entry.ssid == ssid;
  });
  if (found == entries_.end()) {
    return std::nullopt;
  }
  return *found;
}

}  // namespace t3::companion
