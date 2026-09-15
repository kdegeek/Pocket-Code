#include "rollback.hpp"

namespace t3::companion {

bool MemoryBootStateStore::load(BootState& state) const {
  if (!initialized_) return false;
  state = state_;
  return true;
}

bool MemoryBootStateStore::save(const BootState& state) {
  state_ = state;
  initialized_ = true;
  return true;
}

OtaSlotManager::OtaSlotManager(const OtaDeviceProfile& device,
                               const OtaPartitionLayout& layout,
                               BootStateStore& state_store,
                               std::uint64_t health_timeout_ms,
                               const SignatureVerifier* verifier)
    : device_(device),
      layout_(layout),
      state_store_(state_store),
      verifier_(verifier),
      health_timeout_ms_(health_timeout_ms) {
  if (!state_store_.load(state_)) {
    // The bootloader's otadata record is the source of truth on device. A
    // fresh host journal starts from ota_0 until that record is loaded.
    state_.active_slot = OtaSlot::Ota0;
    state_.previous_slot = state_.active_slot;
    state_.storage_generation = 0;
  }
  initial_storage_generation_ = state_.storage_generation;
}

OtaSlot OtaSlotManager::select_target() const {
  return inactive_slot(state_.active_slot);
}

OtaRecoveryResult OtaSlotManager::persist() {
  if (!state_store_.save(state_)) {
    return OtaRecoveryResult::failure(OtaRecoveryErrorCode::Persistence,
                                      "boot state journal rejected update");
  }
  return OtaRecoveryResult::success();
}

OtaRecoveryResult OtaSlotManager::validate_for_stage(
    const OtaManifest& manifest) const {
  if (state_.pending_slot.has_value()) {
    return OtaRecoveryResult::failure(OtaRecoveryErrorCode::AlreadyPending,
                                      "an OTA candidate is already pending");
  }
  if (verifier_ == nullptr) {
    return OtaRecoveryResult::failure(
        OtaRecoveryErrorCode::ManifestRejected,
        "a release signature verifier is required before staging OTA");
  }
  const auto validation = validate_manifest(manifest, device_, *verifier_);
  if (!validation.ok()) {
    return OtaRecoveryResult::failure(
        OtaRecoveryErrorCode::ManifestRejected, validation.error.message);
  }
  if (manifest.slot_size_bytes != layout_.slot_size_bytes ||
      manifest.storage_offset != layout_.storage_offset ||
      manifest.storage_size_bytes != layout_.storage_size_bytes) {
    return OtaRecoveryResult::failure(
        OtaRecoveryErrorCode::ManifestRejected,
        "manifest partition geometry would alter the approved layout");
  }
  return OtaRecoveryResult::success();
}

OtaRecoveryResult OtaSlotManager::stage(const OtaManifest& manifest) {
  const auto validation = validate_for_stage(manifest);
  if (!validation.ok()) return validation;
  const auto old_state = state_;
  state_.previous_slot = state_.active_slot;
  state_.pending_slot = select_target();
  state_.staged_version = manifest.version;
  state_.healthy = false;
  const auto result = persist();
  if (!result.ok()) state_ = old_state;
  return result;
}

OtaRecoveryResult OtaSlotManager::on_boot(std::uint64_t now_ms) {
  if (!state_.pending_slot.has_value()) return OtaRecoveryResult::success();
  const auto old_state = state_;
  state_.active_slot = *state_.pending_slot;
  state_.healthy = false;
  health_ = {};
  boot_started_at_ms_ = now_ms;
  booting_candidate_ = true;
  const auto result = persist();
  if (!result.ok()) {
    state_ = old_state;
    booting_candidate_ = false;
  }
  return result;
}

OtaRecoveryResult OtaSlotManager::mark_offline_health_timeout(
    std::uint64_t now_ms) {
  if (!booting_candidate_) {
    return OtaRecoveryResult::failure(OtaRecoveryErrorCode::InvalidState,
                                      "no candidate boot is being validated");
  }
  if (now_ms < boot_started_at_ms_ ||
      now_ms - boot_started_at_ms_ < health_timeout_ms_) {
    return OtaRecoveryResult::failure(
        OtaRecoveryErrorCode::HealthCriteria,
        "offline health timeout has not elapsed");
  }
  health_.offline_health_timeout = true;
  return OtaRecoveryResult::success();
}

OtaRecoveryResult OtaSlotManager::mark_healthy() {
  if (!booting_candidate_ || !state_.pending_slot.has_value()) {
    return OtaRecoveryResult::failure(OtaRecoveryErrorCode::InvalidState,
                                      "no pending candidate is being validated");
  }
  if (!health_.ready()) {
    return OtaRecoveryResult::failure(
        OtaRecoveryErrorCode::HealthCriteria,
        "board, display, touch, persistence, and handshake/offline health are required");
  }
  const auto accepted_version = state_.staged_version;
  state_.healthy = true;
  state_.pending_slot.reset();
  state_.staged_version = 0;
  if (accepted_version > device_.current_version) {
    device_.current_version = accepted_version;
  }
  booting_candidate_ = false;
  return persist();
}

OtaRecoveryResult OtaSlotManager::rollback() {
  if (!booting_candidate_ && !state_.pending_slot.has_value()) {
    return OtaRecoveryResult::failure(OtaRecoveryErrorCode::InvalidState,
                                      "no candidate is available for rollback");
  }
  state_.active_slot = state_.previous_slot;
  state_.pending_slot.reset();
  state_.staged_version = 0;
  state_.healthy = true;
  health_ = {};
  booting_candidate_ = false;
  return persist();
}

OtaRecoveryResult OtaSlotManager::tick(std::uint64_t now_ms) {
  if (!booting_candidate_) return OtaRecoveryResult::success();
  if (now_ms < boot_started_at_ms_ ||
      now_ms - boot_started_at_ms_ < health_timeout_ms_) {
    return OtaRecoveryResult::success();
  }
  const auto rollback_result = rollback();
  if (!rollback_result.ok()) return rollback_result;
  return OtaRecoveryResult::failure(
      OtaRecoveryErrorCode::BootTimeout,
      "candidate did not meet boot health criteria before timeout; rolled back");
}

}  // namespace t3::companion
