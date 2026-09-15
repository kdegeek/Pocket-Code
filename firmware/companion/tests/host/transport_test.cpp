#include "http_client.hpp"
#include "reconnect.hpp"
#include "transport.hpp"
#include "wifi_ws_client.hpp"

#include "command_outbox.hpp"
#include "device_identity.hpp"
#include "pending_commands.hpp"
#include "snapshot_store.hpp"

#include <array>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <span>
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
    slots_[slot] = std::vector<std::uint8_t>(bytes.begin(), bytes.end());
    return true;
  }

 private:
  std::array<std::optional<std::vector<std::uint8_t>>, 2> slots_;
};

class FakeTls final : public TlsIdentityVerifier {
 public:
  bool verify(std::string_view host, std::string_view peer_identity) override {
    last_host = std::string(host);
    last_peer = std::string(peer_identity);
    return allowed;
  }

  bool allowed = true;
  std::string last_host;
  std::string last_peer;
};

class FakeHttp final : public HttpClient {
 public:
  HttpResponse get(std::string_view url, std::string_view bearer_token) override {
    last_url = std::string(url);
    last_token = std::string(bearer_token);
    return response;
  }

  HttpResponse response;
  std::string last_url;
  std::string last_token;
};

class FakeWs final : public WebSocketClient {
 public:
  bool open(std::string_view url, std::string_view bearer_token) override {
    opened_url = std::string(url);
    opened_token = std::string(bearer_token);
    open_count += 1;
    return open_result;
  }

  bool send_text(std::string_view frame) override {
    sent_frames.emplace_back(frame);
    return send_result;
  }

  void close() override { closed = true; }

  bool open_result = true;
  bool send_result = true;
  bool closed = false;
  int open_count = 0;
  std::string opened_url;
  std::string opened_token;
  std::vector<std::string> sent_frames;
};

Id id(const char* value) {
  Id result;
  (void)result.assign(value);
  return result;
}

DeviceCommand command(const char* command_id) {
  DeviceCommand result;
  result.type = DeviceCommandType::CancelTurn;
  result.command_id = id(command_id);
  result.device_id = id("device-1");
  result.context.environment_id = id("environment");
  result.context.project_id = id("project");
  result.context.thread_id = id("thread");
  result.context.expected_turn_id = id("turn");
  return result;
}

Snapshot snapshot(std::uint64_t sequence) {
  Snapshot result;
  result.adapter_sequence = sequence;
  result.connectivity.status = ConnectivityStatus::Connected;
  result.freshness.is_stale = false;
  (void)result.freshness.observed_at.assign("2026-08-21T12:00:00Z");
  return result;
}

void test_endpoint_security_and_route_isolation() {
  const auto lan = build_companion_routes("http://gateway.local", TransportSecurity::TrustedLan);
  expect(lan.ok() && lan.value.ws == "ws://gateway.local/api/companion/v1/ws",
         "trusted LAN derives the dedicated companion websocket route");
  expect(lan.value.enroll == "ws://gateway.local/api/companion/v1/enroll" &&
             lan.value.snapshot == "http://gateway.local/api/companion/v1/snapshot",
         "trusted LAN derives the dedicated enrollment and snapshot routes");
  expect(!build_companion_routes("http://gateway.local", TransportSecurity::Remote).ok(),
         "remote plain HTTP is rejected");
  const auto remote = build_companion_routes("https://gateway.tailnet", TransportSecurity::Remote);
  expect(remote.ok() && remote.value.ws == "wss://gateway.tailnet/api/companion/v1/ws",
         "remote HTTPS derives secure websocket route");
  expect(!build_companion_routes("https://gateway.local/ws", TransportSecurity::Remote).ok(),
         "general T3 websocket path is rejected");
  expect(!build_companion_routes("https://gateway.local", TransportSecurity::TrustedLan).ok(),
         "trusted LAN mode requires an explicit local HTTP endpoint");
}

