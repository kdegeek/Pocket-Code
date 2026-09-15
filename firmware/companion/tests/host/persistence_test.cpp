#include "device_identity.hpp"
#include "pending_commands.hpp"
#include "snapshot_store.hpp"

#include <array>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace {

using namespace t3::companion;

int failures = 0;

void expect(bool condition, const char* message) {
  if (!condition) {
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
  }
}

class FakeSlots final : public SlotStorage {
 public:
  std::optional<std::vector<std::uint8_t>> read_slot(std::size_t slot) const override {
    return slots_[slot];
  }

  bool write_slot(std::size_t slot, std::span<const std::uint8_t> bytes) override {
    if (fail_next_write_) {
      fail_next_write_ = false;
      return false;
    }
    slots_[slot] = std::vector<std::uint8_t>(bytes.begin(), bytes.end());
    ++writes_;
    return true;
  }

  void fail_next_write() { fail_next_write_ = true; }
  std::size_t writes() const { return writes_; }
  void corrupt(std::size_t slot) {
    if (slots_[slot].has_value() && !slots_[slot]->empty()) {
      (*slots_[slot])[slots_[slot]->size() - 1] ^= 0x5a;
    }
  }

 private:
  std::array<std::optional<std::vector<std::uint8_t>>, 2> slots_;
  bool fail_next_write_ = false;
  std::size_t writes_ = 0;
};

Id id(const char* value) {
  Id result;
  (void)result.assign(value);
  return result;
}

void test_two_slot_crc_and_power_loss_atomicity() {
  FakeSlots slots;
  TwoSlotJournal journal(slots);
  const std::array<std::uint8_t, 3> first{1, 2, 3};
  expect(journal.commit(first), "first journal commit succeeds");
  const auto writes_after_first = slots.writes();
  expect(journal.commit(first), "identical journal commit is a no-op");
  expect(slots.writes() == writes_after_first, "wear limiting skips identical payload writes");
  slots.fail_next_write();
  const std::array<std::uint8_t, 2> second{4, 5};
  expect(!journal.commit(second), "failed alternate-slot write is reported");
  std::vector<std::uint8_t> loaded;
  expect(journal.load(loaded) && loaded == std::vector<std::uint8_t>(first.begin(), first.end()),
         "power loss leaves the previous valid slot intact");
  expect(journal.commit(second), "next journal commit succeeds after power loss");
  slots.corrupt(journal.active_slot());
  expect(journal.load(loaded) && loaded == std::vector<std::uint8_t>(first.begin(), first.end()),
         "CRC rejects the newest corrupt slot and falls back to the older slot");
}

void test_identity_and_pending_command_compact_state() {
  FakeSlots identity_slots;
  DeviceIdentityStore identity(identity_slots);
  DeviceIdentity value;
  value.device_id = "device-123";
  value.scoped_token = "companion-scoped-token";
  value.gateway_url = "https://gateway.example/ws";
  value.adapter_sequence = 42;
  expect(identity.save(value), "device identity persists atomically");
  DeviceIdentityStore reloaded(identity_slots);
  expect(reloaded.load() && reloaded.value().device_id == "device-123" &&
             reloaded.value().scoped_token == "companion-scoped-token" &&
             reloaded.value().gateway_url == value.gateway_url && reloaded.value().adapter_sequence == 42,
         "identity, scoped token, gateway, and cursor round-trip");

  FakeSlots command_slots;
  PendingCommandStore pending(command_slots);
  expect(pending.upsert(id("cmd-1"), CommandStatus::Committed), "pending command is persisted");
  expect(pending.upsert(id("cmd-1"), CommandStatus::Applied), "pending command status reconciles");
  expect(pending.upsert(id("cmd-2"), CommandStatus::Received), "second pending command is persisted");
  PendingCommandStore pending_reloaded(command_slots);
  expect(pending_reloaded.load() && pending_reloaded.find(id("cmd-1"))->status == CommandStatus::Applied &&
             pending_reloaded.find(id("cmd-2"))->status == CommandStatus::Received,
         "only command IDs and statuses are restored");
}

void test_credential_reset_and_add_are_atomic_at_storage_seam() {
  FakeSlots slots;
  CredentialStore store(slots);
  expect(store.add_or_update("home", "home-pass").ok, "credential journal stores an append");
  slots.fail_next_write();
  const auto reset = store.factory_reset();
  expect(!reset.ok && store.find("home").has_value(),
         "failed factory reset leaves in-memory credentials unchanged");
  CredentialStore reloaded(slots);
  expect(reloaded.load() && reloaded.find("home").has_value(),
         "failed factory reset leaves the previous durable slot intact");
}

void test_snapshot_store_strips_sensitive_and_prompt_data() {
  Snapshot snapshot;
  snapshot.adapter_sequence = 77;
  WorkItem work;
  work.environment_id = id("env");
  work.project_id = id("project");
  work.thread_id = id("thread");
  (void)work.project_name.assign("Project");
  (void)work.thread_name.assign("Thread");
  work.state = WorkItemState::Working;
  work.activity = ShortString{};
  (void)work.activity->assign("building");
  (void)snapshot.work_items.push(work);
  snapshot.automatic_focus_thread_id = id("thread");
  PendingRequest prompt;
  prompt.request_id = id("request");
  prompt.thread_id = id("thread");
  prompt.expected_turn_id = id("turn");
  prompt.detail = ShortString{};
  (void)prompt.detail->assign("do not persist this prompt");
  (void)snapshot.pending_requests.push(prompt);
  UsageProvider usage;
  usage.provider = Provider::Claude;
  RateWindow window;
  window.used_percent = 12.5;
  (void)window.reset_at.assign("2026-08-21T12:00:00Z");
  (void)usage.windows.push(window);
  (void)snapshot.usage.providers.push(usage);
  snapshot.connectivity.status = ConnectivityStatus::Connected;

  FakeSlots slots;
  SnapshotStore store(slots);
  expect(store.save(snapshot), "compact snapshot saves atomically");
  SnapshotStore reloaded(slots);
  expect(reloaded.load(), "compact snapshot reloads");
  const auto compact = reloaded.last_valid();
  expect(compact.has_value() && compact->adapter_sequence == 77 && compact->work_items.size() == 1,
         "last valid snapshot keeps only compact display state");
  expect(compact->pending_requests.empty(), "cached prompts are not persisted");
  expect(compact->work_items[0].activity.has_value() &&
             compact->work_items[0].activity->view() == "building",
         "compact work activity survives round-trip");
  expect(compact->usage.providers.size() == 1 && compact->usage.providers[0].windows.size() == 1,
         "provider rate windows survive without account or cost fields");
}

}  // namespace

int main() {
  test_two_slot_crc_and_power_loss_atomicity();
  test_identity_and_pending_command_compact_state();
  test_credential_reset_and_add_are_atomic_at_storage_seam();
  test_snapshot_store_strips_sensitive_and_prompt_data();
  if (failures != 0) {
    std::cerr << failures << " persistence test(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "T3 Companion persistence tests passed\n";
  return EXIT_SUCCESS;
}
