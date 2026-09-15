#pragma once

#include "types.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace t3::companion {

inline constexpr std::uint64_t kCompletionRetentionMs = 15ULL * 60ULL * 1'000ULL;
inline constexpr std::uint64_t kBrowseReturnMs = 30ULL * 1'000ULL;

// The protocol carries an RFC3339 string.  The adapter's authoritative update
// sequence is kept separately so ordering and retention never depend on a
// device wall clock or on parsing a display string.
struct WorkItemRecord {
  WorkItem item;
  std::uint64_t authoritative_updated_ms = 0;
};

struct SnapshotModel {
  std::vector<WorkItemRecord> work_items;
  std::vector<PendingRequest> pending_requests;
  std::optional<Id> automatic_focus_thread_id;
  Connectivity connectivity;
  std::uint64_t adapter_sequence = 0;
  std::uint64_t now_ms = 0;
  bool live_handshake = false;
};

[[nodiscard]] std::uint64_t parse_datetime_ms(std::string_view value);
[[nodiscard]] int work_item_priority(const WorkItemRecord& item, std::uint64_t now_ms);
[[nodiscard]] bool retained(const WorkItemRecord& item, std::uint64_t now_ms);
[[nodiscard]] std::vector<WorkItemRecord> rank_work_items(const SnapshotModel& model,
                                                           std::uint64_t now_ms);
[[nodiscard]] std::vector<PendingRequest> rank_pending_requests(const SnapshotModel& model,
                                                                std::uint64_t now_ms);
[[nodiscard]] SnapshotModel project_snapshot(const Snapshot& snapshot,
                                              std::uint64_t now_ms,
                                              std::span<const std::uint64_t> authoritative_updates = {});

}  // namespace t3::companion
