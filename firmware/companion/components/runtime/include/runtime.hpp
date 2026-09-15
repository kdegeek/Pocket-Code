#pragma once

#include "audio_frame.hpp"
#include "backpressure.hpp"
#include "board_manifest.h"
#include "button_input.hpp"
#include "credential_store.hpp"
#include "device_identity.hpp"
#include "es7210_capture.hpp"
#include "focus_queue.hpp"
#include "gesture_recognizer.hpp"
#include "http_client.hpp"
#include "interaction_fsm.hpp"
#include "pending_commands.hpp"
#include "ptt_button.hpp"
#include "reconnect.hpp"
#include "roaming.hpp"
#include "snapshot_store.hpp"
#include "sleep_lock.hpp"
#include "softap_portal.hpp"
#include "snapshot_model.hpp"
#include "transport.hpp"
#include "ambient_view.hpp"
#include "wifi_ws_client.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace t3::companion::runtime {

/** A small, platform-neutral clock seam. */
class RuntimeClock {
 public:
  virtual ~RuntimeClock() = default;
  [[nodiscard]] virtual std::uint64_t now_ms() const = 0;
  virtual void sleep_ms(std::uint32_t delay_ms) = 0;
};

enum class RuntimeInputKind : std::uint8_t { Touch, Button };

/**
 * Input messages are fixed-size and trivially copyable so the ESP-IDF adapter
 * can enqueue them from an ISR without allocating.  Socket payloads never
 * travel through this queue; they remain bounded protocol frames at the
 * WebSocket adapter boundary.
 */
struct RuntimeInputEvent {
  RuntimeInputKind kind = RuntimeInputKind::Touch;
  RawTouchSample touch;
  RawButtonSample button;
};

class RuntimeInputQueue {
 public:
  virtual ~RuntimeInputQueue() = default;
  [[nodiscard]] virtual bool push(const RuntimeInputEvent& event) = 0;
  [[nodiscard]] virtual bool pop(RuntimeInputEvent& event) = 0;
  [[nodiscard]] virtual std::size_t pending() const = 0;
  [[nodiscard]] virtual std::size_t capacity() const = 0;
};

class RuntimeCoordinator;

/** Host and fallback queue: bounded, allocation-free storage for input events. */
class FixedRuntimeInputQueue final : public RuntimeInputQueue {
 public:
  static constexpr std::size_t kCapacity = 32;

  [[nodiscard]] bool push(const RuntimeInputEvent& event) override;
  [[nodiscard]] bool pop(RuntimeInputEvent& event) override;
  [[nodiscard]] std::size_t pending() const override { return count_; }
  [[nodiscard]] std::size_t capacity() const override { return kCapacity; }

 private:
  std::array<RuntimeInputEvent, kCapacity> entries_{};
  std::size_t head_ = 0;
  std::size_t tail_ = 0;
  std::size_t count_ = 0;
};

/**
 * The board boundary owns BSP/LVGL, Wi-Fi driver, GPIO/PMIC input and the
 * microphone codec.  None of those side effects appear in the pure state
 * components or in host tests.
 */
class RuntimePlatform {
 public:
  virtual ~RuntimePlatform() = default;
  [[nodiscard]] virtual bool initialize() = 0;
  [[nodiscard]] virtual std::string device_id() const = 0;
  [[nodiscard]] virtual bool render(const t3::ui::UiModel& model) = 0;
  [[nodiscard]] virtual bool set_display_power(bool on) = 0;

  // Starting the station interface is separate from joining a network.  The
  // coordinator never calls either method until an enrolled gateway and an
  // explicit transport policy are present.
  [[nodiscard]] virtual bool wifi_start() = 0;
  [[nodiscard]] virtual bool wifi_connect(std::string_view ssid,
                                           std::string_view password) = 0;
  [[nodiscard]] virtual bool wifi_connected() const = 0;
  [[nodiscard]] virtual bool start_provisioning_ap(std::string_view ssid) = 0;

  // Enrollment metadata is short-lived and display-only.  The credential is
  // delivered by the still-open gateway session after T3 Code completes the
  // pairing RPC; it is never supplied through the local portal.
  virtual bool render_enrollment_pairing(std::string_view /*enrollment_id*/,
                                         std::string_view /*pairing_code*/,
                                         std::string_view /*expires_at*/) {
    return true;
  }

  [[nodiscard]] virtual bool audio_start(std::uint32_t sample_rate_hz) = 0;
  [[nodiscard]] virtual int audio_read(std::span<std::int16_t> interleaved_samples) = 0;
  virtual void audio_stop() = 0;

  // ISR/task adapters attach the bounded queue once during boot.
  virtual void attach_input_queue(RuntimeInputQueue& /*queue*/) {}

