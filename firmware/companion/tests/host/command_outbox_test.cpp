#include "command_outbox.hpp"

#include <cstdlib>
#include <iostream>

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

DeviceCommand command(const char* command_id) {
  DeviceCommand result;
  result.type = DeviceCommandType::CancelTurn;
  result.command_id = id(command_id);
  result.device_id = id("device");
  result.context.environment_id = id("env");
  result.context.project_id = id("project");
  result.context.thread_id = id("thread");
  result.context.expected_turn_id = id("turn");
  return result;
}

void test_single_send_and_reconnect_reconciliation() {
  CommandOutbox outbox;
  const auto offline = outbox.enqueue(command("offline"), false);
  expect(!offline.accepted && offline.effects.size() == 1,
         "offline enqueue rejects without persisting a command");
  expect(offline.effects[0].kind == EffectKind::OfflineRejected,
         "offline rejection is declarative");

  const auto first = outbox.enqueue(command("command-1"), true);
  expect(first.accepted && first.effects.size() == 2, "online enqueue persists and sends once");
  expect(outbox.size() == 1, "online command is retained in outbox");
  const auto duplicate = outbox.enqueue(command("command-1"), true);
  expect(duplicate.accepted && duplicate.effects.empty(), "duplicate id is idempotent");

  CommandStatuses statuses;
  CommandStatusRecord record;
  record.command_id = id("command-1");
  record.status = CommandStatus::Committed;
  (void)record.updated_at.assign("later");
  (void)statuses.records.push(record);
  auto progress = outbox.reconcile(statuses);
  expect(progress.effects.empty(), "committed reconnect status has no glow");
  record.status = CommandStatus::Applied;
  statuses.records.items[0] = record;
  progress = outbox.reconcile(statuses);
  expect(has_effect(progress.effects, EffectKind::ShowAppliedGlow),
         "reconnect applied status produces success glow once");
  progress = outbox.reconcile(statuses);
  expect(!has_effect(progress.effects, EffectKind::ShowAppliedGlow),
         "duplicate applied status does not repeat success glow");
}

}  // namespace

int main() {
  test_single_send_and_reconnect_reconciliation();
  if (failures != 0) {
    std::cerr << failures << " command outbox test(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "T3 Companion command outbox tests passed\n";
  return EXIT_SUCCESS;
}
