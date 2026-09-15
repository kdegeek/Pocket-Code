#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace t3::companion {

/** The only errors that can be produced while checking a signed OTA manifest. */
enum class OtaManifestErrorCode {
  None,
  InvalidSchema,
  InvalidVersion,
  InvalidProtocolRange,
  ProtocolMismatch,
  ChipMismatch,
  FlashGeometryMismatch,
  StorageGeometryMismatch,
  ImageTooLarge,
  InvalidHash,
  SignatureMismatch,
  UnknownKey,
  Downgrade,
};

struct OtaManifestError {
  OtaManifestErrorCode code = OtaManifestErrorCode::None;
  std::string message;
};

struct OtaManifestValidation {
  OtaManifestError error;
  [[nodiscard]] bool ok() const {
    return error.code == OtaManifestErrorCode::None;
  }
  static OtaManifestValidation success() { return {}; }
  static OtaManifestValidation failure(OtaManifestErrorCode code,
                                       std::string message) {
    return {{code, std::move(message)}};
  }
};

/**
 * Metadata signed by the release authority. `version` is a monotonically
 * increasing anti-rollback number, not a display string. No private key is
 * ever represented by this type.
 */
struct OtaManifest {
  std::uint32_t schema_version = 1;
  std::uint64_t version = 0;
  std::uint32_t protocol_min = 1;
  std::uint32_t protocol_max = 1;
  std::string chip;
  std::uint32_t flash_size_bytes = 0;
  std::uint32_t slot_size_bytes = 0;
  std::uint32_t image_size_bytes = 0;
  std::uint32_t storage_offset = 0;
  std::uint32_t storage_size_bytes = 0;
  std::string image_sha256;
  std::string key_id;
  std::string signature;

  /** Stable, field-ordered payload covered by the signature. */
  [[nodiscard]] std::string signed_payload() const;
};

struct OtaDeviceProfile {
  std::string chip;
  std::uint32_t flash_size_bytes = 0;
  std::uint32_t protocol_min = 1;
  std::uint32_t protocol_max = 1;
  std::uint64_t current_version = 0;
  bool allow_downgrade = false;
  std::uint32_t storage_offset = 0;
  std::uint32_t storage_size_bytes = 0;
};

/** Release-time public-key boundary; production implementations live outside
 * this host-testable component and must not expose private signing material. */
class SignatureVerifier {
 public:
  virtual ~SignatureVerifier() = default;
  [[nodiscard]] virtual bool verify(std::string_view key_id,
                                    std::string_view signed_payload,
                                    std::string_view signature) const = 0;
};

/**
 * Deterministic, deliberately non-cryptographic fixture verifier. It exists
 * only to make host tests reproducible; it is not a release signing policy.
 */
class FixtureSignatureVerifier final : public SignatureVerifier {
 public:
  static constexpr std::string_view kKeyId = "fixture-v1";

  [[nodiscard]] bool verify(std::string_view key_id,
                            std::string_view signed_payload,
                            std::string_view signature) const override;
  [[nodiscard]] static std::string signature_for_test(
      std::string_view signed_payload);
};

[[nodiscard]] OtaManifestValidation validate_manifest(
    const OtaManifest& manifest, const OtaDeviceProfile& device,
    const SignatureVerifier& verifier);

/** Small dependency-free SHA-256 implementation used for image/hash seams. */
class Sha256Hasher {
 public:
  Sha256Hasher();
  void reset();
  void update(std::span<const std::uint8_t> bytes);
  [[nodiscard]] std::array<std::uint8_t, 32> digest() const;
  [[nodiscard]] std::string finalize_hex() const;

 private:
  void transform(const std::uint8_t* block);

  std::array<std::uint32_t, 8> state_{};
  std::array<std::uint8_t, 64> buffer_{};
  std::size_t buffer_size_ = 0;
  std::uint64_t bit_count_ = 0;
};

[[nodiscard]] std::string sha256_hex(std::span<const std::uint8_t> bytes);
[[nodiscard]] bool is_sha256_hex(std::string_view value);

}  // namespace t3::companion
