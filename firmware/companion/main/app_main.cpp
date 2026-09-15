#include "idf_adapters.hpp"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include "esp_log.h"

using t3::companion::runtime::IdfBspPlatform;
using t3::companion::runtime::IdfClock;
using t3::companion::runtime::IdfHttpClient;
using t3::companion::runtime::IdfNvsSlotStorage;
using t3::companion::runtime::IdfOtaHealth;
using t3::companion::runtime::IdfRuntimeQueue;
using t3::companion::runtime::IdfTlsIdentityVerifier;
using t3::companion::runtime::IdfWebSocketClient;
using t3::companion::runtime::RuntimeConfig;
using t3::companion::runtime::RuntimeCoordinator;
using t3::companion::runtime::RuntimeDependencies;

namespace {

constexpr char kTag[] = "t3_companion";

#if defined(CONFIG_T3_COMPANION_ALLOW_TRUSTED_LAN) && CONFIG_T3_COMPANION_ALLOW_TRUSTED_LAN
constexpr bool kAllowTrustedLan = true;
#else
constexpr bool kAllowTrustedLan = false;
#endif
#if defined(CONFIG_T3_COMPANION_ALLOW_REMOTE) && CONFIG_T3_COMPANION_ALLOW_REMOTE
constexpr bool kAllowRemote = true;
#else
constexpr bool kAllowRemote = false;
#endif

}  // namespace

extern "C" void app_main(void) {
  // NVS initialization is deliberately fail-closed. The runtime never erases
  // NVS automatically because it contains enrollment, Wi-Fi, cursor, and
  // rollback journals needed for recovery.
  const auto nvs_result = nvs_flash_init();
  if (nvs_result != ESP_OK) {
    ESP_LOGE(kTag, "persistent companion state is unavailable; refusing network startup");
    return;
  }

  static IdfClock clock;
  static IdfBspPlatform platform;
  static IdfNvsSlotStorage identity_slots("t3_identity");
  static IdfNvsSlotStorage network_slots("t3_network");
  static IdfNvsSlotStorage pending_slots("t3_pending");
  static IdfNvsSlotStorage snapshot_slots("t3_snapshot");
  if (!identity_slots.begin() || !network_slots.begin() || !pending_slots.begin() ||
      !snapshot_slots.begin()) {
    ESP_LOGE(kTag, "persistent companion journals could not be opened; refusing network startup");
    return;
  }

  static t3::companion::DeviceIdentityStore identity(identity_slots);
  static t3::companion::CredentialStore networks(network_slots);
  static t3::companion::PendingCommandStore pending(pending_slots);
  static t3::companion::SnapshotStore snapshots(snapshot_slots);

#if defined(CONFIG_T3_COMPANION_ALLOW_REMOTE) && CONFIG_T3_COMPANION_ALLOW_REMOTE
  static IdfHttpClient http({true, nullptr, t3::companion::kMaxJsonBytes});
  static IdfWebSocketClient websocket({true, nullptr, t3::companion::kMaxJsonBytes});
#else
  static IdfHttpClient http({false, nullptr, t3::companion::kMaxJsonBytes});
  static IdfWebSocketClient websocket({false, nullptr, t3::companion::kMaxJsonBytes});
#endif
  static IdfTlsIdentityVerifier tls;
  static IdfRuntimeQueue input_queue;
  static IdfOtaHealth ota_health;

  RuntimeConfig config;
  config.transport_enabled = kAllowTrustedLan || kAllowRemote;
  config.allow_trusted_lan = kAllowTrustedLan;
  config.allow_remote = kAllowRemote;
  config.security = kAllowRemote
                        ? t3::companion::TransportSecurity::Remote
                        : t3::companion::TransportSecurity::TrustedLan;

  RuntimeDependencies dependencies{clock, platform, identity, networks, pending, snapshots,
                                    websocket, http, tls, input_queue, &ota_health};
  static RuntimeCoordinator* runtime = nullptr;
  // RuntimeCoordinator owns the bounded state machines and queues.  Keep the
  // object in static storage: app_main runs on ESP-IDF's small main task stack
  // and a stack-local coordinator would overflow before the first render.
  static RuntimeCoordinator coordinator(dependencies, config);
  runtime = &coordinator;
  websocket.set_callbacks(
      [](std::string_view frame) {
        if (runtime != nullptr) (void)runtime->on_socket_text(frame);
      },
      [](std::span<const std::uint8_t> /*frame*/) {},
      []() {
        if (runtime != nullptr) (void)runtime->on_socket_closed();
      });

  if (!coordinator.boot().ok()) {
    ESP_LOGE(kTag, "companion runtime boot failed; leaving device offline");
    return;
  }
  while (true) {
    platform.service_inputs();
    (void)coordinator.tick();
    clock.sleep_ms(20);
  }
}
