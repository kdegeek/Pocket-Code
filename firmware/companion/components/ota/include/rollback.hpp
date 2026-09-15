#pragma once

#include "manifest.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace t3::companion {

enum class OtaSlot { Ota0, Ota1 };

[[nodiscard]] constexpr OtaSlot inactive_slot(OtaSlot active) {
  return active == OtaSlot::Ota0 ? OtaSlot::Ota1 : OtaSlot::Ota0;
}

struct OtaPartitionLayout {
  std::uint32_t ota0_offset = 0x20000;
  std::uint32_t ota1_offset = 0x720000;
  std::uint32_t slot_size_bytes = 0x700000;
  std::uint32_t storage_offset = 0xe20000;
  std::uint32_t storage_size_bytes = 0x1d0000;
};

struct BootState {
  OtaSlot active_slot = OtaSlot::Ota0;
  std::optional<OtaSlot> pending_slot;
  OtaSlot previous_slot = OtaSlot::Ota0;
  std::uint64_t staged_version = 0;
  bool healthy = true;
  std::uint64_t storage_generation = 0;
};

/** NVS/otadata adapters implement this journal boundary on the device. */
class BootStateStore {
 public:
  virtual ~BootStateStore() = default;
  [[nodiscard]] virtual bool load(BootState& state) const = 0;
  [[nodiscard]] virtual bool save(const BootState& state) = 0;
};

/** Deterministic host seam; it does not touch flash or an ESP-IDF partition. */
class MemoryBootStateStore final : public BootStateStore {
 public:
  [[nodiscard]] bool load(BootState& state) const override;
  [[nodiscard]] bool save(const BootState& state) override;
  [[nodiscard]] const BootState& value() const { return state_; }

 private:
  BootState state_{};
  bool initialized_ = false;
};

enum class OtaRecoveryErrorCode {
  None,
  InvalidState,
  ManifestRejected,
  AlreadyPending,
  Persistence,
  HealthCriteria,
  BootTimeout,
};

struct OtaRecoveryError {
  OtaRecoveryErrorCode code = OtaRecoveryErrorCode::None;
  std::string message;
};

struct OtaRecoveryResult {
  OtaRecoveryError error;
  [[nodiscard]] bool ok() const {
    return error.code == OtaRecoveryErrorCode::None;
  }
  static OtaRecoveryResult success() { return {}; }
  static OtaRecoveryResult failure(OtaRecoveryErrorCode code,
                                   std::string message) {
    return {{code, std::move(message)}};
  }
};

struct BootHealthCriteria {
  bool board_initialized = false;
  bool display_initialized = false;
  bool touch_initialized = false;
  bool persistence_mounted = false;
  bool handshake_succeeded = false;
  bool offline_health_timeout = false;

  [[nodiscard]] bool ready() const {
    return board_initialized && display_initialized && touch_initialized &&
           persistence_mounted && (handshake_succeeded || offline_health_timeout);
  }
};

/**
 * A/B activation and recovery policy. Only the app slot in otadata changes;
 * storage geometry and generation are carried through every transition.
 */
class OtaSlotManager {
 public:
  OtaSlotManager(const OtaDeviceProfile& device, const OtaPartitionLayout& layout,
                 BootStateStore& state_store, std::uint64_t health_timeout_ms,
                 const SignatureVerifier* verifier = nullptr);

  [[nodiscard]] OtaSlot select_target() const;
  [[nodiscard]] OtaRecoveryResult stage(const OtaManifest& manifest);
  [[nodiscard]] OtaRecoveryResult on_boot(std::uint64_t now_ms);
  [[nodiscard]] OtaRecoveryResult mark_offline_health_timeout(
      std::uint64_t now_ms);
  [[nodiscard]] OtaRecoveryResult mark_healthy();
  [[nodiscard]] OtaRecoveryResult rollback();
  [[nodiscard]] OtaRecoveryResult tick(std::uint64_t now_ms);

  void mark_board_initialized() { health_.board_initialized = true; }
  void mark_display_initialized() { health_.display_initialized = true; }
  void mark_touch_initialized() { health_.touch_initialized = true; }
  void mark_persistence_mounted() { health_.persistence_mounted = true; }
  void mark_handshake_succeeded() { health_.handshake_succeeded = true; }

  [[nodiscard]] OtaSlot active_slot() const { return state_.active_slot; }
  [[nodiscard]] std::optional<OtaSlot> pending_slot() const {
    return state_.pending_slot;
  }
  [[nodiscard]] const BootHealthCriteria& health() const { return health_; }
  [[nodiscard]] bool storage_unchanged() const {
    return state_.storage_generation == initial_storage_generation_;
  }
  [[nodiscard]] std::uint32_t storage_offset() const {
    return layout_.storage_offset;
  }
  [[nodiscard]] std::uint32_t storage_size_bytes() const {
    return layout_.storage_size_bytes;
  }

 private:
  [[nodiscard]] OtaRecoveryResult persist();
  [[nodiscard]] OtaRecoveryResult validate_for_stage(
      const OtaManifest& manifest) const;

  OtaDeviceProfile device_;
  OtaPartitionLayout layout_;
  BootStateStore& state_store_;
  const SignatureVerifier* verifier_ = nullptr;
  BootState state_;
  BootHealthCriteria health_;
  std::uint64_t health_timeout_ms_ = 0;
  std::uint64_t boot_started_at_ms_ = 0;
  std::uint64_t initial_storage_generation_ = 0;
  bool booting_candidate_ = false;
};

}  // namespace t3::companion