  // The ESP-IDF board adapter owns the device-local provisioning HTTP server.
  // Host fakes do not need to implement it; the coordinator attaches itself
  // before board initialization so an unpaired boot can expose setup as soon
  // as the SoftAP is ready.
  virtual void attach_provisioning_handler(RuntimeCoordinator& /*coordinator*/) {}
};

/**
 * Bootloader health seam.  The device adapter owns the ESP-IDF otadata call;
 * the coordinator only reports the verified gates that make a candidate safe
 * to mark valid.  Host tests can leave this unset or inject a recording fake.
 */
class RuntimeHealthSink {
 public:
  virtual ~RuntimeHealthSink() = default;
  virtual void mark_board_initialized() = 0;
  virtual void mark_display_initialized() = 0;
  virtual void mark_touch_initialized() = 0;
  virtual void mark_persistence_mounted() = 0;
  virtual void mark_handshake_succeeded() = 0;
  virtual void mark_offline_health_timeout() = 0;
};

enum class RuntimePhase : std::uint8_t {
  Booting,
  Provisioning,
  Offline,
  Connecting,
  Live,
  Reconnecting,
  Incompatible,
  Fault,
};

enum class RuntimeErrorCode : std::uint8_t {
  None,
  AlreadyBooted,
  BoardInitialization,
  Persistence,
  Provisioning,
  Configuration,
  Network,
  Protocol,
  Display,
  InputQueue,
};

struct RuntimeError {
  RuntimeErrorCode code = RuntimeErrorCode::None;
  std::string message;
};

struct RuntimeResult {
  RuntimeError error;
  [[nodiscard]] bool ok() const { return error.code == RuntimeErrorCode::None; }
  static RuntimeResult success() { return {}; }
  static RuntimeResult failure(RuntimeErrorCode code, std::string message) {
    return {{code, std::move(message)}};
  }
};

struct RuntimeConfig {
  // Transport remains opt-in even when durable enrollment data exists.  This
  // prevents an image built for local setup from contacting a stale gateway.
  bool transport_enabled = false;
  bool allow_trusted_lan = false;
  bool allow_remote = false;
  TransportSecurity security = TransportSecurity::TrustedLan;
  std::uint64_t reconnect_initial_ms = 250;
  std::uint64_t reconnect_max_ms = 30'000;
  std::uint32_t reconnect_jitter_percent = 20;
  std::uint32_t audio_sample_rate_hz = kPcmCaptureSampleRateHz;
  std::uint64_t ota_offline_health_timeout_ms = 30'000;
};

struct RuntimeDependencies {
  RuntimeClock& clock;
  RuntimePlatform& platform;
  DeviceIdentityStore& identity;
  CredentialStore& networks;
  PendingCommandStore& pending_commands;
  SnapshotStore& snapshots;
  WebSocketClient& websocket;
  HttpClient& http;
  TlsIdentityVerifier& tls_verifier;
  RuntimeInputQueue& input_queue;
  RuntimeHealthSink* health = nullptr;
};

/**
 * Real-device composition for the T3 Companion.  Boot always restores and
 * renders the display-only cache before any configured network attempt.  A
 * hello acknowledgement is the sole transition that enables gestures and
 * mutating commands.
 */
class RuntimeCoordinator {
 public:
  explicit RuntimeCoordinator(RuntimeDependencies dependencies,
                              RuntimeConfig config = {});

  [[nodiscard]] RuntimeResult boot();
  [[nodiscard]] RuntimeResult tick();

  [[nodiscard]] RuntimeResult on_socket_text(std::string_view frame);
  [[nodiscard]] RuntimeResult on_hello_ack(const HelloAck& ack);
  [[nodiscard]] RuntimeResult on_socket_closed();
  [[nodiscard]] RuntimeResult on_gateway_lost();
  [[nodiscard]] RuntimeResult on_access_point_lost();
  [[nodiscard]] RuntimeResult on_transcript_proposed(std::string_view transcript);

  [[nodiscard]] RuntimeResult configure_gateway(std::string_view gateway_url,
                                                 TransportSecurity security);
  // Applies one explicit local setup submission. An empty gateway origin is
  // valid: it means "remember Wi-Fi only" and must not be confused with an
  // invalid configured endpoint.
  [[nodiscard]] RuntimeResult submit_provisioning(std::string_view ssid,
                                                  std::string_view password,
                                                  std::string_view gateway_url,
                                                  TransportSecurity security);
  [[nodiscard]] RuntimeResult accept_enrollment_token(std::string_view token,
                                                      std::uint64_t expires_at_ms);
  [[nodiscard]] RuntimeResult begin_provisioning();
  [[nodiscard]] RuntimeResult append_wifi_network(std::string_view ssid,
                                                  std::string_view password);

