#include "focus_queue.hpp"

#include <algorithm>

namespace t3::companion {
namespace {

std::optional<std::size_t> find_thread(const std::vector<WorkItemRecord>& ranked,
                                       std::string_view thread_id) {
  for (std::size_t index = 0; index < ranked.size(); ++index) {
    if (ranked[index].item.thread_id.view() == thread_id) {
      return index;
    }
  }
  return std::nullopt;
}

void apply_snapshot(FocusQueueState& state, const SnapshotModel& model, std::uint64_t now_ms) {
  std::optional<Id> previous_thread;
  if (state.mode == FocusMode::Browsing) {
    const auto previous = rank_work_items(state.model, state.model.now_ms);
    if (state.browse_index < previous.size()) {
      previous_thread = previous[state.browse_index].item.thread_id;
    }
  }
  state.model = model;
  state.model.now_ms = now_ms;
  const auto ranked = rank_work_items(state.model, now_ms);
  const auto pending = rank_pending_requests(state.model, now_ms);
  if (!pending.empty()) {
    state.mode = FocusMode::PromptTakeover;
    state.browse_index = 0;
    state.last_interaction_ms = now_ms;
    return;
  }
  if (state.mode == FocusMode::Browsing && previous_thread.has_value()) {
    const auto index = find_thread(ranked, previous_thread->view());
    if (index.has_value()) {
      state.browse_index = *index;
      state.last_interaction_ms = now_ms;
      return;
    }
  }
  state.mode = FocusMode::Automatic;
  state.browse_index = 0;
  state.last_interaction_ms = now_ms;
}

}  // namespace

FocusQueueState make_focus_queue(const SnapshotModel& model, std::uint64_t now_ms) {
  FocusQueueState state;
  state.model = model;
  state.model.now_ms = now_ms;
  state.last_interaction_ms = now_ms;
  if (!rank_pending_requests(state.model, now_ms).empty()) {
    state.mode = FocusMode::PromptTakeover;
  }
  return state;
}

FocusTransition reduce_focus(const FocusQueueState& current, const FocusEvent& event) {
  FocusTransition transition{current};
  auto& state = transition.state;
  if (event.model.has_value()) {
    apply_snapshot(state, *event.model, event.now_ms);
  } else if (event.kind == FocusEventKind::SnapshotReplaced) {
    // A caller may atomically update the value model before dispatching this
    // event.  Re-run focus selection without consulting any external clock.
    const SnapshotModel model = state.model;
    apply_snapshot(state, model, event.now_ms);
  }
  state.model.now_ms = event.now_ms;

  const auto ranked = rank_work_items(state.model, event.now_ms);
  const auto pending = rank_pending_requests(state.model, event.now_ms);
  if (!pending.empty()) {
    state.mode = FocusMode::PromptTakeover;
    state.browse_index = 0;
    state.last_interaction_ms = event.now_ms;
    return transition;
  }
  if (state.mode == FocusMode::PromptTakeover) {
    state.mode = FocusMode::Automatic;
    state.browse_index = 0;
  }

  switch (event.kind) {
    case FocusEventKind::BrowseNext:
      if (!ranked.empty()) {
        state.browse_index = state.mode == FocusMode::Browsing
                                 ? (state.browse_index + 1U) % ranked.size()
                                 : (ranked.size() > 1U ? 1U : 0U);
        state.mode = FocusMode::Browsing;
        state.last_interaction_ms = event.now_ms;
      }
      break;
    case FocusEventKind::BrowsePrevious:
      if (!ranked.empty()) {
        if (state.mode != FocusMode::Browsing) {
          state.browse_index = ranked.size() > 1U ? ranked.size() - 1U : 0U;
        } else {
          state.browse_index = state.browse_index == 0U ? ranked.size() - 1U
                                                        : state.browse_index - 1U;
        }
        state.mode = FocusMode::Browsing;
        state.last_interaction_ms = event.now_ms;
      }
      break;
    case FocusEventKind::Interaction:
      state.last_interaction_ms = event.now_ms;
      break;
    case FocusEventKind::Tick:
      if (state.mode == FocusMode::Browsing && event.now_ms >= state.last_interaction_ms &&
          event.now_ms - state.last_interaction_ms >= kBrowseReturnMs) {
        state.mode = FocusMode::Automatic;
        state.browse_index = 0;
      }
      break;
    case FocusEventKind::SnapshotReplaced:
      break;
  }
  return transition;
}

std::optional<PendingRequest> active_request(const FocusQueueState& state) {
  const auto pending = rank_pending_requests(state.model, state.model.now_ms);
  if (pending.empty()) {
    return std::nullopt;
  }
  return pending.front();
}

std::optional<Id> automatic_focus_thread(const FocusQueueState& state) {
  const auto ranked = rank_work_items(state.model, state.model.now_ms);
  if (ranked.empty()) {
    return std::nullopt;
  }
  return ranked.front().item.thread_id;
}

std::optional<Id> focused_thread(const FocusQueueState& state) {
  if (const auto pending = active_request(state); pending.has_value()) {
    return pending->thread_id;
  }
  const auto ranked = rank_work_items(state.model, state.model.now_ms);
  if (ranked.empty()) {
    return std::nullopt;
  }
  if (state.mode == FocusMode::Browsing && state.browse_index < ranked.size()) {
    return ranked[state.browse_index].item.thread_id;
  }
  return ranked.front().item.thread_id;
}

}  // namespace t3::companion
