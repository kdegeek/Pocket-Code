#include "transport.hpp"

#include "snapshot_model.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <variant>

namespace t3::companion {
namespace {

constexpr std::string_view kEnrollPath = "/api/companion/v1/enroll";
constexpr std::string_view kSnapshotPath = "/api/companion/v1/snapshot";
constexpr std::string_view kWsPath = "/api/companion/v1/ws";

bool has_space_or_control(std::string_view value) {
  for (const unsigned char byte : value) {
    if (std::isspace(byte) != 0 || byte < 0x20U || byte == 0x7fU) return true;
  }
  return false;
}

struct ParsedUrl {
  std::string_view scheme;
  std::string_view authority;
  std::string_view path;
};

std::optional<ParsedUrl> parse_gateway_url(std::string_view url) {
  const auto scheme_end = url.find("://");
  if (scheme_end == std::string_view::npos || scheme_end == 0) return std::nullopt;
  const auto authority_start = scheme_end + 3U;
  const auto path_start = url.find_first_of("/?#", authority_start);
  const auto authority_end = path_start == std::string_view::npos ? url.size() : path_start;
  if (authority_end <= authority_start) return std::nullopt;
  const auto path = path_start == std::string_view::npos ? std::string_view{} : url.substr(path_start);
  if (path.find_first_of("?#") != std::string_view::npos) return std::nullopt;
  if (has_space_or_control(url)) return std::nullopt;
  if (path != std::string_view{} && path != std::string_view{"/"}) return std::nullopt;
  return ParsedUrl{url.substr(0, scheme_end), url.substr(authority_start, authority_end - authority_start), path};
}

std::string join_route(std::string_view scheme, std::string_view authority,
                       std::string_view path) {
  std::string result;
  result.reserve(scheme.size() + 3U + authority.size() + path.size());
  result.append(scheme);
  result.append("://");
  result.append(authority);
  result.append(path);
  return result;
}

std::string query_escape(std::string_view value) {
  constexpr char kHex[] = "0123456789ABCDEF";
  std::string result;
  result.reserve(value.size());
  for (const unsigned char byte : value) {
    const bool safe = (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
                      (byte >= '0' && byte <= '9') || byte == '-' || byte == '_' ||
                      byte == '.' || byte == '~';
    if (safe) {
      result.push_back(static_cast<char>(byte));
    } else {
      result.push_back('%');
      result.push_back(kHex[(byte >> 4U) & 0x0FU]);
      result.push_back(kHex[byte & 0x0FU]);
    }
  }
  return result;
}

void append_json_string(std::string& output, std::string_view value) {
  output.push_back('"');
  for (const char character : value) {
    switch (character) {
      case '"': output += "\\\""; break;
      case '\\': output += "\\\\"; break;
      case '\n': output += "\\n"; break;
      case '\r': output += "\\r"; break;
      case '\t': output += "\\t"; break;
      default: output.push_back(character); break;
    }
  }
  output.push_back('"');
}

void append_key(std::string& output, std::string_view key, bool& first) {
  if (!first) output.push_back(',');
  first = false;
  append_json_string(output, key);
  output.push_back(':');
}

std::string_view command_type_name(DeviceCommandType type) {
  switch (type) {
    case DeviceCommandType::AnswerPrompt: return "answer_prompt";
    case DeviceCommandType::CancelTurn: return "cancel_turn";
    case DeviceCommandType::SttStart: return "stt_start";
    case DeviceCommandType::SttStop: return "stt_stop";
    case DeviceCommandType::SubmitTranscript: return "submit_transcript";
    case DeviceCommandType::DiscardTranscript: return "discard_transcript";
    case DeviceCommandType::ProvisioningStatus: return "provisioning_status";
  }
  return "cancel_turn";
}

std::string_view decision_name(ApprovalDecision decision) {
  switch (decision) {
    case ApprovalDecision::Accept: return "accept";
    case ApprovalDecision::Decline: return "decline";
    case ApprovalDecision::Cancel: return "cancel";
  }
  return "cancel";
}

std::string_view provisioning_status_name(ProvisioningStatus status) {
  switch (status) {
    case ProvisioningStatus::Started: return "started";
    case ProvisioningStatus::Completed: return "completed";
    case ProvisioningStatus::Cancelled: return "cancelled";
    case ProvisioningStatus::Error: return "error";
  }
  return "error";
}

void append_answer_value(std::string& output, const AnswerValue& value) {
  switch (value.kind) {
    case AnswerValue::Kind::String:
      append_json_string(output, value.string_value.view());
      return;
    case AnswerValue::Kind::Boolean:
      output += value.boolean_value ? "true" : "false";
      return;
    case AnswerValue::Kind::Number:
      output += std::to_string(value.number_value);
      return;
    case AnswerValue::Kind::Strings:
      output.push_back('[');
      for (std::size_t index = 0; index < value.string_values.count; ++index) {
        if (index != 0) output.push_back(',');
        append_json_string(output, value.string_values[index].view());
      }
      output.push_back(']');
      return;
  }
}

void append_command_context(std::string& output, const DeviceCommand& command, bool& first) {
  append_key(output, "environmentId", first);
  append_json_string(output, command.context.environment_id.view());
  append_key(output, "projectId", first);
  append_json_string(output, command.context.project_id.view());
  append_key(output, "threadId", first);
  append_json_string(output, command.context.thread_id.view());
  append_key(output, "expectedTurnId", first);
  append_json_string(output, command.context.expected_turn_id.view());
  if (command.context.request_id.has_value()) {
    append_key(output, "requestId", first);
    append_json_string(output, command.context.request_id->view());
  }
}

DecodeResult<std::string> encode_command_frame(const DeviceCommand& command) {
  DecodeResult<std::string> result;
  std::string output;
  output.reserve(512);
  output.push_back('{');
  bool first = true;
  append_key(output, "type", first);
  append_json_string(output, command_type_name(command.type));
  append_key(output, "protocolVersion", first);
  output += std::to_string(command.protocol_version);
  append_key(output, "commandId", first);
  append_json_string(output, command.command_id.view());
  append_key(output, "deviceId", first);
  append_json_string(output, command.device_id.view());
  if (command.type != DeviceCommandType::ProvisioningStatus) {
    append_command_context(output, command, first);
  }
  switch (command.type) {
    case DeviceCommandType::AnswerPrompt:
      if (command.payload_kind == DeviceCommandPayload::Decision) {
        append_key(output, "decision", first);
        append_json_string(output, decision_name(command.decision));
      } else {
        append_key(output, "answers", first);
        output.push_back('{');
        bool answer_first = true;
        for (std::size_t index = 0; index < command.answers.entries.count; ++index) {
          const auto& entry = command.answers.entries[index];
          append_key(output, entry.key.view(), answer_first);
          append_answer_value(output, entry.value);
        }
        output.push_back('}');
      }
      break;
    case DeviceCommandType::SttStart:
    case DeviceCommandType::SttStop:
      append_key(output, "sttStreamId", first);
      append_json_string(output, command.stt_stream_id.view());
      break;
    case DeviceCommandType::SubmitTranscript:
      append_key(output, "sttStreamId", first);
      append_json_string(output, command.stt_stream_id.view());
      append_key(output, "transcriptId", first);
      append_json_string(output, command.transcript_id.view());
      append_key(output, "transcript", first);
      append_json_string(output, command.transcript.view());
      break;
    case DeviceCommandType::DiscardTranscript:
      append_key(output, "sttStreamId", first);
      append_json_string(output, command.stt_stream_id.view());
      append_key(output, "transcriptId", first);
      append_json_string(output, command.transcript_id.view());
      break;
    case DeviceCommandType::ProvisioningStatus:
      append_key(output, "provisioningRequestId", first);
      append_json_string(output, command.provisioning_request_id.view());
      append_key(output, "status", first);
      append_json_string(output, provisioning_status_name(command.provisioning_status));
      if (command.error.has_value()) {
        append_key(output, "error", first);
        append_json_string(output, command.error->view());
      }
      break;
    case DeviceCommandType::CancelTurn:
      break;
  }
  output.push_back('}');
  if (output.size() > kMaxJsonBytes) {
    result.error = {DecodeError::LimitExceeded, kMaxJsonBytes,
                    "encoded command exceeds maximum bytes"};
    return result;
  }
  result.value = std::move(output);
  return result;
}

}  // namespace

RouteResult build_companion_routes(std::string_view gateway_url,
                                   TransportSecurity security) {
  RouteResult result;
  const auto parsed = parse_gateway_url(gateway_url);
  if (!parsed.has_value()) {
    result.error = {TransportErrorCode::InvalidEndpoint,
                    "gateway must be an origin without a path, query, or fragment"};
    return result;
  }
  const bool trusted_scheme = parsed->scheme == "http";
  const bool remote_scheme = parsed->scheme == "https";
  if (security == TransportSecurity::TrustedLan && !trusted_scheme) {
    result.error = {TransportErrorCode::InvalidEndpoint,
                    "trusted-LAN companion transport requires an explicit http origin"};
    return result;
  }
  if (security == TransportSecurity::Remote && !remote_scheme) {
    result.error = {TransportErrorCode::InsecureRemote,
                    "remote companion transport requires an explicit https origin"};
    return result;
  }
  const auto http_scheme = security == TransportSecurity::Remote ? "https" : "http";
  const auto ws_scheme = security == TransportSecurity::Remote ? "wss" : "ws";
  // Enrollment is a WebSocket handshake even though the gateway is supplied
  // as an origin-only HTTP/HTTPS URL.  Keep snapshot on HTTP(S), but use the
  // corresponding WS(S) scheme for both socket routes so ESP-IDF's websocket
  // client receives a valid URI.
  result.value.enroll = join_route(ws_scheme, parsed->authority, kEnrollPath);
  result.value.snapshot = join_route(http_scheme, parsed->authority, kSnapshotPath);
  result.value.ws = join_route(ws_scheme, parsed->authority, kWsPath);
  return result;
}

bool PairingTokenStore::put_pairing(std::string_view enrollment_id,
                                    std::string_view pairing_code,
                                    std::uint64_t expires_at_ms) {
  if (enrollment_id.empty() || pairing_code.empty() || expires_at_ms == 0) return false;
  enrollment_id_.assign(enrollment_id);
  pairing_code_.assign(pairing_code);
  pairing_expires_at_ms_ = expires_at_ms;
  return true;
}

bool PairingTokenStore::pairing_matches(std::string_view enrollment_id,
                                        std::string_view pairing_code,
                                        std::uint64_t now_ms) const {
  return !revoked_ && !enrollment_id_.empty() && enrollment_id_ == enrollment_id &&
         pairing_code_ == pairing_code && now_ms < pairing_expires_at_ms_;
}

bool PairingTokenStore::consume_pairing(std::string_view enrollment_id,
                                        std::string_view pairing_code,
                                        std::uint64_t now_ms) {
  if (!pairing_matches(enrollment_id, pairing_code, now_ms)) return false;
  clear_pairing();
  return true;
}

bool PairingTokenStore::put_token(std::string_view token, std::uint64_t expires_at_ms) {
  if (token.empty() || expires_at_ms == 0) return false;
  token_.assign(token);
  token_expires_at_ms_ = expires_at_ms;
  revoked_ = false;
  return true;
}

bool PairingTokenStore::has_valid_token(std::uint64_t now_ms) const {
  return !revoked_ && !token_.empty() && now_ms < token_expires_at_ms_;
}

void PairingTokenStore::revoke() {
  token_.clear();
  token_expires_at_ms_ = 0;
  revoked_ = true;
}

void PairingTokenStore::clear_pairing() {
  enrollment_id_.clear();
  pairing_code_.clear();
  pairing_expires_at_ms_ = 0;
}

Transport::Transport(TransportDependencies dependencies)
    : dependencies_(std::move(dependencies)) {
  if (!dependencies_.now_ms) {
    dependencies_.now_ms = [this]() { return now_fallback_ms_; };
  }
}

std::uint64_t Transport::now() const { return dependencies_.now_ms(); }

TransportResult Transport::failure(TransportErrorCode code, std::string message) const {
  return TransportResult::failure(code, std::move(message));
}

std::string Transport::host_from_url(std::string_view url) {
  const auto parsed = parse_gateway_url(url);
  return parsed.has_value() ? std::string(parsed->authority) : std::string{};
}

TransportResult Transport::verify_remote_identity() const {
  if (security_ != TransportSecurity::Remote) return TransportResult::success();
  if (dependencies_.tls_verifier == nullptr) {
    return failure(TransportErrorCode::TlsIdentity,
                   "remote companion transport requires a TLS identity verifier");
  }
  const auto host = host_from_url(dependencies_.identity->value().gateway_url);
  if (host.empty() || !dependencies_.tls_verifier->verify(host, {})) {
    return failure(TransportErrorCode::TlsIdentity, "gateway TLS identity verification failed");
  }
  return TransportResult::success();
}

TransportResult Transport::configure(TransportSecurity security) {
  if (revoked_) {
    return failure(TransportErrorCode::Revoked, "companion device has been revoked");
  }
  if (dependencies_.identity == nullptr || !dependencies_.identity->has_value()) {
    return failure(TransportErrorCode::MissingIdentity,
                   "companion identity is required before transport configuration");
  }
  const auto route_result =
      build_companion_routes(dependencies_.identity->value().gateway_url, security);
  if (!route_result.ok()) return {route_result.error};
  security_ = security;
  routes_ = route_result.value;
  configured_ = true;
  status_ = TransportStatus::Unconfigured;
  network_loss_ = NetworkLoss::None;
  (void)credentials_.put_token(dependencies_.identity->value().scoped_token,
                               std::numeric_limits<std::uint64_t>::max());
  return TransportResult::success();
}

TransportResult Transport::start_enrollment() {
  if (!configured_ || dependencies_.websocket == nullptr) {
    return failure(TransportErrorCode::InvalidState,
                   "companion enrollment requires configured websocket transport");
  }
  if (status_ == TransportStatus::Enrolling) return TransportResult::success();
  std::string enroll_url = routes_.enroll;
  if (dependencies_.identity != nullptr && dependencies_.identity->has_value() &&
      !dependencies_.identity->value().device_id.empty()) {
    enroll_url += "?deviceId=";
    enroll_url += query_escape(dependencies_.identity->value().device_id);
  }
  if (!dependencies_.websocket->open(enroll_url, {})) {
    return failure(TransportErrorCode::Network, "companion enrollment socket could not open");
  }
  status_ = TransportStatus::Enrolling;
  return TransportResult::success();
}

TransportResult Transport::accept_enrollment_credential(std::string_view token,
                                                         std::uint64_t expires_at_ms) {
  if (!configured_ || dependencies_.identity == nullptr || !dependencies_.identity->has_value()) {
    return failure(TransportErrorCode::MissingIdentity,
                   "companion identity is required to accept an enrollment credential");
  }
  if (expires_at_ms <= now() || !credentials_.put_token(token, expires_at_ms)) {
    return failure(TransportErrorCode::EnrollmentExpired,
                   "companion enrollment credential is empty or expired");
  }
  auto identity = dependencies_.identity->value();
  identity.scoped_token.assign(token);
  if (!dependencies_.identity->save(identity)) {
    credentials_.revoke();
    return failure(TransportErrorCode::Storage, "companion credential could not be persisted");
  }
  credentials_.clear_pairing();
  revoked_ = false;
  status_ = TransportStatus::Unconfigured;
  return TransportResult::success();
}

TransportResult Transport::load_cache() {
  cache_loaded_ = false;
  cache_forced_stale_ = true;
  cache_last_seen_ms_ = 0;
  cached_prompts_display_only_ = true;
  if (dependencies_.snapshot_store == nullptr) return TransportResult::success();
  // A first boot has no journal yet.  Treat an absent/corrupt cache as an
  // empty display-only cache; a live handshake remains the authority.
  (void)dependencies_.snapshot_store->load();
  const auto cached = dependencies_.snapshot_store->last_valid();
  if (!cached.has_value()) return TransportResult::success();
  cache_loaded_ = true;
  if (cached->last_seen_at.has_value()) {
    cache_last_seen_ms_ = parse_datetime_ms(cached->last_seen_at->view());
  }
  return TransportResult::success();
}

TransportResult Transport::connect() {
  if (!configured_) {
    return failure(TransportErrorCode::InvalidState, "companion transport is not configured");
  }
  if (status_ == TransportStatus::Revoked || revoked_) {
    return failure(TransportErrorCode::Revoked, "companion device has been revoked");
  }
  if (dependencies_.identity == nullptr || !dependencies_.identity->has_value() ||
      dependencies_.identity->value().scoped_token.empty()) {
    return failure(TransportErrorCode::MissingIdentity, "companion token is unavailable");
  }
  if (!credentials_.has_valid_token(now())) {
    return failure(TransportErrorCode::Authentication, "companion token is expired");
  }
  const auto tls = verify_remote_identity();
  if (!tls.ok()) return tls;
  if (dependencies_.websocket == nullptr ||
      !dependencies_.websocket->open(routes_.ws, credentials_.token())) {
    status_ = TransportStatus::Reconnecting;
    return failure(TransportErrorCode::Network, "companion websocket could not open");
  }
  status_ = TransportStatus::Connecting;
  network_loss_ = NetworkLoss::None;
  hello_sent_ = false;
  hello_reconciled_ = false;
  live_attached_ = false;
  snapshot_required_ = false;
  gestures_enabled_ = false;
  cached_prompts_display_only_ = true;
  cache_forced_stale_ = true;
  return TransportResult::success();
}

TransportResult Transport::build_hello(Hello& hello) const {
  if (dependencies_.identity == nullptr || !dependencies_.identity->has_value()) {
    return failure(TransportErrorCode::MissingIdentity, "companion identity is unavailable");
  }
  hello = {};
  hello.protocol_version = kProtocolVersion;
  if (!hello.device_id.assign(dependencies_.identity->value().device_id)) {
    return failure(TransportErrorCode::Protocol, "companion device identity exceeds protocol bounds");
  }
  hello.supported_versions = {kProtocolVersion, kProtocolVersion};
  hello.last_accepted_adapter_sequence = dependencies_.identity->value().adapter_sequence;
  (void)hello.firmware.version.assign("t3-companion");
  hello.capabilities.max_message_bytes = kMaxJsonBytes;
  hello.capabilities.max_pending_commands = kMaxPendingCommands;
  hello.capabilities.max_pending_requests = kMaxPendingRequests;
  if (dependencies_.pending_commands != nullptr) {
    for (const auto& entry : dependencies_.pending_commands->entries()) {
      if (!hello.pending_command_ids.push(entry.command_id)) break;
    }
  } else if (dependencies_.command_outbox != nullptr) {
    for (const auto& entry : dependencies_.command_outbox->entries()) {
      if (!hello.pending_command_ids.push(entry.command.command_id)) break;
    }
  }
  hello.pending_command_count = hello.pending_command_ids.count;
  return TransportResult::success();
}

TransportResult Transport::on_socket_open() {
  if (status_ != TransportStatus::Connecting && status_ != TransportStatus::Reconnecting) {
    return failure(TransportErrorCode::HandshakeRequired,
                   "companion socket opened without a pending transport connect");
  }
  if (dependencies_.websocket == nullptr) {
    return failure(TransportErrorCode::Network, "companion websocket client is unavailable");
  }
  Hello hello;
  const auto built = build_hello(hello);
  if (!built.ok()) return built;
  const auto encoded = encode_hello(hello);
  if (!encoded.ok() || !dependencies_.websocket->send_text(encoded.value)) {
    status_ = TransportStatus::Reconnecting;
    return failure(TransportErrorCode::Network, "companion hello could not be sent");
  }
  hello_sent_ = true;
  return TransportResult::success();
}

TransportResult Transport::persist_status(const CommandStatusRecord& record) {
  if (dependencies_.pending_commands != nullptr &&
      !dependencies_.pending_commands->upsert(record.command_id, record.status)) {
    return failure(TransportErrorCode::Storage, "command status could not be persisted");
  }
  return TransportResult::success();
}

TransportResult Transport::on_hello_ack(const HelloAck& ack) {
  if (!hello_sent_) {
    return failure(TransportErrorCode::HandshakeRequired,
                   "hello_ack received before a companion hello");
  }
  if (ack.protocol_version != kProtocolVersion || ack.negotiated_version != kProtocolVersion ||
      ack.session_id.empty()) {
    status_ = TransportStatus::Incompatible;
    gestures_enabled_ = false;
    return failure(TransportErrorCode::Incompatible,
                   "companion gateway did not negotiate protocol version 1");
  }
  for (std::size_t index = 0; index < ack.pending_commands.records.count; ++index) {
    const auto& record = ack.pending_commands.records[index];
    const auto persisted = persist_status(record);
    if (!persisted.ok()) {
      gestures_enabled_ = false;
      return persisted;
    }
  }
  if (dependencies_.command_outbox != nullptr) {
    (void)dependencies_.command_outbox->reconcile(ack.pending_commands);
  }
  session_id_ = ack.session_id;
  status_ = TransportStatus::Live;
  network_loss_ = NetworkLoss::None;
  hello_reconciled_ = true;
  live_attached_ = true;
  snapshot_required_ = !ack.resume_replay;
  cache_forced_stale_ = snapshot_required_;
  gestures_enabled_ = true;
  cached_prompts_display_only_ = false;
  return TransportResult::success();
}

TransportResult Transport::request_snapshot() {
  if (!live_attached_ || !hello_reconciled_) {
    return failure(TransportErrorCode::SnapshotBeforeLive,
                   "snapshot must be requested after the authenticated live hello");
  }
  if (dependencies_.http == nullptr) {
    return failure(TransportErrorCode::Network, "companion HTTP client is unavailable");
  }
  const auto response = dependencies_.http->get(routes_.snapshot, credentials_.token());
  if (response.status != 200) {
    return failure(TransportErrorCode::Network, "companion snapshot request failed");
  }
  const auto decoded = decode_snapshot(response.body);
  if (!decoded.ok()) return failure(TransportErrorCode::Protocol, "companion snapshot is invalid");
  return on_snapshot(decoded.value);
}

TransportResult Transport::apply_snapshot_atomically(const Snapshot& snapshot) {
  // Durable writes happen before replacing the in-memory projection.  A
  // failed journal commit therefore leaves the previously rendered snapshot
  // and cursor untouched.
  if (dependencies_.snapshot_store != nullptr && !dependencies_.snapshot_store->save(snapshot)) {
    return failure(TransportErrorCode::Storage, "companion snapshot could not be cached");
  }
  if (dependencies_.identity != nullptr && dependencies_.identity->has_value() &&
      !dependencies_.identity->update_cursor(snapshot.adapter_sequence)) {
    return failure(TransportErrorCode::Storage, "companion replay cursor could not be persisted");
  }
  live_snapshot_ = snapshot;
  replay_guard_ = ReplayGuard{};
  (void)replay_guard_.accept(snapshot.adapter_sequence);
  snapshot_required_ = false;
  cache_loaded_ = true;
  cache_forced_stale_ = false;
  cache_last_seen_ms_ = parse_datetime_ms(snapshot.freshness.observed_at.view());
  cached_prompts_display_only_ = false;
  return TransportResult::success();
}

TransportResult Transport::on_snapshot(const Snapshot& snapshot) {
  if (!live_attached_ || !hello_reconciled_) {
    return failure(TransportErrorCode::SnapshotBeforeLive,
                   "companion snapshot cannot replace state before live attachment");
  }
  if (snapshot.protocol_version != kProtocolVersion) {
    status_ = TransportStatus::Incompatible;
    gestures_enabled_ = false;
    return failure(TransportErrorCode::Incompatible, "companion snapshot protocol is unsupported");
  }
  if (dependencies_.identity != nullptr && dependencies_.identity->has_value() &&
      snapshot.adapter_sequence < dependencies_.identity->value().adapter_sequence) {
    return failure(TransportErrorCode::RetentionMiss,
                   "companion snapshot would move the durable cursor backwards");
  }
  return apply_snapshot_atomically(snapshot);
}

TransportResult Transport::on_server_event(const ServerEvent& event) {
  if (!live_attached_ || !hello_reconciled_ || event.session_id != session_id_) {
    return failure(TransportErrorCode::HandshakeRequired,
                   "companion event is not attached to the active session");
  }
  if (event.protocol_version != kProtocolVersion) {
    return failure(TransportErrorCode::RetentionMiss,
                   "companion event cursor is duplicate, out of order, or incompatible");
  }
  const auto previous_sequence = replay_guard_.last_sequence();
  ReplayGuard candidate = replay_guard_;
  if (!candidate.accept(event.adapter_sequence) ||
      (previous_sequence != 0 && event.adapter_sequence > previous_sequence + 1U)) {
    return failure(TransportErrorCode::RetentionMiss,
                   "companion event cursor is duplicate, out of order, or incompatible");
  }
  if (event.type.view() == "snapshot") {
    if (!event.snapshot.has_value() ||
        event.snapshot->adapter_sequence != event.adapter_sequence) {
      return failure(TransportErrorCode::Protocol,
                     "companion snapshot event payload is invalid");
    }
    // apply_snapshot_atomically also advances the replay guard and durable
    // cursor, preserving the same atomic replacement semantics as the HTTP
    // snapshot path.
    return on_snapshot(*event.snapshot);
  }
  if (dependencies_.identity != nullptr && dependencies_.identity->has_value() &&
      !dependencies_.identity->update_cursor(event.adapter_sequence)) {
    return failure(TransportErrorCode::Storage, "companion event cursor could not be persisted");
  }
  replay_guard_ = candidate;
  return TransportResult::success();
}

TransportResult Transport::receive_text(std::string_view frame) {
  const auto decoded = decode_envelope(frame);
  if (!decoded.ok()) return failure(TransportErrorCode::Protocol, "companion frame is invalid");
  switch (decoded.value.type) {
    case Envelope::Type::HelloAck:
      return on_hello_ack(std::get<HelloAck>(decoded.value.payload));
    case Envelope::Type::Snapshot:
      return on_snapshot(std::get<Snapshot>(decoded.value.payload));
    case Envelope::Type::ServerEvent:
      return on_server_event(std::get<ServerEvent>(decoded.value.payload));
    default:
      return failure(TransportErrorCode::Protocol, "unexpected companion frame for device");
  }
}

TransportResult Transport::submit_command(const DeviceCommand& command) {
  if (status_ != TransportStatus::Live || !live_attached_ || !hello_reconciled_ ||
      !gestures_enabled_) {
    return failure(TransportErrorCode::Offline,
                   "mutating companion commands are disabled while the session is not live");
  }
  if (dependencies_.command_outbox == nullptr || dependencies_.websocket == nullptr) {
    return failure(TransportErrorCode::InvalidState, "companion command outbox is unavailable");
  }
  if (dependencies_.identity != nullptr && dependencies_.identity->has_value()) {
    Id enrolled_device;
    (void)enrolled_device.assign(dependencies_.identity->value().device_id);
    if (command.device_id != enrolled_device) {
      return failure(TransportErrorCode::Authentication,
                     "companion command device identity does not match the enrolled device");
    }
  }
  const auto transition = dependencies_.command_outbox->enqueue(command, true);
  if (!transition.accepted) {
    return failure(TransportErrorCode::Offline, "companion command was rejected offline");
  }
  bool send = false;
  for (const auto& effect : transition.effects) {
    if (effect.kind == EffectKind::SendCommand) send = true;
  }
  if (!send) return TransportResult::success();
  if (dependencies_.pending_commands != nullptr &&
      !dependencies_.pending_commands->upsert(command.command_id, CommandStatus::Received)) {
    return failure(TransportErrorCode::Storage, "companion command could not be persisted");
  }
  const auto encoded = encode_command_frame(command);
  if (!encoded.ok() || !dependencies_.websocket->send_text(encoded.value)) {
    status_ = TransportStatus::Reconnecting;
    gestures_enabled_ = false;
    return failure(TransportErrorCode::Network, "companion command could not be sent");
  }
  return TransportResult::success();
}

TransportResult Transport::on_socket_closed() {
  if (status_ == TransportStatus::Revoked || status_ == TransportStatus::Incompatible) {
    return TransportResult::success();
  }
  status_ = TransportStatus::Reconnecting;
  hello_sent_ = false;
  hello_reconciled_ = false;
  live_attached_ = false;
  gestures_enabled_ = false;
  cached_prompts_display_only_ = true;
  cache_forced_stale_ = true;
  return TransportResult::success();
}

TransportResult Transport::on_gateway_lost() {
  if (dependencies_.websocket != nullptr) dependencies_.websocket->close();
  network_loss_ = NetworkLoss::Gateway;
  status_ = TransportStatus::Reconnecting;
  hello_sent_ = false;
  hello_reconciled_ = false;
  live_attached_ = false;
  gestures_enabled_ = false;
  cached_prompts_display_only_ = true;
  cache_forced_stale_ = true;
  return TransportResult::success();
}

TransportResult Transport::on_access_point_lost() {
  if (dependencies_.websocket != nullptr) dependencies_.websocket->close();
  network_loss_ = NetworkLoss::AccessPoint;
  status_ = TransportStatus::Offline;
  hello_sent_ = false;
  hello_reconciled_ = false;
  live_attached_ = false;
  gestures_enabled_ = false;
  cached_prompts_display_only_ = true;
  cache_forced_stale_ = true;
  return TransportResult::success();
}

TransportResult Transport::on_revoked() {
  credentials_.revoke();
  revoked_ = true;
  if (dependencies_.websocket != nullptr) dependencies_.websocket->close();
  status_ = TransportStatus::Revoked;
  hello_sent_ = false;
  hello_reconciled_ = false;
  live_attached_ = false;
  gestures_enabled_ = false;
  cached_prompts_display_only_ = true;
  cache_forced_stale_ = true;
  return TransportResult::success();
}

TransportResult Transport::on_protocol_error(std::uint32_t server_min,
                                              std::uint32_t server_max) {
  if (server_min > kProtocolVersion || server_max < kProtocolVersion) {
    status_ = TransportStatus::Incompatible;
    gestures_enabled_ = false;
    if (dependencies_.websocket != nullptr) dependencies_.websocket->close();
    return failure(TransportErrorCode::Incompatible,
                   "companion gateway protocol range does not include version 1");
  }
  return failure(TransportErrorCode::Protocol, "companion protocol error");
}

bool Transport::cache_is_stale(std::uint64_t max_age_ms) const {
  if (!cache_loaded_ || cache_last_seen_ms_ == 0 || cache_forced_stale_ ||
      !live_attached_ || snapshot_required_) {
    return true;
  }
  const auto current = now();
  return current >= cache_last_seen_ms_ && current - cache_last_seen_ms_ > max_age_ms;
}

}  // namespace t3::companion
