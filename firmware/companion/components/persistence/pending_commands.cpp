#include "pending_commands.hpp"

#include <algorithm>

namespace t3::companion {
namespace {

void put_u16(std::vector<std::uint8_t>& bytes, std::uint16_t value) {
  bytes.push_back(static_cast<std::uint8_t>(value & 0xffU));
  bytes.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xffU));
}

[[nodiscard]] std::uint16_t get_u16(std::span<const std::uint8_t> bytes, std::size_t offset) {
  return static_cast<std::uint16_t>(bytes[offset]) |
         static_cast<std::uint16_t>(bytes[offset + 1U] << 8U);
}

[[nodiscard]] bool terminal(CommandStatus status) {
  return status == CommandStatus::Applied || status == CommandStatus::Rejected;
}

[[nodiscard]] int rank(CommandStatus status) {
  switch (status) {
    case CommandStatus::Received:
      return 0;
    case CommandStatus::Committed:
      return 1;
    case CommandStatus::Applied:
    case CommandStatus::Rejected:
      return 2;
  }
  return 0;
}

}  // namespace

PendingCommandStore::PendingCommandStore(SlotStorage& storage)
    : PendingCommandStore(&storage) {}

PendingCommandStore::PendingCommandStore(SlotStorage* storage) : storage_(storage) {
  if (storage_ != nullptr) {
    journal_.emplace(*storage_, kPendingCommandJournalMaxBytes);
    (void)load();
  }
}

bool PendingCommandStore::load() {
  if (!journal_.has_value()) {
    return true;
  }
  std::vector<std::uint8_t> bytes;
  if (!journal_->load(bytes)) {
    entries_.clear();
    return true;
  }
  return decode(bytes, entries_);
}

std::vector<std::uint8_t> PendingCommandStore::encode(
    const std::vector<PersistedCommand>& entries) {
  if (entries.size() > kMaxPersistedPendingCommands) {
    return {};
  }
  std::vector<std::uint8_t> bytes;
  bytes.reserve(kPendingCommandJournalMaxBytes);
  bytes.push_back(static_cast<std::uint8_t>(entries.size()));
  for (const auto& entry : entries) {
    if (entry.command_id.empty() || entry.command_id.view().size() > kMaxPersistedCommandIdBytes) {
      return {};
    }
    put_u16(bytes, static_cast<std::uint16_t>(entry.command_id.view().size()));
    bytes.push_back(static_cast<std::uint8_t>(entry.status));
    bytes.insert(bytes.end(), entry.command_id.view().begin(), entry.command_id.view().end());
  }
  return bytes;
}

bool PendingCommandStore::decode(std::span<const std::uint8_t> bytes,
                                 std::vector<PersistedCommand>& entries) {
  entries.clear();
  if (bytes.empty() || bytes[0] > kMaxPersistedPendingCommands) {
    return bytes.empty();
  }
  std::size_t cursor = 1;
  for (std::size_t index = 0; index < bytes[0]; ++index) {
    if (cursor + 3U > bytes.size()) {
      return false;
    }
    const auto id_size = get_u16(bytes, cursor);
    cursor += 2U;
    const auto raw_status = bytes[cursor++];
    if (id_size == 0U || id_size > kMaxPersistedCommandIdBytes || cursor + id_size > bytes.size() ||
        raw_status > static_cast<std::uint8_t>(CommandStatus::Rejected)) {
      return false;
    }
    PersistedCommand entry;
    if (!entry.command_id.assign(std::string_view(
            reinterpret_cast<const char*>(bytes.data() + cursor), id_size))) {
      return false;
    }
    cursor += id_size;
    entry.status = static_cast<CommandStatus>(raw_status);
    entries.push_back(std::move(entry));
  }
  return cursor == bytes.size();
}

bool PendingCommandStore::persist(const std::vector<PersistedCommand>& entries) {
  const auto bytes = encode(entries);
  if (bytes.empty() && !entries.empty()) {
    return false;
  }
  return !journal_.has_value() || journal_->commit(bytes);
}

bool PendingCommandStore::upsert(const Id& command_id, CommandStatus status) {
  if (command_id.empty() || command_id.view().size() > kMaxPersistedCommandIdBytes) {
    return false;
  }
  auto next = entries_;
  const auto found = std::find_if(next.begin(), next.end(), [&command_id](const auto& entry) {
    return entry.command_id == command_id;
  });
  if (found != next.end()) {
    if (terminal(found->status) || rank(status) < rank(found->status)) {
      return true;
    }
    found->status = status;
  } else {
    if (next.size() >= kMaxPersistedPendingCommands) {
      return false;
    }
    next.push_back(PersistedCommand{command_id, status});
  }
  if (!persist(next)) {
    return false;
  }
  entries_ = std::move(next);
  return true;
}

bool PendingCommandStore::erase(const Id& command_id) {
  auto next = entries_;
  const auto found = std::remove_if(next.begin(), next.end(), [&command_id](const auto& entry) {
    return entry.command_id == command_id;
  });
  if (found == next.end()) {
    return true;
  }
  next.erase(found, next.end());
  if (!persist(next)) {
    return false;
  }
  entries_ = std::move(next);
  return true;
}

bool PendingCommandStore::clear() {
  const std::vector<PersistedCommand> next;
  if (!persist(next)) {
    return false;
  }
  entries_.clear();
  return true;
}

std::optional<PersistedCommand> PendingCommandStore::find(const Id& command_id) const {
  const auto found = std::find_if(entries_.begin(), entries_.end(), [&command_id](const auto& entry) {
    return entry.command_id == command_id;
  });
  if (found == entries_.end()) {
    return std::nullopt;
  }
  return *found;
}

}  // namespace t3::companion
