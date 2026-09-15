#include "https_ota.hpp"
#include "manifest.hpp"
#include "rollback.hpp"

#include <array>
#include <cstdlib>
#include <iostream>
#include <span>
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

OtaManifest fixture_manifest() {
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

OtaDeviceProfile fixture_device() {
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

void test_hash_and_manifest_gates() {
  const auto manifest = fixture_manifest();
  const auto device = fixture_device();
  FixtureSignatureVerifier verifier;
  expect(sha256_hex(std::span<const std::uint8_t>{
             reinterpret_cast<const std::uint8_t*>("abc"), 3}) ==
             "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
         "SHA-256 hash is deterministic at the host seam");
  expect(validate_manifest(manifest, device, verifier).ok(),
         "signed manifest passes chip, geometry, protocol, hash, and signature gates");

  auto wrong_chip = manifest;
  wrong_chip.chip = "esp32c3";
  expect(validate_manifest(wrong_chip, device, verifier).error.code ==
             OtaManifestErrorCode::ChipMismatch,
         "wrong chip is rejected");
  auto wrong_flash = manifest;
  wrong_flash.flash_size_bytes = 0x2000000;
  expect(validate_manifest(wrong_flash, device, verifier).error.code ==
             OtaManifestErrorCode::FlashGeometryMismatch,
         "wrong flash geometry is rejected");
  auto wrong_protocol = manifest;
  wrong_protocol.protocol_min = 2;
  wrong_protocol.protocol_max = 3;
  expect(validate_manifest(wrong_protocol, device, verifier).error.code ==
             OtaManifestErrorCode::ProtocolMismatch,
         "incompatible protocol range is rejected");
  auto downgrade = manifest;
  downgrade.version = 10;
  downgrade.signature = FixtureSignatureVerifier::signature_for_test(
      downgrade.signed_payload());
  expect(validate_manifest(downgrade, device, verifier).error.code ==
             OtaManifestErrorCode::Downgrade,
         "version downgrade is rejected by anti-rollback policy");
  auto tampered = manifest;
  tampered.image_sha256[0] = tampered.image_sha256[0] == '0' ? '1' : '0';
  expect(validate_manifest(tampered, device, verifier).error.code ==
             OtaManifestErrorCode::SignatureMismatch,
         "manifest tampering invalidates the signature");
}

class FakeHttp final : public OtaHttpClient {
 public:
  OtaHttpResponse get(std::string_view url, std::size_t offset) override {
    ++requests;
    last_url = std::string(url);
    if (offset == 0) return OtaHttpResponse{206, 0, {'o'}, false, {}};
    if (offset == 1) return OtaHttpResponse{206, 1, {'t', 'a'}, true, {}};
    return OtaHttpResponse{416, offset, {}, false, "unexpected offset"};
  }

  int requests = 0;
  std::string last_url;
};

class FakeWriter final : public OtaImageWriter {
 public:
  bool begin(std::size_t size) override {
    image.clear();
    image.reserve(size);
    return true;
  }
  bool write(std::size_t offset, std::span<const std::uint8_t> bytes) override {
    if (offset != image.size()) return false;
    image.insert(image.end(), bytes.begin(), bytes.end());
    return true;
  }
  bool finish() override {
    finished = true;
    return true;
  }

  std::vector<std::uint8_t> image;
  bool finished = false;
};

void test_https_resumable_transfer() {
  auto manifest = fixture_manifest();
  FakeHttp http;
  FakeWriter writer;
  HttpsOtaSession session(http);
  expect(!session.start("http://gateway.local/firmware.bin", manifest, writer).ok(),
         "plain HTTP OTA endpoint is rejected");
  expect(session.start("https://gateway.local/firmware.bin", manifest, writer).ok(),
         "HTTPS session starts without making a request");
  expect(http.requests == 0, "session start is an injectable no-request boundary");
  expect(session.pull().ok() && session.received_bytes() == 1,
         "first chunk is accepted at the expected offset");
  expect(session.interrupt().ok() && session.status() == OtaTransferStatus::Interrupted,
         "interrupted transfer preserves resumable state");
  expect(session.resume().ok() && session.pull().ok(),
         "resumed transfer requests from the preserved offset");
  expect(session.finish().ok() && writer.finished && session.status() == OtaTransferStatus::Ready,
         "complete image hash and size promote the inactive image to ready");
  expect(writer.image == std::vector<std::uint8_t>({'o', 't', 'a'}),
         "A/B image writer receives bytes in order");
  expect(http.requests == 2 && http.last_url == "https://gateway.local/firmware.bin",
         "only explicit pull calls use the HTTPS client seam");
}

void test_ab_health_timeout_rollback_and_storage() {
  const auto manifest = fixture_manifest();
  auto device = fixture_device();
  MemoryBootStateStore state;
  OtaPartitionLayout layout;
  layout.ota0_offset = 0x20000;
  layout.ota1_offset = 0x720000;
  layout.slot_size_bytes = 0x700000;
  layout.storage_offset = 0xe20000;
  layout.storage_size_bytes = 0x1d0000;
  FixtureSignatureVerifier verifier;
  OtaSlotManager manager(device, layout, state, 1000, &verifier);
  expect(manager.select_target() == OtaSlot::Ota1,
         "A/B selector chooses the inactive slot");
  expect(manager.stage(manifest).ok() && manager.pending_slot() == OtaSlot::Ota1,
         "validated image is staged only in the inactive slot");
  expect(manager.storage_unchanged(), "staging does not erase or rewrite storage");
  expect(manager.on_boot(1000).ok(), "pending slot enters boot validation");
  manager.mark_board_initialized();
  manager.mark_display_initialized();
  manager.mark_touch_initialized();
  manager.mark_persistence_mounted();
  expect(manager.tick(31'001).error.code == OtaRecoveryErrorCode::BootTimeout,
         "boot health timeout rolls back an unhealthy image");
  expect(manager.active_slot() == OtaSlot::Ota0 && !manager.pending_slot().has_value(),
         "rollback selects the previous healthy slot");
  expect(manager.storage_unchanged(), "rollback preserves the storage partition");

  expect(manager.stage(manifest).ok() && manager.on_boot(40'000).ok(),
         "a second candidate can be staged after rollback");
  manager.mark_board_initialized();
  manager.mark_display_initialized();
  manager.mark_touch_initialized();
  manager.mark_persistence_mounted();
  manager.mark_handshake_succeeded();
  expect(manager.mark_healthy().ok() && manager.active_slot() == OtaSlot::Ota1,
         "board init plus handshake marks the candidate healthy");
  expect(manager.storage_offset() == layout.storage_offset &&
             manager.storage_size_bytes() == layout.storage_size_bytes,
         "storage geometry remains unchanged across a successful A/B switch");
}

}  // namespace

int main() {
  test_hash_and_manifest_gates();
  test_https_resumable_transfer();
  test_ab_health_timeout_rollback_and_storage();
  if (failures != 0) {
    std::cerr << failures << " OTA test(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "T3 Companion OTA tests passed\n";
  return EXIT_SUCCESS;
}
