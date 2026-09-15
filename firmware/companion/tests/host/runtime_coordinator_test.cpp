#include "runtime.hpp"

#include "audio_frame.hpp"
#include "credential_store.hpp"
#include "device_identity.hpp"
#include "http_client.hpp"
#include "snapshot_store.hpp"
#include "transport.hpp"
#include "wifi_ws_client.hpp"

#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <vector>

namespace {

using namespace t3::companion;
using namespace t3::companion::runtime;

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

class FakeClock final : public RuntimeClock {
 public:
  std::uint64_t now_ms() const override { return now; }
  void sleep_ms(std::uint32_t delay_ms) override { now += delay_ms; }

  std::uint64_t now = 1'000;
};

class FakePlatform final : public RuntimePlatform {
 public:
  bool initialize() override {
    events.push_back("board:init");
    return initialize_result;
  }

  std::string device_id() const override { return "device-runtime-test"; }

  bool render(const t3::ui::UiModel& model) override {
    events.push_back("display:render");
    rendered.push_back(model);
    return true;
  }

  bool set_display_power(bool on) override {
    events.push_back(on ? "display:on" : "display:off");
    return true;
  }

  bool wifi_start() override {
    events.push_back("wifi:start");
    ++wifi_starts;
    return true;
  }

  bool wifi_connect(std::string_view ssid, std::string_view /*password*/) override {
    events.push_back("wifi:connect");
    requested_ssids.emplace_back(ssid);
    ++wifi_connects;
    return true;
  }

  bool wifi_connected() const override { return connected; }

  bool start_provisioning_ap(std::string_view ssid) override {
    events.push_back("wifi:provisioning-ap");
    provisioning_ssids.emplace_back(ssid);
    return true;
  }

  bool render_enrollment_pairing(std::string_view enrollment_id,
                                 std::string_view pairing_code,
                                 std::string_view expires_at) override {
    this->enrollment_id = std::string(enrollment_id);
    this->pairing_code = std::string(pairing_code);
    enrollment_expires_at = std::string(expires_at);
    events.push_back("display:enrollment-pairing");
    return true;
  }

  bool audio_start(std::uint32_t /*sample_rate_hz*/) override { return true; }
  int audio_read(std::span<std::int16_t> /*interleaved_samples*/) override { return 0; }
  void audio_stop() override {}

  bool initialize_result = true;
  bool connected = false;
  std::size_t wifi_starts = 0;
  std::size_t wifi_connects = 0;
  std::vector<std::string> events;
  std::vector<std::string> requested_ssids;
  std::vector<std::string> provisioning_ssids;
  std::string enrollment_id;
  std::string pairing_code;
  std::string enrollment_expires_at;
  std::vector<t3::ui::UiModel> rendered;
  std::vector<RuntimeInputEvent> inputs;
};

class FakeWebSocket final : public WebSocketClient {
 public:
  bool open(std::string_view url, std::string_view /*bearer_token*/) override {
    opened_urls.emplace_back(url);
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
  std::vector<std::string> text_frames;
  std::vector<std::vector<std::uint8_t>> binary_frames;
};

class FakeHttp final : public HttpClient {
 public:
  HttpResponse get(std::string_view /*url*/, std::string_view /*bearer_token*/) override {
    ++requests;
    return response;
  }

  HttpResponse response;
  std::size_t requests = 0;
};

class FakeTls final : public TlsIdentityVerifier {
 public:
  bool verify(std::string_view /*host*/, std::string_view /*peer_identity*/) override {
    ++checks;
    return allowed;
  }