void test_short_lived_pairing_and_revocation() {
  PairingTokenStore store;
  expect(store.put_pairing("enrollment", "123456", 1'000), "pairing code is stored");
  expect(store.pairing_matches("enrollment", "123456", 999), "pairing works before expiry");
  expect(store.consume_pairing("enrollment", "123456", 999), "pairing code is single-use");
  expect(!store.pairing_matches("enrollment", "123456", 999), "consumed pairing cannot replay");
  expect(store.put_pairing("enrollment-2", "654321", 1'000), "second pairing can be opened");
  expect(!store.pairing_matches("enrollment-2", "654321", 1'000), "pairing expires at the boundary");
  expect(store.put_token("scoped-token", 2'000), "scoped token is stored");
  expect(store.has_valid_token(1'999), "token is valid before expiry");
  store.revoke();
  expect(!store.has_valid_token(1'999), "revocation invalidates the cached token");
}

void test_handshake_reconcile_snapshot_and_offline_commands() {
  FakeSlots identity_slots;
  DeviceIdentityStore identity(identity_slots);
  DeviceIdentity identity_value;
  identity_value.device_id = "device-1";
  identity_value.scoped_token = "scoped-token";
  identity_value.gateway_url = "http://gateway.local";
  expect(identity.save(identity_value), "identity saves for transport");
  PendingCommandStore pending;
  CommandOutbox outbox;
  SnapshotStore snapshots;
  FakeWs ws;
  FakeHttp http;
  FakeTls tls;
  std::uint64_t now = 500;
  Transport transport({&identity, &pending, &snapshots, &outbox, &ws, &http, &tls,
                       [&now]() { return now; }});
  expect(transport.configure(TransportSecurity::TrustedLan).ok(), "transport configures LAN routes");
  expect(transport.load_cache().ok(), "cache load is safe when empty");
  expect(transport.cached_prompts_display_only(), "cached prompts start display-only");
  expect(transport.connect().ok(), "transport opens the dedicated websocket");
  expect(ws.opened_url == "ws://gateway.local/api/companion/v1/ws" && ws.opened_token == "scoped-token",
         "transport authenticates only to the dedicated companion route");
  expect(transport.on_socket_open().ok() && ws.sent_frames.size() == 1,
         "socket open sends exactly one hello cursor");
  HelloAck ack;
  (void)ack.session_id.assign("session-1");
  ack.resume_replay = false;
  CommandStatusRecord record;
  record.command_id = id("command-1");
  record.status = CommandStatus::Committed;
  (void)record.updated_at.assign("2026-08-21T12:00:00Z");
  (void)ack.pending_commands.records.push(record);
  expect(transport.on_hello_ack(ack).ok(), "hello ack is accepted");
  expect(transport.gestures_enabled(), "gestures enable only after hello status reconciliation");
  expect(pending.find(id("command-1"))->status == CommandStatus::Committed,
         "hello pending status is persisted");
  expect(transport.on_snapshot(snapshot(7)).ok(), "snapshot applies after live attachment");
  expect(identity.value().adapter_sequence == 7, "snapshot cursor is persisted atomically");
  ServerEvent event;
  event.protocol_version = kProtocolVersion;
  event.session_id = id("session-1");
  event.adapter_sequence = 8;
  event.event_id = id("event-8");
  expect(transport.on_server_event(event).ok(), "next live event advances the persisted cursor");
  expect(!transport.on_server_event(event).ok(), "duplicate live event is replay-rejected");
  event.adapter_sequence = 10;
  event.event_id = id("event-10");
  expect(transport.on_server_event(event).error.code == TransportErrorCode::RetentionMiss,
         "event gap requests a retention-miss snapshot");
  const auto online = transport.submit_command(command("command-2"));
  expect(online.ok() && ws.sent_frames.size() == 2, "live command is sent once");
  expect(decode_device_command(ws.sent_frames.back()).ok(),
         "sent command uses the bounded v1 device-command codec");
  const auto duplicate = transport.submit_command(command("command-2"));
  expect(duplicate.ok() && ws.sent_frames.size() == 2, "duplicate command id is not resent");
  (void)transport.on_gateway_lost();
  const auto offline = transport.submit_command(command("offline"));
  expect(!offline.ok() && offline.error.code == TransportErrorCode::Offline,
         "offline mutating command is rejected");
  expect(!transport.gestures_enabled(), "gateway loss disables mutating gestures");
  expect(transport.on_socket_open().ok() && ws.sent_frames.size() == 3,
         "reconnect sends a fresh hello but never resends the pending command");
}

void test_replay_and_protocol_failures() {
  ReplayGuard replay;
  expect(replay.accept(4), "first adapter sequence is accepted");
  expect(!replay.accept(4), "duplicate adapter sequence is rejected");
  expect(!replay.accept(3), "older adapter sequence is rejected");
  expect(replay.accept(5), "next adapter sequence is accepted");

  Backoff backoff({100, 800, 2.0, 25});
  const auto first = backoff.next(50);
  const auto second = backoff.next(50);
  const auto third = backoff.next(100);
  expect(first.delay_ms == 100 && second.delay_ms == 200 && third.delay_ms == 500,
         "backoff is exponential, capped, and jittered deterministically");
  backoff.reset();
  expect(backoff.attempt() == 0, "backoff reset clears attempts");

  FakeWs ws;
  FakeHttp http;
  FakeTls tls;
  Transport transport({nullptr, nullptr, nullptr, nullptr, &ws, &http, &tls, []() { return 0ULL; }});
  expect(transport.configure(TransportSecurity::Remote).error.code == TransportErrorCode::MissingIdentity,
         "remote transport requires enrolled identity before connecting");
  expect(transport.on_protocol_error(2, 3).error.code == TransportErrorCode::Incompatible,
         "protocol incompatibility is surfaced as a terminal transport state");
}

}  // namespace

int main() {
  test_endpoint_security_and_route_isolation();
  test_short_lived_pairing_and_revocation();
  test_handshake_reconcile_snapshot_and_offline_commands();
  test_replay_and_protocol_failures();
  if (failures != 0) {
    std::cerr << failures << " transport test(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "T3 Companion transport tests passed\n";
  return EXIT_SUCCESS;
}
