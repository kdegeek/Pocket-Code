#pragma once

#include "snapshot_model.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace t3::companion {

enum class FocusMode { Automatic, Browsing, PromptTakeover };

struct FocusQueueState {
  SnapshotModel model;
  FocusMode mode = FocusMode::Automatic;
  std::size_t browse_index = 0;
  std::uint64_t last_interaction_ms = 0;
};

enum class FocusEventKind {
  BrowseNext,
  BrowsePrevious,
  Interaction,
  Tick,
  SnapshotReplaced,
};

struct FocusEvent {
  FocusEventKind kind = FocusEventKind::Interaction;
  std::uint64_t now_ms = 0;
  std::optional<SnapshotModel> model;
};

struct FocusTransition {
  FocusQueueState state;
};

[[nodiscard]] FocusQueueState make_focus_queue(const SnapshotModel& model,
                                               std::uint64_t now_ms);
[[nodiscard]] FocusTransition reduce_focus(const FocusQueueState& current,
                                            const FocusEvent& event);
[[nodiscard]] std::optional<PendingRequest> active_request(const FocusQueueState& state);
[[nodiscard]] std::optional<Id> automatic_focus_thread(const FocusQueueState& state);
[[nodiscard]] std::optional<Id> focused_thread(const FocusQueueState& state);

}  // namespace t3::companion