  bool allowed = true;
  std::size_t checks = 0;
};

std::string read_fixture(const char* name) {
  std::ifstream input(std::filesystem::path(COMPANION_FIXTURE_DIR) / name,
                      std::ios::binary);
  std::ostringstream bytes;
  bytes << input.rdbuf();
  return bytes.str();
}

void test_unconfigured_first_boot_is_local_and_provisioning() {
  FakeClock clock;
  FakePlatform platform;
  FakeSlots identity_slots;
  FakeSlots network_slots;
  FakeSlots pending_slots;
  FakeSlots snapshot_slots;
  DeviceIdentityStore identity(identity_slots);
  CredentialStore networks(network_slots);
  PendingCommandStore pending(pending_slots);
  SnapshotStore snapshots(snapshot_slots);
  FakeWebSocket websocket;
  FakeHttp http;
  FakeTls tls;
  FixedRuntimeInputQueue input_queue;
  RuntimeDependencies dependencies{clock, platform, identity, networks, pending, snapshots,
                                    websocket, http, tls, input_queue};
  RuntimeCoordinator runtime(dependencies);

  const auto result = runtime.boot();
  expect(result.ok() && runtime.phase() == RuntimePhase::Provisioning,
         "first boot without an enrolled gateway enters provisioning");
  expect(!identity.value().device_id.empty() && identity.value().scoped_token.empty() &&
             identity.value().gateway_url.empty(),
         "first boot persists only a local device identity");
  expect(!platform.rendered.empty() && platform.rendered.front().options.provisioning,
         "provisioning UI is rendered before any external transport action");
  expect(platform.rendered.front().view_state == t3::ui::ViewState::Provisioning,
         "first rendered surface carries the explicit provisioning view state");
  expect(platform.wifi_connects == 0 && websocket.opened_urls.empty() && http.requests == 0,
         "unconfigured boot makes no Wi-Fi, WebSocket, or HTTP gateway call");
  expect(!platform.provisioning_ssids.empty() &&
             platform.provisioning_ssids.front().starts_with("T3-Companion-"),
         "first boot exposes the bounded local setup AP");
}

void test_cached_render_precedes_configured_connect_and_hello_reconcile() {
  FakeClock clock;
  FakePlatform platform;
  FakeSlots identity_slots;
  FakeSlots network_slots;
  FakeSlots pending_slots;
  FakeSlots snapshot_slots;
  DeviceIdentityStore identity(identity_slots);
  DeviceIdentity identity_value;
  identity_value.device_id = "device-runtime-test";
  identity_value.scoped_token = "scoped-runtime-token";
  identity_value.gateway_url = "http://gateway.local";
  expect(identity.save(identity_value), "configured identity is available to the runtime test");
  CredentialStore networks(network_slots);
  expect(networks.add_or_update("runtime-lan", "runtime-password").ok,
         "configured Wi-Fi is available to the runtime test");
  PendingCommandStore pending(pending_slots);
  SnapshotStore snapshots(snapshot_slots);
  const auto decoded_snapshot = decode_snapshot(read_fixture("snapshot.json"));
  expect(decoded_snapshot.ok() && snapshots.save(decoded_snapshot.value),
         "cached fixture snapshot is durable before configured boot");
  FakeWebSocket websocket;
  FakeHttp http;
  FakeTls tls;
  FixedRuntimeInputQueue input_queue;
  RuntimeConfig config;
  config.transport_enabled = true;
  config.allow_trusted_lan = true;
  RuntimeDependencies dependencies{clock, platform, identity, networks, pending, snapshots,
                                    websocket, http, tls, input_queue};
  RuntimeCoordinator runtime(dependencies, config);

  expect(runtime.boot().ok(), "configured runtime boots from the cached state");
  expect(platform.rendered.size() == 1 && !platform.events.empty() &&
             platform.events.front() == "board:init" &&
             platform.events[1] == "display:render",
         "cached display state is rendered before the connection attempt");
  expect(platform.wifi_connects == 1 && websocket.opened_urls.empty(),
         "configured boot starts Wi-Fi but waits for station readiness before WebSocket");
  expect(runtime.provisioning_status_json().find("\"snapshot_sequence\":0") !=
             std::string::npos,
         "provisioning status reports zero before a live snapshot is applied");
  platform.connected = true;
  expect(runtime.tick().ok(), "station readiness advances the companion connection");
  expect(websocket.opened_urls.size() == 1 &&
             websocket.opened_urls.front() == "ws://gateway.local/api/companion/v1/ws",
         "configured runtime opens only the dedicated companion websocket route");
  expect(!runtime.gestures_enabled(), "gestures stay disabled until hello reconciliation");

  HelloAck ack;
  (void)ack.session_id.assign("runtime-session");
  ack.resume_replay = false;
  expect(runtime.on_hello_ack(ack).ok() && runtime.gestures_enabled(),
         "hello acknowledgement reconciles status before enabling gestures");
  expect(runtime.on_socket_text(read_fixture("snapshot.json")).ok(),
         "live snapshot replaces the display-only cache after hello");
  expect(runtime.provisioning_status_json().find("\"snapshot_sequence\":7") !=
             std::string::npos,
         "provisioning status reports the applied live snapshot sequence");
  expect(!platform.rendered.empty() && platform.rendered.back().live_handshake,
         "live snapshot is visibly marked live after the handshake");
}

void test_enrollment_offer_displays_pairing_and_reconnects_with_gateway_credential() {
  FakeClock clock;
  FakePlatform platform;
  FakeSlots identity_slots;
  FakeSlots network_slots;
  FakeSlots pending_slots;
  FakeSlots snapshot_slots;
  DeviceIdentityStore identity(identity_slots);
  CredentialStore networks(network_slots);
  PendingCommandStore pending(pending_slots);
  SnapshotStore snapshots(snapshot_slots);
  FakeWebSocket websocket;
  FakeHttp http;
  FakeTls tls;
  FixedRuntimeInputQueue input_queue;
  RuntimeConfig config;
  config.transport_enabled = true;
  config.allow_trusted_lan = true;
  RuntimeDependencies dependencies{clock, platform, identity, networks, pending, snapshots,
                                    websocket, http, tls, input_queue};
  RuntimeCoordinator runtime(dependencies, config);

  expect(runtime.boot().ok(), "enrollment flow starts from local provisioning");
  expect(runtime.configure_gateway("http://gateway.local", TransportSecurity::TrustedLan).ok(),
         "gateway origin is configured before enrollment");
  expect(runtime.append_wifi_network("runtime-lan", "runtime-password").ok(),
         "remembered Wi-Fi starts the enrollment transport");
  expect(platform.wifi_connects == 1 && websocket.opened_urls.empty(),
         "enrollment waits for station readiness before opening its socket");

  platform.connected = true;
  expect(runtime.tick().ok(), "station readiness opens the enrollment socket");
  expect(websocket.opened_urls.size() == 1 &&
             websocket.opened_urls.back() ==
                 "ws://gateway.local/api/companion/v1/enroll?deviceId=device-runtime-test",
         "enrollment uses the dedicated route with the local device identity query");

  const std::string offer =
      R"({"type":"enrollment","protocolVersion":1,"enrollmentId":"enroll-1","deviceId":"device-runtime-test","pairingCode":"ABCD23","expiresAt":"2026-08-21T19:40:00Z"})";
  expect(runtime.on_socket_text(offer).ok() && platform.enrollment_id == "enroll-1" &&
             platform.pairing_code == "ABCD23" &&
             platform.enrollment_expires_at == "2026-08-21T19:40:00Z",
         "enrollment metadata is bounded and rendered for T3 Code pairing");

  const std::string credential =
      R"({"type":"enrollment_credential","credential":"scoped-runtime-token"})";
  expect(runtime.on_socket_text(credential).ok() &&
             identity.value().scoped_token == "scoped-runtime-token",
         "gateway credential persists only after the still-open pairing session completes");
  expect(runtime.tick().ok() && websocket.opened_urls.size() == 2 &&
             websocket.opened_urls.back() == "ws://gateway.local/api/companion/v1/ws",
         "enrollment credential transitions to the authenticated companion route");
}

void test_paired_transport_reconnects_from_provisioning_phase() {
  FakeClock clock;
  FakePlatform platform;
  FakeSlots identity_slots;
  FakeSlots network_slots;
  FakeSlots pending_slots;
  FakeSlots snapshot_slots;
  DeviceIdentityStore identity(identity_slots);
  DeviceIdentity identity_value;
  identity_value.device_id = "device-runtime-test";
  identity_value.scoped_token = "scoped-runtime-token";
  identity_value.gateway_url = "http://192.0.2.113:39073";
  expect(identity.save(identity_value), "paired identity is available to provisioning reconnect test");
  CredentialStore networks(network_slots);
  expect(networks.add_or_update("runtime-lan", "runtime-password").ok,
         "paired reconnect test has a remembered Wi-Fi network");
  PendingCommandStore pending(pending_slots);
  SnapshotStore snapshots(snapshot_slots);
  FakeWebSocket websocket;
  FakeHttp http;
  FakeTls tls;
  FixedRuntimeInputQueue input_queue;
  RuntimeConfig config;
  config.transport_enabled = true;
  config.allow_trusted_lan = true;
  RuntimeDependencies dependencies{clock, platform, identity, networks, pending, snapshots,
                                    websocket, http, tls, input_queue};
  RuntimeCoordinator runtime(dependencies, config);

  expect(runtime.boot().ok() && websocket.opened_urls.empty(),
         "paired boot waits for station readiness before opening transport");
  expect(runtime.begin_provisioning().ok() && runtime.phase() == RuntimePhase::Provisioning,
         "paired runtime can show local provisioning while reconnecting");
  platform.connected = true;
  expect(runtime.tick().ok() && websocket.opened_urls.size() == 1 &&
             websocket.opened_urls.back() == "ws://192.0.2.113:39073/api/companion/v1/ws",
         "paired transport reconnects even when provisioning remains the visible phase");
}

void test_optional_gateway_submission_and_trusted_lan_origin() {
  FakeClock clock;
  FakePlatform platform;
  FakeSlots identity_slots;
  FakeSlots network_slots;
  FakeSlots pending_slots;
  FakeSlots snapshot_slots;
  DeviceIdentityStore identity(identity_slots);
  CredentialStore networks(network_slots);
  PendingCommandStore pending(pending_slots);
  SnapshotStore snapshots(snapshot_slots);
  FakeWebSocket websocket;
  FakeHttp http;
  FakeTls tls;
  FixedRuntimeInputQueue input_queue;
  RuntimeConfig config;
  config.transport_enabled = true;
  config.allow_trusted_lan = true;
  RuntimeDependencies dependencies{clock, platform, identity, networks, pending, snapshots,
                                    websocket, http, tls, input_queue};
  RuntimeCoordinator runtime(dependencies, config);

  expect(runtime.boot().ok(), "optional-gateway regression starts from provisioning");
  expect(runtime.submit_provisioning("runtime-lan", "runtime-password", "",
                                     TransportSecurity::TrustedLan)
             .ok(),
         "blank optional gateway saves Wi-Fi without a configuration error");
  expect(identity.value().gateway_url.empty() && networks.find("runtime-lan").has_value(),
         "Wi-Fi-only setup leaves the gateway unconfigured");
  expect(runtime.submit_provisioning("", "", "http://192.0.2.113:39073",
                                     TransportSecurity::TrustedLan)
             .ok(),
         "explicit trusted-LAN origin succeeds when local policy is enabled");
  expect(identity.value().gateway_url == "http://192.0.2.113:39073",
         "trusted-LAN origin is persisted exactly as an origin");
}

}  // namespace

int main() {
  test_unconfigured_first_boot_is_local_and_provisioning();
  test_cached_render_precedes_configured_connect_and_hello_reconcile();
  test_enrollment_offer_displays_pairing_and_reconnects_with_gateway_credential();
  test_paired_transport_reconnects_from_provisioning_phase();
  test_optional_gateway_submission_and_trusted_lan_origin();
  if (failures != 0) {
    std::cerr << failures << " runtime coordinator test(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "T3 Companion runtime coordinator tests passed\n";
  return EXIT_SUCCESS;
}
