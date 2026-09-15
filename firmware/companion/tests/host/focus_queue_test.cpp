#include "focus_queue.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

using namespace t3::companion;

int failures = 0;

void expect(bool condition, const char* message) {
  if (!condition) {
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
  }
}

Id id(const char* value) {
  Id result;
  (void)result.assign(value);
  return result;
}

WorkItemRecord item(const char* thread, WorkItemState state, std::uint64_t updated) {
  WorkItemRecord result;
  result.item.thread_id = id(thread);
  result.item.state = state;
  result.authoritative_updated_ms = updated;
  return result;
}

PendingRequest request(const char* request_id, const char* thread, std::uint64_t created) {
  PendingRequest result;
  result.request_id = id(request_id);
  result.thread_id = id(thread);
  result.expected_turn_id = id("turn");
  (void)result.created_at.assign(std::to_string(created));
  result.kind = PendingRequestKind::Approval;
  return result;
}

void test_priority_tie_break_and_completion_retention() {
  SnapshotModel model;
  model.work_items = {
      item("idle", WorkItemState::Idle, 900),
      item("working", WorkItemState::Working, 800),
      item("completed-expired", WorkItemState::Completed, 0),
      item("blocked", WorkItemState::Blocked, 700),
      item("failed", WorkItemState::Failed, 600),
      item("completed", WorkItemState::Completed, 100),
      item("needs-b", WorkItemState::NeedsInput, 400),
      item("needs-a", WorkItemState::NeedsInput, 400),
  };

  const auto ordered = rank_work_items(model, 900'001);
  expect(ordered.size() == 7, "completed work is retained only for fifteen minutes");
  expect(ordered[0].item.thread_id.view() == "needs-a", "same-time needs-input ties use thread id");
  expect(ordered[1].item.thread_id.view() == "needs-b", "needs-input outranks all other work");
  expect(ordered[2].item.thread_id.view() == "blocked", "blocked and failed share the urgent rank");
  expect(ordered[3].item.thread_id.view() == "failed", "urgent ties use latest authoritative update");
  expect(ordered[4].item.thread_id.view() == "working", "working outranks recent completion");
  expect(ordered[5].item.thread_id.view() == "completed", "recent completion outranks idle");
  expect(ordered[6].item.thread_id.view() == "idle", "idle remains last");
}

void test_browse_returns_and_prompt_takeover() {
  SnapshotModel model;
  model.work_items = {
      item("urgent", WorkItemState::Failed, 100),
      item("other", WorkItemState::Working, 90),
  };
  FocusQueueState state = make_focus_queue(model, 1'000);
  expect(focused_thread(state).value().view() == "urgent", "automatic focus starts at highest priority");

  state = reduce_focus(state, FocusEvent{FocusEventKind::BrowseNext, 1'001, std::nullopt}).state;
  expect(focused_thread(state).value().view() == "other", "horizontal browsing is local");
  state = reduce_focus(state, FocusEvent{FocusEventKind::Tick, 31'001, std::nullopt}).state;
  expect(focused_thread(state).value().view() == "urgent", "browse returns after thirty seconds");

  state.model.pending_requests.push_back(request("request", "other", 1'002));
  state = reduce_focus(state, FocusEvent{FocusEventKind::SnapshotReplaced, 1'003, std::nullopt}).state;
  expect(active_request(state).has_value(), "new blocking request takes over immediately");
  expect(focused_thread(state).value().view() == "other", "takeover focuses the request owner");

  state.model.pending_requests.clear();
  state = reduce_focus(state, FocusEvent{FocusEventKind::SnapshotReplaced, 1'004, std::nullopt}).state;
  expect(!active_request(state).has_value(), "closing the last request exits takeover");
  expect(focused_thread(state).value().view() == "urgent", "closing prompt restores automatic focus");
}

}  // namespace

int main() {
  test_priority_tie_break_and_completion_retention();
  test_browse_returns_and_prompt_takeover();
  if (failures != 0) {
    std::cerr << failures << " focus queue test(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "T3 Companion focus queue tests passed\n";
  return EXIT_SUCCESS;
}
