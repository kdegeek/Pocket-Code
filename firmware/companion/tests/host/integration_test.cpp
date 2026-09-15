#include "audio_frame.hpp"
#include "backpressure.hpp"
#include "board_manifest.h"
#include "command_outbox.hpp"
#include "credential_store.hpp"
#include "envelope.hpp"
#include "es7210_capture.hpp"
#include "focus_queue.hpp"
#include "interaction_fsm.hpp"
#include "manifest.hpp"
#include "pending_commands.hpp"
#include "ptt_button.hpp"
#include "rollback.hpp"
#include "sleep_lock.hpp"
#include "snapshot_model.hpp"
#include "snapshot_store.hpp"
#include "softap_portal.hpp"
#include "transport.hpp"

#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
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

Id id(std::string_view value) {
  Id result;
  (void)result.assign(value);
  return result;
}

std::string read_fixture(const char* name) {
  std::ifstream input(std::filesystem::path(COMPANION_FIXTURE_DIR) / name,
                      std::ios::binary);
  std::ostringstream bytes;
  bytes << input.rdbuf();
  return bytes.str();
}

class FakeSlots final : public SlotStorage {
 public:
  std::optional<std::vector<std::uint8_t>> read_slot(std::size_t slot) const override {
    return slots_[slot];
  }

  bool write_slot(std::size_t slot, std::span<const std::uint8_t> bytes) override {
    slots_[slot] = std::vector<std::uint8_t>(bytes.begin(), bytes.end());
    return true;
  }

 private:
  std::array<std::optional<std::vector<std::uint8_t>>, 2> slots_;
};

class FakeWebSocket final : public WebSocketClient {
 public:
  bool open(std::string_view url, std::string_view bearer_token) override {
    opened_urls.emplace_back(url);
    opened_tokens.emplace_back(bearer_token);
    return open_result;
  }

  bool send_text(std::string_view frame) override {
    text_frames.emplace_back(frame);
    return send_result;
  }

  bool send_binary(std::span<const std::uint8_t> frame) override {
    binary_frames.emplace_back(frame.begin(), frame.end());
    return send_result;
  }

  void close() override { closed = true; }

  bool open_result = true;
  bool send_result = true;
  bool closed = false;
  std::vector<std::string> opened_urls;
  std::vector<std::string> opened_tokens;
  std::vector<std::string> text_frames;
  std::vector<std::vector<std::uint8_t>> binary_frames;
};

class FakeHttp final : public HttpClient {
 public:
  HttpResponse get(std::string_view url, std::string_view bearer_token) override {
    requested_url = std::string(url);
    requested_token = std::string(bearer_token);
    ++requests;
    return response;
  }

  HttpResponse response;
  std::string requested_url;
  std::string requested_token;
  std::size_t requests = 0;
};

PendingRequest approval_request(const Snapshot& snapshot) {
  return snapshot.pending_requests[0];
}

PendingRequest choice_request(const Snapshot& snapshot) {
  return snapshot.pending_requests[1];
}

DeviceCommand cancel_command(const Id& command_id, const Id& device_id) {
  DeviceCommand command;
  command.type = DeviceCommandType::CancelTurn;
  command.command_id = command_id;
  command.device_id = device_id;
  command.context.environment_id = id("environment-fixture");
  command.context.project_id = id("project-fixture");
  command.context.thread_id = id("thread-needs-input");
  command.context.expected_turn_id = id("turn-needs-input");
  return command;
}

OtaManifest ota_fixture_manifest() {
  OtaManifest manifest;
  manifest.schema_version = 1;
  manifest.version = 12;
  manifest.protocol_min = 1;
  manifest.protocol_max = 1;
  manifest.chip = "esp32s3";
  manifest.flash_size_bytes = 0x1000000;
  manifest.slot_size_bytes = 0x700000;
  manifest.image_size_bytes = 3;
  manifest.storage_offset = 0xe20000;
  manifest.storage_size_bytes = 0x1d0000;
  manifest.image_sha256 = sha256_hex(std::span<const std::uint8_t>{
      reinterpret_cast<const std::uint8_t*>("ota"), 3});
  manifest.key_id = "fixture-v1";
  manifest.signature = FixtureSignatureVerifier::signature_for_test(
      manifest.signed_payload());
  return manifest;
}

