#pragma once

#include "command_outbox.hpp"
#include "device_identity.hpp"
#include "http_client.hpp"
#include "pending_commands.hpp"
#include "snapshot_store.hpp"
#include "wifi_ws_client.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace t3::companion {

enum class TransportSecurity { TrustedLan, Remote };

struct CompanionRoutes {
  std::string enroll;
  std::string snapshot;
  std::string ws;
};

enum class TransportErrorCode {
  None,
  InvalidEndpoint,
  InsecureRemote,
  MissingIdentity,
  Authentication,
  Revoked,
  Offline,
  Incompatible,
  Protocol,
  RetentionMiss,
  SnapshotBeforeLive,
  HandshakeRequired,
  Storage,
  Network,
  TlsIdentity,
  EnrollmentExpired,
  InvalidState,
};

struct TransportError {
  TransportErrorCode code = TransportErrorCode::None;
  std::string message;
};

struct TransportResult {
  TransportError error;
  [[nodiscard]] bool ok() const { return error.code == TransportErrorCode::None; }
  static TransportResult success() { return {}; }
  static TransportResult failure(TransportErrorCode code, std::string message) {
    return {{code, std::move(message)}};
  }
};

struct RouteResult {
  CompanionRoutes value;
  TransportError error;
  [[nodiscard]] bool ok() const { return error.code == TransportErrorCode::None; }
};

/**
 * Build the only three firmware routes.  A trusted-LAN gateway is explicitly
 * plain HTTP/WS; a remote gateway is explicitly HTTPS/WSS.  Existing paths
 * (including T3's general `/ws`) are rejected rather than silently joined.
 */
[[nodiscard]] RouteResult build_companion_routes(std::string_view gateway_url,
                                                 TransportSecurity security);

/** Short-lived enrollment material; it is never confused with general T3 credentials. */
class PairingTokenStore {
 public:
  [[nodiscard]] bool put_pairing(std::string_view enrollment_id,
                                 std::string_view pairing_code,
                                 std::uint64_t expires_at_ms);
  [[nodiscard]] bool pairing_matches(std::string_view enrollment_id,
                                     std::string_view pairing_code,
                                     std::uint64_t now_ms) const;
  /** Consume the one-time pairing code after a successful exchange. */
  [[nodiscard]] bool consume_pairing(std::string_view enrollment_id,
                                      std::string_view pairing_code,
                                      std::uint64_t now_ms);
  [[nodiscard]] bool put_token(std::string_view token, std::uint64_t expires_at_ms);
  [[nodiscard]] bool has_valid_token(std::uint64_t now_ms) const;
  [[nodiscard]] std::string_view token() const { return token_; }
  [[nodiscard]] std::uint64_t token_expires_at_ms() const { return token_expires_at_ms_; }
  void revoke();
  void clear_pairing();

 private:
  std::string enrollment_id_;
  std::string pairing_code_;
  std::uint64_t pairing_expires_at_ms_ = 0;
  std::string token_;
  std::uint64_t token_expires_at_ms_ = 0;
  bool revoked_ = false;
};

enum class TransportStatus {
  Unconfigured,
  Enrolling,
  Connecting,
  Live,
  Reconnecting,
  Offline,
  Revoked,
  Incompatible,
};

enum class NetworkLoss { None, Gateway, AccessPoint };

struct TransportDependencies {
  DeviceIdentityStore* identity = nullptr;
  PendingCommandStore* pending_commands = nullptr;
  SnapshotStore* snapshot_store = nullptr;
  CommandOutbox* command_outbox = nullptr;
  WebSocketClient* websocket = nullptr;
  HttpClient* http = nullptr;
  TlsIdentityVerifier* tls_verifier = nullptr;
  std::function<std::uint64_t()> now_ms;
};

/**
 * Authenticated companion session coordinator.  It has no socket or HTTP
 * implementation of its own: ESP-IDF adapters and host fakes implement the
 * two narrow client interfaces above.
 */
