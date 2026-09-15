#include "runtime.hpp"

#include "envelope.hpp"
#include "encoder.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>

namespace t3::companion::runtime {
namespace {

constexpr std::size_t kAudioSamplesPerRead = 512;
constexpr std::size_t kAudioQueueChunks = 8;
constexpr std::size_t kAudioQueueBytes = 16 * 1024;

bool has_target(const TargetContext& target) {
  return !target.environment_id.empty() && !target.project_id.empty() &&
         !target.thread_id.empty() && !target.expected_turn_id.empty();
}

TargetContext target_for_focus(const FocusQueueState& focus) {
  TargetContext target;
  const auto focused = focused_thread(focus);
  if (!focused.has_value()) return target;
  for (const auto& record : focus.model.work_items) {
    if (record.item.thread_id != *focused) continue;
    target.environment_id = record.item.environment_id;
    target.project_id = record.item.project_id;
    target.thread_id = record.item.thread_id;
    if (record.item.turn_id.has_value()) target.expected_turn_id = *record.item.turn_id;
    return target;
  }
  return target;
}

Id next_command_id(InteractionState& state) {
  Id id;
  const auto number = state.next_command_number++;
  (void)id.assign("companion-command-" + std::to_string(number));
  return id;
}

AudioStreamId stream_id_for(std::uint64_t now_ms, std::string_view device_id) {
  AudioStreamId stream;
  std::uint64_t state = now_ms ^ 0x9e3779b97f4a7c15ULL;
  for (const auto byte : device_id) {
    state ^= static_cast<std::uint8_t>(byte);
    state *= 0x100000001b3ULL;
  }
  for (auto& byte : stream.bytes) {
    state ^= state >> 12U;
    state ^= state << 25U;
    state ^= state >> 27U;
    byte = static_cast<std::uint8_t>((state * 0x2545F4914F6CDD1DULL) >> 56U);
  }
  return stream;
}

std::uint16_t provisioning_suffix(std::string_view device_id) {
  // IdfBspPlatform::device_id() is derived from the stable Wi-Fi MAC and ends
  // in four hexadecimal characters.  Keep the suffix deterministic without
  // exposing the full identifier in the setup UI (e.g. ...611c -> 611C).
  if (device_id.size() < 4U) return 0;
  const auto tail = device_id.substr(device_id.size() - 4U);
  std::uint16_t suffix = 0;
  for (std::size_t index = 0; index < tail.size(); ++index) {
    const char value = tail[index];
    std::uint8_t nibble = 0;
    if (value >= '0' && value <= '9') {
      nibble = static_cast<std::uint8_t>(value - '0');
    } else if (value >= 'a' && value <= 'f') {
      nibble = static_cast<std::uint8_t>(value - 'a' + 10);
    } else if (value >= 'A' && value <= 'F') {
      nibble = static_cast<std::uint8_t>(value - 'A' + 10);
    } else {
      return 0;
    }
    suffix = static_cast<std::uint16_t>((suffix << 4U) | nibble);
  }
  return suffix;
}

enum class EnrollmentFrameKind : std::uint8_t { Offer, Credential };

struct ParsedEnrollmentFrame {
  EnrollmentFrameKind kind = EnrollmentFrameKind::Offer;
  std::string enrollment_id;
  std::string device_id;
  std::string pairing_code;
  std::string expires_at;
  std::string credential;
};

bool json_string_field(std::string_view json, std::string_view key, std::size_t max_bytes,
                       std::string& value) {
  std::string needle = "\"";
  needle.append(key);
  needle.push_back('"');
  const auto found = json.find(needle);
  if (found == std::string_view::npos) return false;
  std::size_t cursor = found + needle.size();
  while (cursor < json.size() && (json[cursor] == ' ' || json[cursor] == '\t' ||
                                  json[cursor] == '\r' || json[cursor] == '\n')) {
    ++cursor;
  }
  if (cursor >= json.size() || json[cursor] != ':') return false;
  ++cursor;
  while (cursor < json.size() && (json[cursor] == ' ' || json[cursor] == '\t' ||
                                  json[cursor] == '\r' || json[cursor] == '\n')) {
    ++cursor;
  }
  if (cursor >= json.size() || json[cursor] != '"') return false;
  ++cursor;
  value.clear();
  value.reserve(std::min(max_bytes, json.size() - cursor));
  while (cursor < json.size()) {
    const char character = json[cursor++];
    if (character == '"') return true;
    if (character == '\\') {
      if (cursor >= json.size()) return false;
      const char escaped = json[cursor++];
      switch (escaped) {
        case '"':
        case '\\':
        case '/': value.push_back(escaped); break;
        case 'b': value.push_back('\b'); break;
        case 'f': value.push_back('\f'); break;
        case 'n': value.push_back('\n'); break;
        case 'r': value.push_back('\r'); break;
        case 't': value.push_back('\t'); break;
        default: return false;
      }
    } else {
      if (static_cast<unsigned char>(character) < 0x20U) return false;
      value.push_back(character);
    }
    if (value.size() > max_bytes) return false;
  }
  return false;
}

bool json_number_field(std::string_view json, std::string_view key, std::uint64_t& value) {
  std::string needle = "\"";
  needle.append(key);
  needle.push_back('"');
  const auto found = json.find(needle);
  if (found == std::string_view::npos) return false;
  std::size_t cursor = found + needle.size();
  while (cursor < json.size() && (json[cursor] == ' ' || json[cursor] == '\t' ||
                                  json[cursor] == '\r' || json[cursor] == '\n')) {
    ++cursor;
  }
  if (cursor >= json.size() || json[cursor] != ':') return false;
  ++cursor;
  while (cursor < json.size() && (json[cursor] == ' ' || json[cursor] == '\t' ||
                                  json[cursor] == '\r' || json[cursor] == '\n')) {
    ++cursor;
  }
  const auto begin = json.data() + cursor;
  const auto end = json.data() + json.size();
  const auto parsed = std::from_chars(begin, end, value);
  return parsed.ec == std::errc{} && parsed.ptr != begin;
}

bool parse_enrollment_frame(std::string_view frame, ParsedEnrollmentFrame& parsed) {
  constexpr std::size_t kMaxEnrollmentFrameBytes = 4'096;
  if (frame.empty() || frame.size() > kMaxEnrollmentFrameBytes) return false;
  std::string type;
  if (!json_string_field(frame, "type", 32U, type)) return false;
  if (type == "enrollment") {
    std::uint64_t protocol_version = 0;
    if (!json_number_field(frame, "protocolVersion", protocol_version) ||
        protocol_version != kProtocolVersion ||
        !json_string_field(frame, "enrollmentId", kMaxIdBytes, parsed.enrollment_id) ||
        !json_string_field(frame, "deviceId", kMaxDeviceIdBytes, parsed.device_id) ||
        !json_string_field(frame, "pairingCode", 32U, parsed.pairing_code) ||
        !json_string_field(frame, "expiresAt", kMaxDateTimeBytes, parsed.expires_at) ||
        parsed.enrollment_id.empty() || parsed.device_id.empty() || parsed.pairing_code.empty() ||
        parsed.expires_at.empty()) {
      return false;
    }
    parsed.kind = EnrollmentFrameKind::Offer;
    return true;
  }
  if (type == "enrollment_credential") {
    if (!json_string_field(frame, "credential", kMaxScopedTokenBytes, parsed.credential) ||
        parsed.credential.empty()) {
      return false;
    }
    parsed.kind = EnrollmentFrameKind::Credential;
    return true;
  }
  return false;
}

Snapshot snapshot_from_compact(const CompactSnapshot& compact) {
  Snapshot snapshot;
  snapshot.protocol_version = compact.protocol_version;
  snapshot.adapter_sequence = compact.adapter_sequence;
  snapshot.connectivity.status = compact.connectivity;
  snapshot.connectivity.last_seen_at = compact.last_seen_at;
  snapshot.freshness.observed_at = compact.last_seen_at.value_or(DateTime{});
  if (compact.automatic_focus_thread_id.has_value()) {
    snapshot.automatic_focus_thread_id = compact.automatic_focus_thread_id;
  }
  for (const auto& source : compact.work_items) {
    WorkItem item;
    item.environment_id = source.environment_id;
    item.project_id = source.project_id;
    item.thread_id = source.thread_id;
    item.turn_id = source.turn_id;
    item.project_name = source.project_name;
    item.thread_name = source.thread_name;
    item.provider = source.provider;
    item.state = source.state;
    item.activity = source.activity;
    item.updated_at = source.updated_at;
    (void)snapshot.work_items.push(item);
  }
  for (const auto& source : compact.usage.providers) {
    UsageProvider provider;
    provider.provider = source.provider;
    provider.status = source.status;
    provider.observed_at = source.observed_at;
    for (const auto& window : source.windows) (void)provider.windows.push(window);
    (void)snapshot.usage.providers.push(provider);
  }
  snapshot.usage.provider_count = snapshot.usage.providers.count;
  snapshot.work_item_count = snapshot.work_items.count;
  snapshot.pending_request_count = 0;
  return snapshot;
}

}  // namespace

bool FixedRuntimeInputQueue::push(const RuntimeInputEvent& event) {
  if (count_ == kCapacity) return false;
  entries_[tail_] = event;
  tail_ = (tail_ + 1U) % kCapacity;
  ++count_;
  return true;
}

bool FixedRuntimeInputQueue::pop(RuntimeInputEvent& event) {
  if (count_ == 0) return false;
  event = entries_[head_];
  head_ = (head_ + 1U) % kCapacity;
  --count_;
  return true;
}

RuntimeCoordinator::RuntimeCoordinator(RuntimeDependencies dependencies,
                                       RuntimeConfig config)
    : dependencies_(dependencies),
      config_(config),
      identity_(dependencies.identity),
      networks_(dependencies.networks),
      pending_commands_(dependencies.pending_commands),
      snapshots_(dependencies.snapshots),
      platform_(dependencies.platform),
      input_queue_(dependencies.input_queue),
      transport_outbox_(),
      transport_({&identity_, &pending_commands_, &snapshots_, &transport_outbox_,
                  &dependencies.websocket, &dependencies.http, &dependencies.tls_verifier,
                  [clock = &dependencies.clock]() { return clock->now_ms(); }}),
      portal_(networks_, provisioning_suffix(dependencies.platform.device_id())),
      roaming_(networks_),
      buttons_(),
      lock_(),
      ptt_(),
      gestures_(),
      capture_(t3::board::verified_manifest().audio),
      reconnect_backoff_({config.reconnect_initial_ms, config.reconnect_max_ms, 2.0,
                          config.reconnect_jitter_percent}) {
  interaction_ = make_interaction_state(false);
  model_.now_ms = dependencies.clock.now_ms();
  focus_ = make_focus_queue(model_, model_.now_ms);
  usage_ = {};
  update_gesture_context();
}

RuntimeResult RuntimeCoordinator::fail(RuntimeErrorCode code, std::string message) {
  last_error_ = {code, std::move(message)};
  phase_ = RuntimePhase::Fault;
  return {last_error_};
}

bool RuntimeCoordinator::paired_identity() const {
  return identity_.has_value() && !identity_.value().device_id.empty() &&
         !identity_.value().scoped_token.empty() && !identity_.value().gateway_url.empty();
}

bool RuntimeCoordinator::transport_policy_allows() const {
  if (!enrollment_policy_allows() || !paired_identity()) return false;
  return true;
}

bool RuntimeCoordinator::enrollment_policy_allows() const {
  if (!config_.transport_enabled || !identity_.has_value() ||
      identity_.value().gateway_url.empty()) {
    return false;
  }
  return config_.security == TransportSecurity::TrustedLan ? config_.allow_trusted_lan
                                                            : config_.allow_remote;
}

RuntimeResult RuntimeCoordinator::ensure_identity() {
  if (identity_.has_value() && !identity_.value().device_id.empty()) return RuntimeResult::success();
  std::string device_id = platform_.device_id();
  if (device_id.empty()) device_id = "t3-companion-unpaired";
  if (device_id.size() > kMaxDeviceIdBytes) device_id.resize(kMaxDeviceIdBytes);
  DeviceIdentity value;
  value.device_id = std::move(device_id);
  if (!identity_.save(value)) {
    return fail(RuntimeErrorCode::Persistence, "local companion identity could not be stored");
  }
  return RuntimeResult::success();
}

Snapshot RuntimeCoordinator::cached_snapshot() const {
  const auto cached = snapshots_.last_valid();
  if (!cached.has_value()) {
    Snapshot snapshot;
    snapshot.connectivity.status = ConnectivityStatus::Offline;
    (void)snapshot.freshness.observed_at.assign("0", false);
    return snapshot;
  }
  return snapshot_from_compact(*cached);
}

RuntimeResult RuntimeCoordinator::render() {
  t3::ui::UiOptions options;
  options.locked = lock_.locked();
  options.protocol_incompatible = phase_ == RuntimePhase::Incompatible;
  options.provisioning = phase_ == RuntimePhase::Provisioning;
  options.pixel_shift_enabled = true;
  options.pixel_shift_seed = static_cast<std::uint32_t>(dependencies_.clock.now_ms());
  ui_model_ = compose_ui_model(model_, focus_, interaction_, usage_, options);
  if (!platform_.render(ui_model_)) {
    return fail(RuntimeErrorCode::Display, "companion display render failed");
  }
  if (enrollment_socket_open_ && !enrollment_id_.empty() && !pairing_code_.empty() &&
      !platform_.render_enrollment_pairing(enrollment_id_, pairing_code_,
                                           enrollment_expires_at_)) {
    return fail(RuntimeErrorCode::Display, "companion enrollment pairing render failed");
  }
  return RuntimeResult::success();
}

void RuntimeCoordinator::set_connectivity(bool live) {
  model_.live_handshake = live;
  if (live) {
    model_.connectivity.status = ConnectivityStatus::Connected;
  } else if (phase_ == RuntimePhase::Reconnecting) {
    model_.connectivity.status = ConnectivityStatus::Reconnecting;
  } else {
    model_.connectivity.status = ConnectivityStatus::Offline;
  }
  interaction_ = reduce_interaction(
                     interaction_,
                     InteractionEvent::connectivity(live, dependencies_.clock.now_ms()))
                   .state;
  update_gesture_context();
}

void RuntimeCoordinator::update_gesture_context() {
  GestureContext context;
  context.mode = interaction_.mode;
  context.locked = lock_.locked() || !transport_.gestures_enabled();
  if (interaction_.request.has_value() &&
      interaction_.question_index < interaction_.request->questions.count) {
    const auto& question = interaction_.request->questions[interaction_.question_index];
    context.multi_select = question.multi_select;
    context.submit_card = question.multi_select &&
                          interaction_.card_index >= question.options.count;
  }
  gestures_.set_context(context);
  ptt_.set_context({lock_.locked(), transport_.gestures_enabled(),
                    lock_.accepts_voice(), false});
}

RuntimeResult RuntimeCoordinator::open_provisioning_ap() {
  if (provisioning_ap_started_) return RuntimeResult::success();
  if (!portal_.begin(platform_.wifi_connected())) {
    return fail(RuntimeErrorCode::Provisioning, "companion provisioning portal could not start");
  }
  if (!platform_.start_provisioning_ap(portal_.ap_ssid())) {
    return fail(RuntimeErrorCode::Provisioning, "companion provisioning AP could not start");
  }
  provisioning_ap_started_ = true;
  return RuntimeResult::success();
}

RuntimeResult RuntimeCoordinator::boot() {
  if (booted_) return RuntimeResult::failure(RuntimeErrorCode::AlreadyBooted,
                                             "companion runtime is already booted");
  phase_ = RuntimePhase::Booting;
  platform_.attach_input_queue(input_queue_);
  platform_.attach_provisioning_handler(*this);
  if (!platform_.initialize()) {
    return fail(RuntimeErrorCode::BoardInitialization, "verified companion board initialization failed");
  }
  if (dependencies_.health != nullptr) {
    dependencies_.health->mark_board_initialized();
    // The BSP initializes the display driver and touch input together.  Keep
    // both gates explicit so OTA rollback cannot be bypassed by a display-only
    // boot that never enabled touch input.
    dependencies_.health->mark_display_initialized();
    dependencies_.health->mark_touch_initialized();
  }
  (void)networks_.load();
  (void)pending_commands_.load();
  (void)snapshots_.load();
  (void)identity_.load();
  const auto identity_result = ensure_identity();
  if (!identity_result.ok()) return identity_result;
  if (dependencies_.health != nullptr) {
    dependencies_.health->mark_persistence_mounted();
  }

  (void)transport_.load_cache();
  const auto cached = cached_snapshot();
  model_ = project_snapshot(cached, dependencies_.clock.now_ms());
  model_.live_handshake = false;
  model_.connectivity.status = ConnectivityStatus::Offline;
  usage_ = cached.usage;
  focus_ = make_focus_queue(model_, model_.now_ms);
  interaction_ = make_interaction_state(false, identity_.value().device_id);
  boot_started_ms_ = dependencies_.clock.now_ms();
  offline_health_reported_ = false;
  reconnect_backoff_.reset();
  booted_ = true;

  const bool needs_setup = identity_.value().scoped_token.empty() ||
                           identity_.value().gateway_url.empty();
  phase_ = needs_setup ? RuntimePhase::Provisioning : RuntimePhase::Offline;
  update_gesture_context();
  // This render is intentionally before Wi-Fi or WebSocket setup so stale
  // cached state is useful even when the gateway is unavailable.
  const auto rendered = render();
  if (!rendered.ok()) return rendered;

  if (needs_setup) {
    const auto ap = open_provisioning_ap();
    if (!ap.ok()) return ap;
    // A reboot during the T3 Code pairing window must resume the normal
    // enrollment attempt when a gateway and remembered Wi-Fi already exist;
    // otherwise remain safely local and wait for the setup form.
    if (identity_.value().scoped_token.empty() &&
        !identity_.value().gateway_url.empty() && !networks_.empty() &&
        enrollment_policy_allows()) {
      enrollment_requested_ = true;
      return start_enrollment_transport();
    }
    return RuntimeResult::success();
  }
  if (!transport_policy_allows()) return RuntimeResult::success();
  return start_configured_transport();
}

RuntimeResult RuntimeCoordinator::start_configured_transport() {
  if (!transport_policy_allows()) return RuntimeResult::success();
  if (!transport_configured_) {
    const auto configured = transport_.configure(config_.security);
    if (!configured.ok()) {
      return fail(RuntimeErrorCode::Configuration, configured.error.message);
    }
    transport_configured_ = true;
  }
  if (!wifi_started_) {
    if (!platform_.wifi_start()) {
      mark_offline(RuntimePhase::Offline);
      return RuntimeResult::success();
    }
    wifi_started_ = true;
  }
  return connect_if_ready();
}

RuntimeResult RuntimeCoordinator::start_enrollment_transport() {
  if (!enrollment_policy_allows() || paired_identity()) return RuntimeResult::success();
  if (!transport_configured_) {
    const auto configured = transport_.configure(config_.security);
    if (!configured.ok()) {
      return fail(RuntimeErrorCode::Configuration, configured.error.message);
    }
    transport_configured_ = true;
  }
  if (!wifi_started_) {
    if (!platform_.wifi_start()) {
      mark_offline(RuntimePhase::Provisioning);
      return RuntimeResult::success();
    }
    wifi_started_ = true;
  }
  return connect_if_ready();
}

RuntimeResult RuntimeCoordinator::connect_if_ready() {
  if (!wifi_started_) return RuntimeResult::success();
  const bool enrolling = enrollment_requested_ && enrollment_policy_allows() && !paired_identity();
  if (!transport_policy_allows() && !enrolling) return RuntimeResult::success();
  const auto now_ms = dependencies_.clock.now_ms();
  if (now_ms < next_connect_ms_) return RuntimeResult::success();
  if (!platform_.wifi_connected()) {
    const auto credential = roaming_.next_attempt();
    if (!credential.has_value()) {
      phase_ = RuntimePhase::Provisioning;
      return open_provisioning_ap();
    }
    if (!platform_.wifi_connect(credential->ssid, credential->password)) {
      phase_ = RuntimePhase::Reconnecting;
      next_connect_ms_ = now_ms + reconnect_backoff_.next(50).delay_ms;
      // Advance to the next remembered network only when the station driver
      // rejects the join request.  A successful asynchronous join request may
      // still emit a transient disconnect while scanning/associating; marking
      // it as a roaming failure here exhausts a one-network journal before the
      // GOT_IP event and strands the paired runtime in local provisioning.
      (void)roaming_.report_failure();
    } else {
      phase_ = RuntimePhase::Connecting;
      // The station driver reports readiness asynchronously. Do not add a
      // second backoff delay after a successful join request; the next tick
      // may immediately observe the connected event and send hello.
      next_connect_ms_ = now_ms;
    }
    return RuntimeResult::success();
  }

  if (transport_.status() == TransportStatus::Live ||
      transport_.status() == TransportStatus::Connecting ||
      transport_.status() == TransportStatus::Enrolling) {
    return RuntimeResult::success();
  }
  if (enrolling) {
    const auto started = transport_.start_enrollment();
    if (!started.ok()) {
      phase_ = RuntimePhase::Reconnecting;
      next_connect_ms_ = now_ms + reconnect_backoff_.next(50).delay_ms;
      set_connectivity(false);
      return render();
    }
    enrollment_socket_open_ = true;
    phase_ = RuntimePhase::Connecting;
    return render();
  }
  const auto connected = transport_.connect();
  if (!connected.ok()) {
    phase_ = RuntimePhase::Reconnecting;
    next_connect_ms_ = now_ms + reconnect_backoff_.next(50).delay_ms;
    set_connectivity(false);
    return render();
  }
  phase_ = RuntimePhase::Connecting;
  const auto hello = transport_.on_socket_open();
  if (!hello.ok()) {
    phase_ = RuntimePhase::Reconnecting;
    next_connect_ms_ = now_ms + reconnect_backoff_.next(50).delay_ms;
    set_connectivity(false);
    return render();
  }
  return render();
}

RuntimeResult RuntimeCoordinator::tick() {
  if (!booted_) return RuntimeResult::failure(RuntimeErrorCode::BoardInitialization,
                                              "companion runtime has not booted");
  RuntimeInputEvent event;
  // Drain a bounded number per tick; an ISR storm cannot starve transport or
  // display work indefinitely.
  for (std::size_t count = 0; count < input_queue_.capacity() && input_queue_.pop(event);
       ++count) {
    const auto result = process_input(event);
    if (!result.ok()) return result;
  }
  const auto timed_button_events = buttons_.tick(dependencies_.clock.now_ms());
  if (!timed_button_events.empty()) {
    const auto buttons = process_button_events(timed_button_events);
    if (!buttons.ok()) return buttons;
    const auto rendered = render();
    if (!rendered.ok()) return rendered;
  }
  // A paired device may briefly enter the provisioning phase while roaming
  // retries exhaust (the local setup AP remains available in that interval).
  // Once station connectivity returns, do not let that display phase suppress
  // the authenticated transport reconnect.
  if (transport_policy_allows() || enrollment_requested_) {
    const auto configured = enrollment_requested_ && !paired_identity()
                                ? start_enrollment_transport()
                                : start_configured_transport();
    if (!configured.ok()) return configured;
  }
  maybe_mark_offline_health();
  InteractionEvent tick_event;
  tick_event.kind = InteractionEventKind::Tick;
  tick_event.now_ms = dependencies_.clock.now_ms();
  const auto tick_transition = reduce_interaction(interaction_, tick_event);
  interaction_ = tick_transition.state;
  if (!tick_transition.effects.empty()) {
    const auto effects = process_effects(tick_transition.effects);
    if (!effects.ok()) return effects;
    const auto rendered = render();
    if (!rendered.ok()) return rendered;
  }
  const auto audio = pump_audio();
  if (!audio.ok()) return audio;
  return RuntimeResult::success();
}

bool RuntimeCoordinator::enqueue(const RuntimeInputEvent& event) {
  return input_queue_.push(event);
}

bool RuntimeCoordinator::enqueue_touch(const RawTouchSample& sample) {
  RuntimeInputEvent event;
  event.kind = RuntimeInputKind::Touch;
  event.touch = sample;
  return enqueue(event);
}

bool RuntimeCoordinator::enqueue_button(const RawButtonSample& sample) {
  RuntimeInputEvent event;
  event.kind = RuntimeInputKind::Button;
  event.button = sample;
  return enqueue(event);
}

RuntimeResult RuntimeCoordinator::process_input(const RuntimeInputEvent& event) {
  return event.kind == RuntimeInputKind::Touch ? process_touch(event.touch)
                                               : process_button(event.button);
}

RuntimeResult RuntimeCoordinator::process_touch(const RawTouchSample& sample) {
  if (lock_.locked()) {
    (void)lock_.on_touch(sample.timestamp_ms);
    return RuntimeResult::success();
  }
  update_gesture_context();
  const auto action = gestures_.feed(sample);
  if (!action.has_value()) return RuntimeResult::success();
  if (*action == SemanticAction::BrowseNext || *action == SemanticAction::BrowsePrevious) {
    const auto event_kind = *action == SemanticAction::BrowseNext ? FocusEventKind::BrowseNext
                                                                    : FocusEventKind::BrowsePrevious;
    focus_ = reduce_focus(focus_, FocusEvent{event_kind, sample.timestamp_ms, std::nullopt}).state;
    return render();
  }
  const auto interaction_event =
      to_interaction_event(*action, interaction_.mode, sample.timestamp_ms);
  if (!interaction_event.has_value()) return RuntimeResult::success();
  return process_interaction(*interaction_event);
}

RuntimeResult RuntimeCoordinator::process_button(const RawButtonSample& sample) {
  const auto events = buttons_.feed(sample);
  const auto processed = process_button_events(events);
  if (!processed.ok()) return processed;
  return render();
}

RuntimeResult RuntimeCoordinator::process_button_events(const std::vector<ButtonEvent>& events) {
  for (const auto& event : events) {
    const auto lock_transition = lock_.on_button(event);
    for (const auto& effect : lock_transition.effects) {
      if (effect.kind == LockEffectKind::DisplayOff || effect.kind == LockEffectKind::DisplayOn) {
        if (!platform_.set_display_power(effect.kind == LockEffectKind::DisplayOn)) {
          return fail(RuntimeErrorCode::Display, "companion display power transition failed");
        }
      }
    }
    if (!lock_transition.effects.empty()) {
      const bool locked = lock_.locked();
      (void)ptt_.on_lock_changed(locked, event.timestamp_ms);
      // A lock transition cancels an active PTT session.  The local capture
      // must stop at the same edge as the UI lock; the bounded STT stop is
      // sent only while the transport is still live.
      if (locked && audio_stream_.has_value()) {
        const auto stopped = stop_audio_stream(event.timestamp_ms);
        if (!stopped.ok() && stopped.error.code != RuntimeErrorCode::Network) {
          return stopped;
        }
      }
    }
    update_gesture_context();
    const auto ptt_effects = ptt_.on_button(event);
    const auto processed = process_ptt_effects(ptt_effects);
    if (!processed.ok()) return processed;
  }
  return RuntimeResult::success();
}

RuntimeResult RuntimeCoordinator::process_ptt_effects(const std::vector<PttEffect>& effects) {
  for (const auto& effect : effects) {
    if (effect.kind == PttEffectKind::CaptureStarted) {
      const auto result = start_audio_stream(effect.timestamp_ms);
      if (!result.ok()) return result;
    } else if (effect.kind == PttEffectKind::RequestTranscription) {
      const auto result = stop_audio_stream(effect.timestamp_ms);
      if (!result.ok()) return result;
    } else if (effect.kind == PttEffectKind::CaptureCancelled && audio_stream_.has_value()) {
      // There is no transcript to commit for a cancelled hold.  Stopping the
      // bounded stream still lets the gateway release its server-side lease.
      const auto result = stop_audio_stream(effect.timestamp_ms);
      if (!result.ok() && result.error.code != RuntimeErrorCode::Network) return result;
    }
  }
  return RuntimeResult::success();
}

RuntimeResult RuntimeCoordinator::process_interaction(const InteractionEvent& event) {
  const auto transition = reduce_interaction(interaction_, event);
  interaction_ = transition.state;
  update_gesture_context();
  const auto effects = process_effects(transition.effects);
  if (!effects.ok()) return effects;
  return render();
}

RuntimeResult RuntimeCoordinator::process_effects(const std::vector<Effect>& effects) {
  for (const auto& effect : effects) {
    if (effect.kind == EffectKind::SendCommand && effect.command.has_value()) {
      const auto sent = transport_.submit_command(*effect.command);
      if (!sent.ok()) {
        (void)process_interaction(InteractionEvent::command_status(
            effect.command->command_id, CommandStatus::Rejected, dependencies_.clock.now_ms()));
        if (sent.error.code == TransportErrorCode::Offline) phase_ = RuntimePhase::Offline;
      }
    } else if (effect.kind == EffectKind::RequestVoiceInput) {
      const auto started = start_audio_stream(dependencies_.clock.now_ms());
      if (!started.ok()) return started;
    } else if (effect.kind == EffectKind::OfflineRejected) {
      phase_ = RuntimePhase::Offline;
    }
  }
  return RuntimeResult::success();
}

RuntimeResult RuntimeCoordinator::send_aux_command(DeviceCommand command) {
  if (!transport_.gestures_enabled()) {
    return fail(RuntimeErrorCode::Network, "voice command is unavailable while the gateway is offline");
  }
  const auto queued = interaction_.outbox.enqueue(command, true);
  if (!queued.accepted) return RuntimeResult::failure(RuntimeErrorCode::Network,
                                                       "voice command queue is full");
  const auto sent = transport_.submit_command(command);
  if (!sent.ok()) {
    interaction_ = reduce_interaction(
                       interaction_, InteractionEvent::command_status(
                                         command.command_id, CommandStatus::Rejected,
                                         dependencies_.clock.now_ms()))
                   .state;
    return sent.error.code == TransportErrorCode::Offline
               ? RuntimeResult::failure(RuntimeErrorCode::Network, "voice command is offline")
               : RuntimeResult::failure(RuntimeErrorCode::Protocol, sent.error.message);
  }
  return RuntimeResult::success();
}

RuntimeResult RuntimeCoordinator::start_audio_stream(std::uint64_t now_ms) {
  if (!transport_.gestures_enabled() || lock_.locked()) {
    return RuntimeResult::failure(RuntimeErrorCode::Network,
                                  "audio capture is unavailable while the gateway is offline");
  }
  TargetContext target = interaction_.request.has_value() ? interaction_.target
                                                           : target_for_focus(focus_);
  if (!has_target(target)) {
    return RuntimeResult::failure(RuntimeErrorCode::Protocol,
                                  "audio capture has no frozen companion target");
  }
  if (capture_.start() != CaptureResult::Started ||
      !platform_.audio_start(config_.audio_sample_rate_hz)) {
    return RuntimeResult::failure(RuntimeErrorCode::Network, "verified microphone capture failed to start");
  }
  const auto stream_id = stream_id_for(now_ms, identity_.value().device_id);
  audio_stream_.emplace(AudioStreamLimits{stream_id, kAudioQueueChunks, kAudioQueueBytes});
  (void)audio_stream_->start();
  Id stream_label;
  (void)stream_label.assign("stream");
  interaction_ = reduce_interaction(
                     interaction_, InteractionEvent::stt_start(stream_label, target, now_ms))
                   .state;

  DeviceCommand command;
  command.type = DeviceCommandType::SttStart;
  command.command_id = next_command_id(interaction_);
  (void)command.device_id.assign(identity_.value().device_id);
  command.context = target;
  (void)command.stt_stream_id.assign("stream");
  // The binary stream identifier is carried in the audio frames.  The
  // protocol's textual sttStreamId remains a bounded correlation label.
  return send_aux_command(std::move(command));
}

RuntimeResult RuntimeCoordinator::stop_audio_stream(std::uint64_t now_ms) {
  (void)now_ms;
  if (!audio_stream_.has_value()) return RuntimeResult::success();
  const auto stream_id = audio_stream_->stream_id();
  (void)audio_stream_->stop();
  (void)capture_.stop();
  platform_.audio_stop();
  audio_stream_.reset();

  DeviceCommand command;
  command.type = DeviceCommandType::SttStop;
  command.command_id = next_command_id(interaction_);
  (void)command.device_id.assign(identity_.value().device_id);
  command.context = interaction_.transcript_target_frozen ? interaction_.transcript_target
                                                           : target_for_focus(focus_);
  (void)command.stt_stream_id.assign("stream");
  (void)stream_id;
  return send_aux_command(std::move(command));
}

RuntimeResult RuntimeCoordinator::pump_audio() {
  if (!audio_stream_.has_value() || !audio_stream_->active()) return RuntimeResult::success();
  if (!transport_.gestures_enabled()) {
    (void)audio_stream_->on_disconnect();
    (void)capture_.disconnect();
    platform_.audio_stop();
    audio_stream_.reset();
    return RuntimeResult::success();
  }

  std::array<std::int16_t, kAudioSamplesPerRead> samples{};
  const int read = platform_.audio_read(samples);
  if (read <= 0) return RuntimeResult::success();
  const auto count = std::min<std::size_t>(static_cast<std::size_t>(read), samples.size());
  const auto accepted = audio_stream_->push_pcm(
      audio_stream_->stream_id(), std::span<const std::int16_t>(samples.data(), count),
      capture_.config().downmix);
  if (accepted == StreamResult::Backpressure) return RuntimeResult::success();
  if (accepted != StreamResult::Accepted) {
    return fail(RuntimeErrorCode::Network, "bounded audio stream rejected a capture frame");
  }
  while (const auto frame = audio_stream_->pop_frame()) {
    if (!send_companion_audio_frame(dependencies_.websocket, *frame)) {
      return fail(RuntimeErrorCode::Network, "companion audio frame could not be sent");
    }
  }
  return RuntimeResult::success();
}

RuntimeResult RuntimeCoordinator::apply_snapshot(const Snapshot& snapshot) {
  model_ = project_snapshot(snapshot, dependencies_.clock.now_ms());
  model_.live_handshake = transport_.gestures_enabled();
  usage_ = snapshot.usage;
  focus_ = make_focus_queue(model_, model_.now_ms);
  if (transport_.gestures_enabled() && !lock_.locked() &&
      interaction_.mode == InteractionMode::Ambient && !model_.pending_requests.empty()) {
    const auto request = active_request(focus_);
    if (request.has_value()) interaction_ = reduce_interaction(
        interaction_, InteractionEvent::open(*request, dependencies_.clock.now_ms())).state;
  }
  update_gesture_context();
  return render();
}

RuntimeResult RuntimeCoordinator::apply_hello_ack(const HelloAck& ack) {
  const auto result = transport_.on_hello_ack(ack);
  if (!result.ok()) {
    phase_ = result.error.code == TransportErrorCode::Incompatible ? RuntimePhase::Incompatible
                                                                    : RuntimePhase::Reconnecting;
    set_connectivity(false);
    return render();
  }
  phase_ = RuntimePhase::Live;
  if (dependencies_.health != nullptr) dependencies_.health->mark_handshake_succeeded();
  InteractionEvent statuses_event;
  statuses_event.kind = InteractionEventKind::CommandStatuses;
  statuses_event.now_ms = dependencies_.clock.now_ms();
  statuses_event.live_handshake = true;
  statuses_event.statuses = ack.pending_commands;
  interaction_ = reduce_interaction(interaction_, statuses_event).state;
  set_connectivity(true);
  reconnect_backoff_.reset();
  update_gesture_context();
  return render();
}

RuntimeResult RuntimeCoordinator::on_hello_ack(const HelloAck& ack) {
  if (!booted_) return RuntimeResult::failure(RuntimeErrorCode::BoardInitialization,
                                              "companion runtime has not booted");
  return apply_hello_ack(ack);
}

RuntimeResult RuntimeCoordinator::handle_enrollment_text(std::string_view frame) {
  ParsedEnrollmentFrame parsed;
  if (!parse_enrollment_frame(frame, parsed)) {
    phase_ = RuntimePhase::Incompatible;
    set_connectivity(false);
    (void)render();
    return fail(RuntimeErrorCode::Protocol, "companion enrollment frame was invalid");
  }
  if (parsed.kind == EnrollmentFrameKind::Offer) {
    if (!identity_.has_value() || parsed.device_id != identity_.value().device_id) {
      phase_ = RuntimePhase::Incompatible;
      set_connectivity(false);
      (void)render();
      return fail(RuntimeErrorCode::Protocol, "companion enrollment identity did not match");
    }
    enrollment_id_ = std::move(parsed.enrollment_id);
    pairing_code_ = std::move(parsed.pairing_code);
    enrollment_expires_at_ = std::move(parsed.expires_at);
    enrollment_socket_open_ = true;
    phase_ = RuntimePhase::Connecting;
    return render();
  }
  if (!enrollment_socket_open_ || !enrollment_requested_) {
    return fail(RuntimeErrorCode::Protocol, "companion enrollment credential arrived unexpectedly");
  }
  // Gateway-issued credentials are revocable server-side and have no separate
  // device expiry field.  Keep them valid until revocation rather than asking
  // the user to handle opaque bearer material in the local portal.
  const auto accepted = transport_.accept_enrollment_credential(
      parsed.credential, std::numeric_limits<std::uint64_t>::max());
  if (!accepted.ok()) {
    phase_ = RuntimePhase::Reconnecting;
    set_connectivity(false);
    (void)render();
    return fail(RuntimeErrorCode::Persistence, accepted.error.message);
  }
  enrollment_requested_ = false;
  enrollment_socket_open_ = false;
  enrollment_id_.clear();
  pairing_code_.clear();
  enrollment_expires_at_.clear();
  transport_configured_ = false;
  phase_ = RuntimePhase::Offline;
  update_gesture_context();
  return render();
}

RuntimeResult RuntimeCoordinator::on_socket_text(std::string_view frame) {
  if (enrollment_socket_open_ || transport_.status() == TransportStatus::Enrolling) {
    return handle_enrollment_text(frame);
  }
  const auto decoded = decode_envelope(frame);
  if (!decoded.ok()) {
    phase_ = RuntimePhase::Incompatible;
    set_connectivity(false);
    (void)render();
    return fail(RuntimeErrorCode::Protocol, "companion gateway frame was invalid");
  }
  const auto result = transport_.receive_text(frame);
  if (!result.ok()) {
    if (result.error.code == TransportErrorCode::Incompatible) phase_ = RuntimePhase::Incompatible;
    else if (result.error.code == TransportErrorCode::RetentionMiss) phase_ = RuntimePhase::Connecting;
    else phase_ = RuntimePhase::Reconnecting;
    set_connectivity(false);
    (void)render();
    return {result.error.code == TransportErrorCode::Incompatible ?
                RuntimeError{RuntimeErrorCode::Protocol, result.error.message} :
                RuntimeError{RuntimeErrorCode::Network, result.error.message}};
  }
  switch (decoded.value.type) {
    case Envelope::Type::HelloAck:
      return apply_hello_ack(std::get<HelloAck>(decoded.value.payload));
    case Envelope::Type::Snapshot:
      if (transport_.live_snapshot().has_value()) {
        return apply_snapshot(*transport_.live_snapshot());
      }
      break;
    case Envelope::Type::ServerEvent:
      if (const auto* event = std::get_if<ServerEvent>(&decoded.value.payload);
          event != nullptr && event->snapshot.has_value()) {
        return apply_snapshot(*event->snapshot);
      }
    case Envelope::Type::Hello:
    case Envelope::Type::DeviceCommand:
      break;
  }
  return RuntimeResult::success();
}

RuntimeResult RuntimeCoordinator::on_socket_closed() {
  const bool enrollment = enrollment_socket_open_ && enrollment_requested_ && !paired_identity();
  (void)transport_.on_socket_closed();
  if (enrollment) {
    enrollment_socket_open_ = false;
    enrollment_id_.clear();
    pairing_code_.clear();
    enrollment_expires_at_.clear();
    phase_ = RuntimePhase::Provisioning;
    set_connectivity(false);
    next_connect_ms_ = dependencies_.clock.now_ms() + reconnect_backoff_.next(50).delay_ms;
    return render();
  }
  phase_ = RuntimePhase::Reconnecting;
  set_connectivity(false);
  (void)ptt_.on_disconnect(dependencies_.clock.now_ms());
  if (audio_stream_.has_value()) {
    (void)audio_stream_->on_disconnect();
    (void)capture_.disconnect();
    platform_.audio_stop();
    audio_stream_.reset();
  }
  next_connect_ms_ = dependencies_.clock.now_ms() + reconnect_backoff_.next(50).delay_ms;
  return render();
}

RuntimeResult RuntimeCoordinator::on_gateway_lost() {
  (void)transport_.on_gateway_lost();
  return on_socket_closed();
}

RuntimeResult RuntimeCoordinator::on_access_point_lost() {
  (void)transport_.on_access_point_lost();
  phase_ = RuntimePhase::Offline;
  set_connectivity(false);
  (void)ptt_.on_disconnect(dependencies_.clock.now_ms());
  return render();
}

RuntimeResult RuntimeCoordinator::on_transcript_proposed(std::string_view transcript) {
  if (transcript.size() > kMaxLongStringBytes) {
    return fail(RuntimeErrorCode::Protocol, "companion transcript exceeds its bounded limit");
  }
  return process_interaction(InteractionEvent::transcript(transcript,
                                                           dependencies_.clock.now_ms()));
}

RuntimeResult RuntimeCoordinator::configure_gateway(std::string_view gateway_url,
                                                      TransportSecurity security) {
  if (!booted_) return RuntimeResult::failure(RuntimeErrorCode::BoardInitialization,
                                              "companion runtime has not booted");
  if (security == TransportSecurity::TrustedLan && !config_.allow_trusted_lan) {
    return fail(RuntimeErrorCode::Configuration,
                "trusted-LAN companion transport requires explicit local configuration");
  }
  if (security == TransportSecurity::Remote && !config_.allow_remote) {
    return fail(RuntimeErrorCode::Configuration,
                "remote companion transport requires explicit TLS configuration");
  }
  const auto routes = build_companion_routes(gateway_url, security);
  if (!routes.ok()) return fail(RuntimeErrorCode::Configuration, routes.error.message);
  if (!identity_.set_gateway(gateway_url)) {
    return fail(RuntimeErrorCode::Persistence, "companion gateway could not be persisted");
  }
  config_.security = security;
  transport_configured_ = false;
  phase_ = identity_.value().scoped_token.empty() ? RuntimePhase::Provisioning
                                                   : RuntimePhase::Offline;
  enrollment_requested_ = identity_.value().scoped_token.empty() && !networks_.empty();
  update_gesture_context();
  if (enrollment_requested_) {
    const auto started = start_enrollment_transport();
    if (!started.ok()) return started;
  }
  return render();
}

RuntimeResult RuntimeCoordinator::submit_provisioning(std::string_view ssid,
                                                       std::string_view password,
                                                       std::string_view gateway_url,
                                                       TransportSecurity security) {
  if (gateway_url.empty() && ssid.empty()) {
    return fail(RuntimeErrorCode::Provisioning, "companion setup requires a Wi-Fi network or gateway");
  }
  if (!gateway_url.empty()) {
    const auto gateway = configure_gateway(gateway_url, security);
    if (!gateway.ok()) return gateway;
  }
  if (!ssid.empty()) {
    const auto network = append_wifi_network(ssid, password);
    if (!network.ok()) return network;
  }
  return RuntimeResult::success();
}

RuntimeResult RuntimeCoordinator::accept_enrollment_token(std::string_view token,
                                                           std::uint64_t expires_at_ms) {
  if (!identity_.has_value() || identity_.value().gateway_url.empty()) {
    return fail(RuntimeErrorCode::Configuration,
                "companion gateway must be configured before enrollment");
  }
  if (!transport_configured_) {
    const auto configured = transport_.configure(config_.security);
    if (!configured.ok()) return fail(RuntimeErrorCode::Configuration, configured.error.message);
    transport_configured_ = true;
  }
  const auto accepted = transport_.accept_enrollment_credential(token, expires_at_ms);
  if (!accepted.ok()) return fail(RuntimeErrorCode::Persistence, accepted.error.message);
  phase_ = RuntimePhase::Offline;
  return render();
}

RuntimeResult RuntimeCoordinator::begin_provisioning() {
  phase_ = RuntimePhase::Provisioning;
  provisioning_ap_started_ = false;
  const auto started = open_provisioning_ap();
  if (!started.ok()) return started;
  return render();
}

RuntimeResult RuntimeCoordinator::append_wifi_network(std::string_view ssid,
                                                       std::string_view password) {
  // An HTTP form submission is the explicit user action that authorizes the
  // setup station handoff.  The pure portal keeps this transition visible and
  // append-only; no background path can silently replace the current station.
  if (portal_.state() == PortalState::Advertising) {
    if (!portal_.request_setup_join() || !portal_.join_setup_ap()) {
      return fail(RuntimeErrorCode::Provisioning,
                  "companion setup station handoff was not accepted");
    }
  }
  if (!portal_.append_network(ssid, password)) {
    return fail(RuntimeErrorCode::Provisioning, "companion Wi-Fi credential was rejected");
  }
  if (enrollment_policy_allows() && !paired_identity()) {
    enrollment_requested_ = true;
    return start_enrollment_transport();
  }
  return RuntimeResult::success();
}

std::string RuntimeCoordinator::provisioning_status_json() const {
  const char* phase = "booting";
  switch (phase_) {
    case RuntimePhase::Provisioning: phase = "provisioning"; break;
    case RuntimePhase::Offline: phase = "offline"; break;
    case RuntimePhase::Connecting: phase = "connecting"; break;
    case RuntimePhase::Live: phase = "live"; break;
    case RuntimePhase::Reconnecting: phase = "reconnecting"; break;
    case RuntimePhase::Incompatible: phase = "incompatible"; break;
    case RuntimePhase::Fault: phase = "fault"; break;
    case RuntimePhase::Booting: break;
  }
  std::string result = "{\"ok\":true,\"phase\":\"";
  result += phase;
  result += "\",\"wifi_networks\":";
  result += std::to_string(networks_.size());
  result += ",\"gateway_configured\":";
  result += identity_.has_value() && !identity_.value().gateway_url.empty() ? "true" : "false";
  result += ",\"paired\":";
  result += paired_identity() ? "true" : "false";
  result += ",\"enrollment_pending\":";
  result += enrollment_requested_ ? "true" : "false";
  result += ",\"pairing_ready\":";
  result += (!pairing_code_.empty() && enrollment_socket_open_) ? "true" : "false";
  result += ",\"gateway_transport_enabled\":";
  result += config_.transport_enabled ? "true" : "false";
  result += ",\"snapshot_sequence\":";
  if (const auto& live_snapshot = transport_.live_snapshot(); live_snapshot.has_value()) {
    result += std::to_string(live_snapshot->adapter_sequence);
  } else {
    result += "0";
  }
  result += "}";
  return result;
}

void RuntimeCoordinator::mark_offline(RuntimePhase phase) {
  phase_ = phase;
  set_connectivity(false);
}

void RuntimeCoordinator::maybe_mark_offline_health() {
  if (offline_health_reported_ || dependencies_.health == nullptr ||
      config_.ota_offline_health_timeout_ms == 0U) {
    return;
  }
  const auto now_ms = dependencies_.clock.now_ms();
  if (now_ms < boot_started_ms_ || now_ms - boot_started_ms_ < config_.ota_offline_health_timeout_ms) {
    return;
  }
  dependencies_.health->mark_offline_health_timeout();
  offline_health_reported_ = true;
}

}  // namespace t3::companion::runtime