OtaDeviceProfile ota_fixture_device() {
  OtaDeviceProfile device;
  device.chip = "esp32s3";
  device.flash_size_bytes = 0x1000000;
  device.protocol_min = 1;
  device.protocol_max = 1;
  device.current_version = 11;
  device.storage_offset = 0xe20000;
  device.storage_size_bytes = 0x1d0000;
  return device;
}

void test_fixture_runner_and_decoding() {
  const std::string command = std::string("\"") + COMPANION_FIXTURE_RUNNER + "\" \"" +
                              COMPANION_FIXTURE_DIR + "\" --acceptance >/dev/null 2>&1";
  expect(std::system(command.c_str()) == 0,
         "fixture runner accepts the deterministic integrated fixture contract");

  const auto hello = decode_hello(read_fixture("hello.json"));
  const auto snapshot = decode_snapshot(read_fixture("snapshot.json"));
  const auto pending = decode_pending_requests(read_fixture("pending-requests.json"));
  const auto statuses = decode_command_statuses(read_fixture("command-statuses.json"));
  expect(hello.ok() && hello.value.device_id == id("device-fixture-01"),
         "canonical hello is available to the integrated host flow");
  expect(snapshot.ok() && snapshot.value.adapter_sequence == 7U,
         "canonical snapshot carries the expected replay cursor");
  expect(pending.ok() && pending.value.count == 2U,
         "canonical pending-request fixture carries simultaneous prompts");
  expect(statuses.ok() && statuses.value.records[2].status == CommandStatus::Applied,
         "canonical status fixture carries an authoritative applied result");
}

