#pragma once

#include "credential_store.hpp"
#include "types.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

namespace t3::companion {

// Pending command persistence intentionally stores only IDs and their durable
// lifecycle status. Thirty-two IDs bounded to 64 bytes plus one status byte fit
// in a compact journal record in the shared NVS budget.
inline constexpr std::size_t kMaxPersistedPendingCommands = 32;
inline constexpr std::size_t kMaxPersistedCommandIdBytes = 64;
inline constexpr std::size_t kPendingCommandJournalMaxBytes = 2'560;

struct PersistedCommand {
  Id command_id;
  CommandStatus status = CommandStatus::Received;
};

class PendingCommandStore {
 public:
  explicit PendingCommandStore(SlotStorage& storage);
  explicit PendingCommandStore(SlotStorage* storage = nullptr);

  [[nodiscard]] bool load();
  [[nodiscard]] bool upsert(const Id& command_id, CommandStatus status);
  [[nodiscard]] bool erase(const Id& command_id);
  [[nodiscard]] bool clear();
  [[nodiscard]] const std::vector<PersistedCommand>& entries() const { return entries_; }
  [[nodiscard]] std::optional<PersistedCommand> find(const Id& command_id) const;

 private:
  [[nodiscard]] bool persist(const std::vector<PersistedCommand>& entries);
  [[nodiscard]] static std::vector<std::uint8_t> encode(
      const std::vector<PersistedCommand>& entries);
  [[nodiscard]] static bool decode(std::span<const std::uint8_t> bytes,
                                   std::vector<PersistedCommand>& entries);

  SlotStorage* storage_ = nullptr;
  std::optional<TwoSlotJournal> journal_;
  std::vector<PersistedCommand> entries_;
};

}  // namespace t3::companion

namespace t3::companion::persistence {
using ::t3::companion::PendingCommandStore;
using ::t3::companion::PersistedCommand;
}  // namespace t3::companion::persistence