  // Sanitized, bounded status for the device-local setup page.  It contains
  // no Wi-Fi passphrase, enrollment token, audio, transcript, or prompt data.
  [[nodiscard]] std::string provisioning_status_json() const;

  [[nodiscard]] bool enqueue(const RuntimeInputEvent& event);
  [[nodiscard]] bool enqueue_touch(const RawTouchSample& sample);
  [[nodiscard]] bool enqueue_button(const RawButtonSample& sample);

  [[nodiscard]] RuntimePhase phase() const { return phase_; }
  [[nodiscard]] RuntimeError last_error() const { return last_error_; }
  [[nodiscard]] bool booted() const { return booted_; }
  [[nodiscard]] std::size_t remembered_network_count() const { return networks_.size(); }
  [[nodiscard]] bool gestures_enabled() const { return transport_.gestures_enabled(); }
  [[nodiscard]] const Transport& transport() const { return transport_; }
  [[nodiscard]] const InteractionState& interaction() const { return interaction_; }
  [[nodiscard]] const SnapshotModel& model() const { return model_; }
  [[nodiscard]] const t3::ui::UiModel& ui_model() const { return ui_model_; }

 private:
  [[nodiscard]] RuntimeResult fail(RuntimeErrorCode code, std::string message);
  [[nodiscard]] bool paired_identity() const;
  [[nodiscard]] bool transport_policy_allows() const;
  [[nodiscard]] RuntimeResult ensure_identity();
  [[nodiscard]] RuntimeResult render();
  [[nodiscard]] RuntimeResult start_configured_transport();
  [[nodiscard]] RuntimeResult connect_if_ready();
  [[nodiscard]] RuntimeResult process_input(const RuntimeInputEvent& event);
  [[nodiscard]] RuntimeResult process_touch(const RawTouchSample& sample);
  [[nodiscard]] RuntimeResult process_button(const RawButtonSample& sample);
  [[nodiscard]] RuntimeResult process_button_events(const std::vector<ButtonEvent>& events);
  [[nodiscard]] RuntimeResult process_ptt_effects(const std::vector<PttEffect>& effects);
  [[nodiscard]] RuntimeResult process_interaction(const InteractionEvent& event);
  [[nodiscard]] RuntimeResult process_effects(const std::vector<Effect>& effects);
  [[nodiscard]] RuntimeResult send_aux_command(DeviceCommand command);
  [[nodiscard]] RuntimeResult start_audio_stream(std::uint64_t now_ms);
  [[nodiscard]] RuntimeResult stop_audio_stream(std::uint64_t now_ms);
  [[nodiscard]] RuntimeResult pump_audio();
  [[nodiscard]] RuntimeResult apply_snapshot(const Snapshot& snapshot);
  [[nodiscard]] RuntimeResult apply_hello_ack(const HelloAck& ack);
  [[nodiscard]] RuntimeResult open_provisioning_ap();
  [[nodiscard]] RuntimeResult start_enrollment_transport();
  [[nodiscard]] RuntimeResult handle_enrollment_text(std::string_view frame);
  [[nodiscard]] bool enrollment_policy_allows() const;
  [[nodiscard]] Snapshot cached_snapshot() const;
  void set_connectivity(bool live);
  void update_gesture_context();
  void mark_offline(RuntimePhase phase);
  void maybe_mark_offline_health();

  RuntimeDependencies dependencies_;
  RuntimeConfig config_;
  DeviceIdentityStore& identity_;
  CredentialStore& networks_;
  PendingCommandStore& pending_commands_;
  SnapshotStore& snapshots_;
  RuntimePlatform& platform_;
  RuntimeInputQueue& input_queue_;

  CommandOutbox transport_outbox_;
  Transport transport_;
  SoftApPortal portal_;
  RoamingController roaming_;
  ButtonInput buttons_;
  SleepLockController lock_;
  PttButtonController ptt_;
  GestureRecognizer gestures_;
  Es7210Capture capture_;
  std::optional<AudioStreamSession> audio_stream_;

  SnapshotModel model_;
  FocusQueueState focus_;
  UsageSnapshot usage_;
  InteractionState interaction_;
  t3::ui::UiModel ui_model_;
  RuntimePhase phase_ = RuntimePhase::Booting;
  RuntimeError last_error_;
  Backoff reconnect_backoff_;
  std::uint64_t next_connect_ms_ = 0;
  std::uint64_t boot_started_ms_ = 0;
  bool offline_health_reported_ = false;
  bool booted_ = false;
  bool wifi_started_ = false;
  bool transport_configured_ = false;
  bool provisioning_ap_started_ = false;
  bool enrollment_requested_ = false;
  bool enrollment_socket_open_ = false;
  std::string enrollment_id_;
  std::string pairing_code_;
  std::string enrollment_expires_at_;
};

}  // namespace t3::companion::runtime