class Transport {
 public:
  explicit Transport(TransportDependencies dependencies);

  [[nodiscard]] TransportResult configure(TransportSecurity security);
  [[nodiscard]] TransportResult start_enrollment();
  [[nodiscard]] TransportResult accept_enrollment_credential(std::string_view token,
                                                              std::uint64_t expires_at_ms);
  [[nodiscard]] TransportResult load_cache();
  [[nodiscard]] TransportResult connect();
  [[nodiscard]] TransportResult on_socket_open();
  [[nodiscard]] TransportResult on_hello_ack(const HelloAck& ack);
  [[nodiscard]] TransportResult request_snapshot();
  [[nodiscard]] TransportResult on_snapshot(const Snapshot& snapshot);
  [[nodiscard]] TransportResult on_server_event(const ServerEvent& event);
  [[nodiscard]] TransportResult receive_text(std::string_view frame);
  [[nodiscard]] TransportResult submit_command(const DeviceCommand& command);
  [[nodiscard]] TransportResult on_socket_closed();
  [[nodiscard]] TransportResult on_gateway_lost();
  [[nodiscard]] TransportResult on_access_point_lost();
  [[nodiscard]] TransportResult on_revoked();
  [[nodiscard]] TransportResult on_protocol_error(std::uint32_t server_min,
                                                  std::uint32_t server_max);

  [[nodiscard]] TransportStatus status() const { return status_; }
  [[nodiscard]] NetworkLoss network_loss() const { return network_loss_; }
  [[nodiscard]] bool gestures_enabled() const { return gestures_enabled_; }
  [[nodiscard]] bool cached_prompts_display_only() const {
    return !gestures_enabled_ || cached_prompts_display_only_;
  }
  [[nodiscard]] bool snapshot_required() const { return snapshot_required_; }
  [[nodiscard]] bool live_attached() const { return live_attached_; }
  [[nodiscard]] bool cache_is_stale(std::uint64_t max_age_ms = 15ULL * 60ULL * 1'000ULL) const;
  [[nodiscard]] const CompanionRoutes& routes() const { return routes_; }
  [[nodiscard]] const PairingTokenStore& credentials() const { return credentials_; }
  [[nodiscard]] const std::optional<Snapshot>& live_snapshot() const { return live_snapshot_; }
  [[nodiscard]] const ReplayGuard& replay_guard() const { return replay_guard_; }

 private:
  [[nodiscard]] std::uint64_t now() const;
  [[nodiscard]] TransportResult failure(TransportErrorCode code, std::string message) const;
  [[nodiscard]] TransportResult build_hello(Hello& hello) const;
  [[nodiscard]] TransportResult persist_status(const CommandStatusRecord& record);
  [[nodiscard]] TransportResult apply_snapshot_atomically(const Snapshot& snapshot);
  [[nodiscard]] TransportResult verify_remote_identity() const;
  [[nodiscard]] static std::string host_from_url(std::string_view url);

  TransportDependencies dependencies_;
  PairingTokenStore credentials_;
  CompanionRoutes routes_;
  TransportSecurity security_ = TransportSecurity::TrustedLan;
  TransportStatus status_ = TransportStatus::Unconfigured;
  NetworkLoss network_loss_ = NetworkLoss::None;
  std::uint64_t now_fallback_ms_ = 0;
  bool configured_ = false;
  bool hello_sent_ = false;
  bool hello_reconciled_ = false;
  bool live_attached_ = false;
  bool revoked_ = false;
  bool snapshot_required_ = false;
  bool gestures_enabled_ = false;
  bool cached_prompts_display_only_ = true;
  bool cache_loaded_ = false;
  bool cache_forced_stale_ = true;
  std::uint64_t cache_last_seen_ms_ = 0;
  Id session_id_;
  std::optional<Snapshot> live_snapshot_;
  ReplayGuard replay_guard_;
};

}  // namespace t3::companion