void test_cached_boot_enrollment_wifi_and_live_snapshot() {
  const auto decoded_snapshot = decode_snapshot(read_fixture("snapshot.json"));
  expect(decoded_snapshot.ok(), "snapshot decodes for cached boot setup");
  if (!decoded_snapshot.ok()) return;

  FakeSlots identity_slots;
  FakeSlots snapshot_slots;
  FakeSlots pending_slots;
  FakeSlots network_slots;
  DeviceIdentityStore identity_store(identity_slots);
  SnapshotStore snapshot_store(snapshot_slots);
  PendingCommandStore pending_store(pending_slots);
  CredentialStore networks(network_slots);

  DeviceIdentity identity;
  identity.device_id = "device-fixture-01";
  identity.scoped_token = "cached-token";
  identity.gateway_url = "http://gateway.local";
  expect(identity_store.save(identity), "cached identity persists before boot");
  expect(snapshot_store.save(decoded_snapshot.value), "cached compact snapshot persists before boot");

  expect(networks.add_or_update("home", "home-password").ok,
         "remembered home Wi-Fi is retained before provisioning append");
  SoftApPortal portal(networks, 0x1a2b);
  expect(portal.begin(true) && portal.station_preserved(),
         "enrollment AP preserves the current station until explicit join");
  expect(portal.request_setup_join() && portal.join_setup_ap(),
         "enrollment AP join is an explicit transition");
  expect(portal.append_network("hotspot", "hotspot-password") && portal.complete(),
         "enrollment appends a second Wi-Fi credential");
  expect(networks.size() == 2U && networks.find("home").has_value() &&
             networks.find("hotspot").has_value(),
         "Wi-Fi append never replaces the existing remembered network");

  FakeWebSocket websocket;
  FakeHttp http;
  http.response = {200, read_fixture("snapshot.json")};
  std::uint64_t now_ms = 12'000;
  Transport transport({&identity_store, &pending_store, &snapshot_store, nullptr,
                       &websocket, &http, nullptr, [&now_ms]() { return now_ms; }});
  expect(transport.configure(TransportSecurity::TrustedLan).ok(),
         "cached boot configures the dedicated trusted-LAN routes");
  expect(transport.load_cache().ok() && transport.cached_prompts_display_only(),
         "cached boot renders prompts display-only before a live handshake");

  PairingTokenStore pairing;
  expect(pairing.put_pairing("enrollment-1", "123456", now_ms + 10'000) &&
             pairing.consume_pairing("enrollment-1", "123456", now_ms),
         "enrollment pairing is one-time and expires by the host clock");
  expect(transport.start_enrollment().ok() && !websocket.opened_urls.empty() &&
             websocket.opened_urls.back() ==
                 "ws://gateway.local/api/companion/v1/enroll?deviceId=device-fixture-01",
         "enrollment uses only the dedicated companion route with device identity");
  expect(transport.accept_enrollment_credential("scoped-token", now_ms + 30'000).ok() &&
             identity_store.value().scoped_token == "scoped-token",
         "enrollment persists only the scoped companion credential");

  expect(transport.connect().ok() && transport.on_socket_open().ok(),
         "live connect sends the authenticated companion hello");
  expect(!websocket.text_frames.empty() && decode_hello(websocket.text_frames.back()).ok(),
         "live hello is a bounded v1 message");
  HelloAck ack;
  ack.session_id = id("session-1");
  ack.resume_replay = false;
  expect(transport.on_hello_ack(ack).ok() && transport.gestures_enabled(),
         "hello acknowledgement reconciles status before enabling gestures");
  expect(transport.request_snapshot().ok() && transport.live_snapshot().has_value(),
         "snapshot is fetched only after the authenticated hello");
  expect(http.requested_url == "http://gateway.local/api/companion/v1/snapshot" &&
             http.requested_token == "scoped-token",
         "snapshot request uses the dedicated route and scoped token");
}

void test_focus_prompt_commands_and_reconnect() {
  const auto decoded_snapshot = decode_snapshot(read_fixture("snapshot.json"));
  const auto decoded_statuses = decode_command_statuses(read_fixture("command-statuses.json"));
  expect(decoded_snapshot.ok() && decoded_statuses.ok(),
         "focus and command fixtures decode before integration");
  if (!decoded_snapshot.ok() || !decoded_statuses.ok()) return;

  const auto model = project_snapshot(decoded_snapshot.value, 1'000'000);
  const auto ranked = rank_work_items(model, model.now_ms);
  expect(ranked.size() == 6U && ranked.front().item.thread_id == id("thread-needs-input") &&
             ranked[1].item.thread_id == id("thread-failed"),
         "ambient focus follows needs-input then failed priority");

  SnapshotModel ambient = model;
  ambient.pending_requests.clear();
  auto focus = make_focus_queue(ambient, 1'000'000);
  expect(focus.mode == FocusMode::Automatic &&
             focused_thread(focus) == id("thread-needs-input"),
         "cached ambient focus starts on the highest-priority work item");
  const auto takeover = reduce_focus(
      focus, FocusEvent{FocusEventKind::SnapshotReplaced, 1'000'001, model});
  expect(takeover.state.mode == FocusMode::PromptTakeover && active_request(takeover.state).has_value(),
         "a live blocking request immediately takes over the ambient center");

  const auto approval = approval_request(decoded_snapshot.value);
  InteractionState interaction = make_interaction_state(true, "device-fixture-01");
  interaction = reduce_interaction(interaction, InteractionEvent::open(approval, 1'000'010)).state;
  auto accepted = reduce_interaction(
      interaction, InteractionEvent::gesture(Gesture::SwipeRight, 1'000'011));
  expect(accepted.state.mode == InteractionMode::AwaitingAck &&
             has_effect(accepted.effects, EffectKind::SendCommand),
         "approval swipe creates one context-bound command");
  const Id approval_command_id = accepted.state.active_command_id.value_or(Id{});
  auto committed = reduce_interaction(
      accepted.state, InteractionEvent::command_status(approval_command_id,
                                                       CommandStatus::Committed, 1'000'012));
  expect(!has_effect(committed.effects, EffectKind::ShowAppliedGlow),
         "committed acknowledgement does not show a success glow");
  auto applied = reduce_interaction(
      committed.state, InteractionEvent::command_status(approval_command_id,
                                                        CommandStatus::Applied, 1'000'013));
  expect(has_effect(applied.effects, EffectKind::ShowAppliedGlow) &&
             applied.state.mode == InteractionMode::AppliedGlow,
         "success glow appears only after authoritative applied acknowledgement");
  InteractionEvent glow_tick;
  glow_tick.kind = InteractionEventKind::Tick;
  glow_tick.now_ms = 1'000'814;
  const auto ambient_again = reduce_interaction(applied.state, glow_tick);
  expect(ambient_again.state.mode == InteractionMode::Ambient,
         "applied glow returns to ambient after its bounded duration");

  const auto choice = choice_request(decoded_snapshot.value);
  InteractionState choice_state = make_interaction_state(true, "device-fixture-01");
  choice_state = reduce_interaction(choice_state, InteractionEvent::open(choice, 1'000'020)).state;
  choice_state = reduce_interaction(
      choice_state, InteractionEvent::gesture(Gesture::SwipeUp, 1'000'021)).state;
  choice_state = reduce_interaction(
      choice_state, InteractionEvent::gesture(Gesture::SwipeUp, 1'000'022)).state;
  choice_state = reduce_interaction(
      choice_state, InteractionEvent::gesture(Gesture::SwipeRight, 1'000'023)).state;
  choice_state = reduce_interaction(
      choice_state, InteractionEvent::gesture(Gesture::SwipeRight, 1'000'024)).state;
  const auto choice_submit = reduce_interaction(
      choice_state, InteractionEvent::gesture(Gesture::SwipeUp, 1'000'025));
  expect(choice_submit.state.mode == InteractionMode::AwaitingAck &&
             has_effect(choice_submit.effects, EffectKind::SendCommand),
         "bounded choice and multi-select answers submit through the same outbox");
  bool choice_payload = false;
  for (const auto& effect : choice_submit.effects) {
    if (effect.kind == EffectKind::SendCommand && effect.command.has_value()) {
      choice_payload = effect.command->payload_kind == DeviceCommandPayload::Answers &&
                       effect.command->answers.entries.count == 2U;
    }
  }
  expect(choice_payload, "choice submission preserves both ordered answer records");

  InteractionState cancel_state = make_interaction_state(true, "device-fixture-01");
  cancel_state = reduce_interaction(cancel_state, InteractionEvent::open(approval, 1'000'030)).state;
  const auto cancelled = reduce_interaction(
      cancel_state, InteractionEvent::gesture(Gesture::SwipeDown, 1'000'031));
  expect(cancelled.state.mode == InteractionMode::AwaitingAck &&
             has_effect(cancelled.effects, EffectKind::SendCommand),
         "cancel gesture emits a context-bound cancel command");

  FakeSlots identity_slots;
  FakeSlots pending_slots;
  FakeSlots snapshot_slots;
  DeviceIdentityStore identity_store(identity_slots);
  PendingCommandStore pending_store(pending_slots);
  SnapshotStore snapshot_store(snapshot_slots);
  CommandOutbox outbox;
  DeviceIdentity identity;
  identity.device_id = "device-fixture-01";
  identity.scoped_token = "scoped-token";
  identity.gateway_url = "http://gateway.local";
  expect(identity_store.save(identity), "reconnect identity persists");
  FakeWebSocket websocket;
  FakeHttp http;
  std::uint64_t now_ms = 2'000'000;
  Transport transport({&identity_store, &pending_store, &snapshot_store, &outbox,
                       &websocket, &http, nullptr, [&now_ms]() { return now_ms; }});
  expect(transport.configure(TransportSecurity::TrustedLan).ok() && transport.connect().ok() &&
             transport.on_socket_open().ok(),
         "reconnect fixture establishes its first live session");
  HelloAck ack;
  ack.session_id = id("session-reconnect-1");
  ack.resume_replay = true;
  expect(transport.on_hello_ack(ack).ok(), "first reconnect hello is reconciled");
  const auto reconnect_id = decoded_statuses.value.records[2].command_id;
  const auto command = cancel_command(reconnect_id, id("device-fixture-01"));
  expect(transport.submit_command(command).ok(),
         "a transmitted command is persisted before a gateway loss");
  expect(transport.on_gateway_lost().ok() && !transport.gestures_enabled(),
         "gateway loss disables mutating gestures without deleting the pending command");
  expect(transport.connect().ok() && transport.on_socket_open().ok(),
         "reconnect sends a fresh hello without replaying the command");
  HelloAck reconciled;
  reconciled.session_id = id("session-reconnect-2");
  reconciled.resume_replay = true;
  reconciled.pending_commands = decoded_statuses.value;
  expect(transport.on_hello_ack(reconciled).ok() && transport.gestures_enabled(),
         "reconnect hello reconciles authoritative command statuses");
  expect(pending_store.find(reconnect_id).has_value() &&
             pending_store.find(reconnect_id)->status == CommandStatus::Applied &&
             outbox.find(reconnect_id).has_value() &&
             outbox.find(reconnect_id)->status == CommandStatus::Applied,
         "applied status is durable and is not resent after reconnect");
}

void test_fake_ptt_audio_transcript_and_lock() {
  auto unresolved = t3::board::verified_manifest().audio;
  unresolved.front_mic_channels_verified = false;
  unresolved.mic_unresolved = true;
  Es7210Capture blocked(unresolved);
  expect(!blocked.available() && blocked.start() == CaptureResult::Unavailable,
         "physical audio remains fail-closed while Task 2 mic mapping is unresolved");

  auto fixture_audio = unresolved;
  fixture_audio.front_mic_channels_verified = true;
  fixture_audio.mic_unresolved = false;
  Es7210Capture capture(fixture_audio);
  expect(capture.available() && capture.start() == CaptureResult::Started,
         "fake host audio requires an explicit fixture-only verified mapping");
  const std::array<std::int16_t, 4> stereo = {100, 300, -100, -300};
  std::vector<std::int16_t> mono;
  expect(capture.push_stereo(stereo, mono) == CaptureResult::Accepted &&
             mono == std::vector<std::int16_t>({200, -200}),
         "fake STD stereo audio downmixes verified channels");
  AudioStreamId stream_id;
  stream_id.bytes[0] = 0x2aU;
  AudioStreamSession stream({stream_id, 4, 1024});
  const auto pcm = encode_pcm_s16le_24khz_mono(mono);
  expect(stream.start() == StreamResult::Started &&
             stream.push(stream_id, pcm) == StreamResult::Accepted,
         "fake PTT audio enters the bounded ephemeral stream");
  const auto frame = stream.pop_frame();
  expect(frame.has_value() && decode_audio_frame(*frame, 1024).ok() &&
             decode_audio_frame(*frame, 1024).value.header.chunk_number == 0U,
         "fake audio produces a monotonic v1 binary frame");
  expect(capture.stop() == CaptureResult::Stopped && stream.stop() == StreamResult::Stopped,
         "release clears the host capture and stream");

  PttButtonController ptt(PttContext{false, true, true, false});
  const auto started = ptt.on_button(
      ButtonEvent{ButtonId::UpperRight, ButtonEventKind::LongHoldStart, 3'000'000});
  const auto stopped = ptt.on_button(
      ButtonEvent{ButtonId::UpperRight, ButtonEventKind::LongHoldEnd, 3'000'100});
  expect(!started.empty() && started[1].kind == PttEffectKind::CaptureStarted &&
             !stopped.empty() && stopped[1].kind == PttEffectKind::RequestTranscription,
         "upper-right PTT hold/release requests server-side transcription");

  PendingRequest target_request;
  target_request.environment_id = id("environment-fixture");
  target_request.project_id = id("project-fixture");
  target_request.thread_id = id("thread-needs-input");
  target_request.expected_turn_id = id("turn-needs-input");
  target_request.request_id = id("request-approval");
  target_request.kind = PendingRequestKind::Approval;
  InteractionState speech = make_interaction_state(true, "device-fixture-01");
  speech = reduce_interaction(speech, InteractionEvent::open(target_request, 3'000'200)).state;
  const auto frozen_target = speech.target;
  speech = reduce_interaction(
      speech, InteractionEvent::stt_start(id("stream-fixture"), frozen_target, 3'000'201,
                                          id("transcript-1")))
               .state;
  PendingRequest later_request = target_request;
  later_request.thread_id = id("thread-later");
  later_request.expected_turn_id = id("turn-later");
  later_request.request_id = id("request-later");
  speech = reduce_interaction(speech, InteractionEvent::open(later_request, 3'000'202)).state;
  speech = reduce_interaction(
      speech, InteractionEvent::transcript("run the tests", 3'000'203)).state;
  const auto submitted = reduce_interaction(
      speech, InteractionEvent::gesture(Gesture::SwipeRight, 3'000'204));
  expect(submitted.state.mode == InteractionMode::AwaitingAck &&
             has_effect(submitted.effects, EffectKind::SendCommand),
         "final transcript requires an explicit confirmation swipe");
  bool frozen_context = false;
  for (const auto& effect : submitted.effects) {
    if (effect.kind == EffectKind::SendCommand && effect.command.has_value()) {
      frozen_context = effect.command->type == DeviceCommandType::SubmitTranscript &&
                       effect.command->context.thread_id == frozen_target.thread_id &&
                       effect.command->transcript.view() == "run the tests";
    }
  }
  expect(frozen_context, "transcript submission keeps the STT target and text together");

  SleepLockController lock;
  const auto locked = lock.on_bottom_button(
      ButtonEvent{ButtonId::BottomRight, ButtonEventKind::DoubleClick, 3'000'300});
  expect(locked.state.locked && !locked.state.display_on && !locked.state.touch_enabled &&
             !locked.state.voice_enabled && locked.state.transport_retained &&
             !locked.state.hard_power_off,
         "bottom-right double-click locks with display-off and retained transport");
  const auto wake = lock.on_bottom_button(
      ButtonEvent{ButtonId::BottomRight, ButtonEventKind::DoubleClick, 3'000'400});
  expect(!wake.state.locked && wake.state.display_on && wake.state.touch_enabled &&
             wake.state.voice_enabled,
         "bottom-right double-click wakes and unlocks the device");
}

void test_ota_rollback_and_persistence() {
  FakeSlots identity_slots;
  FakeSlots network_slots;
  DeviceIdentityStore identity_store(identity_slots);
  CredentialStore networks(network_slots);
  DeviceIdentity identity;
  identity.device_id = "device-fixture-01";
  identity.scoped_token = "scoped-token";
  identity.gateway_url = "http://gateway.local";
  identity.adapter_sequence = 7;
  expect(identity_store.save(identity) && networks.add_or_update("home", "home-password").ok,
         "OTA fixture starts with durable enrollment and Wi-Fi state");

  MemoryBootStateStore boot_state;
  FixtureSignatureVerifier verifier;
  OtaDeviceProfile device = ota_fixture_device();
  OtaSlotManager manager(device, OtaPartitionLayout{}, boot_state, 1'000, &verifier);
  const auto manifest = ota_fixture_manifest();
  expect(manager.stage(manifest).ok() && manager.on_boot(1'000).ok(),
         "OTA fixture stages and boots the inactive candidate slot");
  manager.mark_board_initialized();
  manager.mark_display_initialized();
  manager.mark_touch_initialized();
  manager.mark_persistence_mounted();
  const auto rollback = manager.tick(31'001);
  expect(rollback.error.code == OtaRecoveryErrorCode::BootTimeout &&
             manager.active_slot() == OtaSlot::Ota0 && !manager.pending_slot().has_value() &&
             manager.storage_unchanged(),
         "unhealthy OTA candidate rolls back without changing storage geometry");

  DeviceIdentityStore reloaded_identity(identity_slots);
  CredentialStore reloaded_networks(network_slots);
  expect(reloaded_identity.load() && reloaded_identity.value().scoped_token == "scoped-token" &&
             reloaded_identity.value().adapter_sequence == 7U && reloaded_networks.load() &&
             reloaded_networks.find("home").has_value(),
         "rollback preserves enrollment, replay cursor, and remembered Wi-Fi state");

  expect(manager.stage(manifest).ok() && manager.on_boot(40'000).ok(),
         "OTA fixture can stage a replacement after rollback");
  manager.mark_board_initialized();
  manager.mark_display_initialized();
  manager.mark_touch_initialized();
  manager.mark_persistence_mounted();
  manager.mark_handshake_succeeded();
  expect(manager.mark_healthy().ok() && manager.active_slot() == OtaSlot::Ota1 &&
             manager.storage_unchanged(),
         "healthy OTA candidate activates while preserving storage");
}

}  // namespace

int main() {
  test_fixture_runner_and_decoding();
  test_cached_boot_enrollment_wifi_and_live_snapshot();
  test_focus_prompt_commands_and_reconnect();
  test_fake_ptt_audio_transcript_and_lock();
  test_ota_rollback_and_persistence();
  if (failures != 0) {
    std::cerr << failures << " integrated host flow test(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "T3 Companion integrated host flow passed (physical audio/flash not exercised)\n";
  return EXIT_SUCCESS;
}
