#include "gateway_discovery.hpp"
#include "network_store.hpp"
#include "roaming.hpp"
#include "softap_portal.hpp"

#include <cstdlib>
#include <iostream>
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

void test_append_update_priority_fallback_and_eviction() {
  NetworkStore store;
  expect(store.capacity() == kMaxRememberedNetworks, "capacity is the documented bounded NVS budget");
  expect(store.add_or_update("one", "pass-one").ok, "first network appends");
  expect(store.add_or_update("two", "pass-two").ok, "second network appends");
  expect(store.add_or_update("three", "pass-three").ok, "third network appends");
  expect(store.add_or_update("four", "pass-four").ok, "fourth network appends");
  expect(store.add_or_update("five", "pass-five").ok, "fifth network appends");

  const auto before = store.entries();
  const auto update = store.add_or_update("three", "new-pass");
  expect(update.kind == CredentialMutation::Updated, "duplicate SSID updates credentials");
  expect(store.entries().size() == 5, "duplicate update does not grow the list");
  expect(store.entries()[2].ssid == "three" && store.entries()[2].password == "new-pass",
         "duplicate update preserves position while replacing the password");
  expect(store.entries()[0].ssid == before[0].ssid && store.entries()[1].ssid == before[1].ssid,
         "duplicate update does not reorder neighboring entries");

  expect(store.mark_successful("four"), "successful station is recorded");
  const auto fallback = store.fallback_sequence();
  expect(fallback.size() == 5 && fallback[0].ssid == "four",
         "current successful network receives first fallback priority");
  expect(store.current()->ssid == "four", "current network is observable");

  const auto evicted = store.add_or_update("six", "pass-six");
  expect(evicted.kind == CredentialMutation::Added && evicted.evicted_ssid.has_value(),
         "full store adds by evicting an old non-current entry");
  expect(evicted.evicted_ssid.value() == "one", "oldest non-current network is evicted");
  expect(store.find("four").has_value(), "current network is never evicted");
  expect(!store.find("one").has_value(), "oldest non-current network is removed");
}

void test_reset_is_explicit_and_station_roaming_is_bounded() {
  NetworkStore store;
  (void)store.add_or_update("home", "home-pass");
  (void)store.add_or_update("backup", "backup-pass");
  expect(!store.reset_requested(), "normal operation does not request a factory reset");
  expect(store.factory_reset().ok, "factory reset is explicit");
  expect(store.empty() && store.reset_requested(), "factory reset atomically clears the list");

  NetworkStore roaming_store;
  (void)roaming_store.add_or_update("a", "a-pass");
  (void)roaming_store.add_or_update("b", "b-pass");
  RoamingController roaming(roaming_store);
  const auto first = roaming.next_attempt();
  expect(first.has_value() && first->ssid == "a", "roaming starts with ordered credentials");
  expect(roaming.report_failure(), "failed station advances the fallback cursor");
  const auto second = roaming.next_attempt();
  expect(second.has_value() && second->ssid == "b", "roaming falls back to next network");
  expect(roaming.report_success("b"), "successful fallback becomes current");
  expect(roaming_store.current()->ssid == "b", "roaming success updates station priority");
}

void test_softap_preserves_station_until_intentional_join_and_lifecycle() {
  NetworkStore store;
  (void)store.add_or_update("current", "current-pass");
  SoftApPortal portal(store, 0x1a2b);
  expect(portal.begin(true), "portal starts while station is connected");
  expect(portal.ap_ssid() == "T3-Companion-1A2B", "setup AP name uses the device suffix");
  expect(portal.station_preserved(), "starting setup preserves the current station");
  expect(portal.status() == ProvisioningStatus::Started, "portal emits started status");
  expect(portal.request_setup_join(), "setup join requires an explicit intent");
  expect(portal.station_preserved(), "requesting setup join still preserves station");
  expect(portal.join_setup_ap(), "intentional setup join transitions the station");
  expect(!portal.station_preserved(), "station is released only after intentional setup join");
  expect(portal.append_network("new", "new-pass"), "portal appends selected credentials");
  expect(portal.complete(), "portal completes after a successful append");
  expect(portal.status() == ProvisioningStatus::Completed, "portal emits completed status");

  SoftApPortal cancelled(store, 0xbeef);
  expect(cancelled.begin(false), "portal can start without a current station");
  expect(cancelled.cancel(), "portal supports cancellation");
  expect(cancelled.status() == ProvisioningStatus::Cancelled, "portal emits cancelled status");
}

void test_gateway_scan_and_manual_url() {
  GatewayDiscovery discovery;
  discovery.set_scan_results({GatewayCandidate{"T3 Gateway", "http://gateway.local:4310"}});
  const auto scanned = discovery.scan();
  expect(scanned.size() == 1 && scanned[0].url == "http://gateway.local:4310",
         "gateway discovery exposes injected scan results");
  expect(discovery.select_manual("https://gateway.example/ws"), "manual gateway URL is accepted");
  expect(discovery.selected()->url == "https://gateway.example/ws", "manual gateway is selected");
  expect(!discovery.select_manual("gateway.example"), "gateway URLs require an explicit scheme");
  expect(!discovery.select_manual("https://bad url"), "gateway URLs reject whitespace");
}

}  // namespace

int main() {
  test_append_update_priority_fallback_and_eviction();
  test_reset_is_explicit_and_station_roaming_is_bounded();
  test_softap_preserves_station_until_intentional_join_and_lifecycle();
  test_gateway_scan_and_manual_url();
  if (failures != 0) {
    std::cerr << failures << " network/provisioning test(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "T3 Companion network/provisioning tests passed\n";
  return EXIT_SUCCESS;
}
