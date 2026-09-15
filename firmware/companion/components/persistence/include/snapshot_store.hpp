#pragma once

#include "credential_store.hpp"
#include "types.hpp"

#include <optional>
#include <span>
#include <vector>

namespace t3::companion {

inline constexpr std::size_t kMaxPersistedWorkItems = 8;
inline constexpr std::size_t kMaxPersistedProviders = kMaxUsageProviders;
inline constexpr std::size_t kMaxPersistedWindows = kMaxUsageWindows;
inline constexpr std::size_t kMaxPersistedSnapshotTextBytes = 96;
inline constexpr std::size_t kSnapshotJournalMaxBytes = 8'192;

// Compact display projection. Pending requests, prompt options/answers,
// transcripts, audio, provider account data, and cost data deliberately have
// no fields here and cannot be serialized by SnapshotStore.
struct CompactWorkItem {
  Id environment_id;
  Id project_id;
  Id thread_id;
  std::optional<Id> turn_id;
  ShortString project_name;
  ShortString thread_name;
  std::optional<Provider> provider;
  WorkItemState state = WorkItemState::Idle;
  std::optional<ShortString> activity;
  DateTime updated_at;
};

struct CompactUsageProvider {
  Provider provider = Provider::Codex;
  UsageStatus status = UsageStatus::Unavailable;
  std::vector<RateWindow> windows;
  DateTime observed_at;
};

struct CompactUsageSnapshot {
  std::vector<CompactUsageProvider> providers;
};

struct CompactSnapshot {
  std::uint8_t protocol_version = kProtocolVersion;
  std::uint64_t adapter_sequence = 0;
  std::vector<CompactWorkItem> work_items;
  std::optional<Id> automatic_focus_thread_id;
  CompactUsageSnapshot usage;
  ConnectivityStatus connectivity = ConnectivityStatus::Offline;
  std::optional<DateTime> last_seen_at;

  // Always empty; retained as a visible invariant for callers that project a
  // cached snapshot into the live model. It is never read from or written to
  // durable storage.
  std::vector<PendingRequest> pending_requests;
};

class SnapshotStore {
 public:
  explicit SnapshotStore(SlotStorage& storage);
  explicit SnapshotStore(SlotStorage* storage = nullptr);

  [[nodiscard]] bool load();
  [[nodiscard]] bool save(const Snapshot& snapshot);
  [[nodiscard]] bool save(const CompactSnapshot& snapshot);
  [[nodiscard]] std::optional<CompactSnapshot> last_valid() const { return last_valid_; }

 private:
  [[nodiscard]] static bool project(const Snapshot& snapshot, CompactSnapshot& output);
  [[nodiscard]] static std::vector<std::uint8_t> encode(const CompactSnapshot& snapshot);
  [[nodiscard]] static bool decode(std::span<const std::uint8_t> bytes,
                                   CompactSnapshot& snapshot);

  SlotStorage* storage_ = nullptr;
  std::optional<TwoSlotJournal> journal_;
  std::optional<CompactSnapshot> last_valid_;
};

}  // namespace t3::companion

namespace t3::companion::persistence {
using ::t3::companion::CompactSnapshot;
using ::t3::companion::CompactUsageProvider;
using ::t3::companion::CompactUsageSnapshot;
using ::t3::companion::CompactWorkItem;
using ::t3::companion::SnapshotStore;
}  // namespace t3::companion::persistence
