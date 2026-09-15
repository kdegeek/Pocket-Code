#include "audio_frame.hpp"
#include "backpressure.hpp"
#include "board_manifest.h"
#include "encoder.hpp"
#include "es7210_capture.hpp"
#include "interaction_fsm.hpp"
#include "pcm_downmix.hpp"
#include "wifi_ws_client.hpp"

#include <array>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace {

using namespace t3::companion;
using t3::board::AudioManifest;

int failures = 0;

class BinaryWebSocket final : public WebSocketClient {
 public:
  bool open(std::string_view, std::string_view) override { return true; }
  bool send_text(std::string_view) override { return true; }
  bool send_binary(std::span<const std::uint8_t> frame) override {
    frames.emplace_back(frame.begin(), frame.end());
    return true;
  }
  void close() override {}

  std::vector<std::vector<std::uint8_t>> frames;
};

void expect(bool condition, const char* message) {
  if (!condition) {
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
  }
}

Id id(const char* value) {
  Id result;
  (void)result.assign(value);
  return result;
}

PendingRequest request(const char* thread) {
  PendingRequest result;
  result.environment_id = id("environment");
  result.project_id = id("project");
  result.thread_id = id(thread);
  result.expected_turn_id = id("turn");
  result.request_id = id("request");
  result.kind = PendingRequestKind::Approval;
  (void)result.available_decisions.push(ApprovalDecision::Accept);
  (void)result.available_decisions.push(ApprovalDecision::Decline);
  (void)result.available_decisions.push(ApprovalDecision::Cancel);
  return result;
}

AudioManifest verified_audio_manifest() {
  AudioManifest manifest = t3::board::verified_manifest().audio;
  manifest.front_mic_channels_verified = true;
  manifest.mic_unresolved = false;
  return manifest;
}

void test_manifest_gate_and_fixture_downmix() {
  auto unresolved = t3::board::verified_manifest().audio;
  unresolved.front_mic_channels_verified = false;
  unresolved.mic_unresolved = true;
  Es7210Capture blocked(unresolved);
  expect(!blocked.available(), "ES7210 capture is unavailable for unresolved manifest");
  expect(blocked.start() == CaptureResult::Unavailable,
         "unresolved microphone mapping fails closed at capture start");

  Es7210Capture capture(verified_audio_manifest());
  expect(capture.available(), "host fixture may opt into an explicitly verified mapping");
  expect(capture.start() == CaptureResult::Started, "verified fixture starts capture");
  const std::array<std::int16_t, 4> stereo = {100, 300, -100, -300};
  std::vector<std::int16_t> mono;
  expect(capture.push_stereo(stereo, mono) == CaptureResult::Accepted,
         "capture downmixes a complete stereo host fixture");
  expect(mono == std::vector<std::int16_t>({200, -200}),
         "capture averages verified ES7210 channels 0 and 1");
  expect(capture.stop() == CaptureResult::Stopped, "release stops host capture");
  expect(capture.push_stereo(stereo, mono) == CaptureResult::NotStarted,
         "capture rejects samples after release");
}

void test_saturating_pcm_encoder() {
  const std::array<std::int16_t, 6> stereo = {
      32767, 32767, -32768, -32768, 32767, -32768};
  const auto mono = downmix_stereo_to_mono(stereo);
  expect(mono == std::vector<std::int16_t>({32767, -32768, 0}),
         "stereo downmix saturates without 16-bit overflow");

  PcmS16leEncoder encoder;
  std::vector<std::uint8_t> bytes;
  expect(encoder.encode(mono, bytes) == EncoderResult::Encoded,
         "PCM encoder emits signed little-endian bytes");
  expect(bytes.size() == 6 && bytes[0] == 0xffU && bytes[1] == 0x7fU &&
             bytes[2] == 0x00U && bytes[3] == 0x80U,
         "PCM encoder preserves little-endian sample order");
  expect(encoder.sample_rate_hz() == 24'000 && encoder.channels() == 1,
         "encoder advertises negotiated 24 kHz mono format");
}

void test_bounded_stream_lifecycle_and_framing() {
  AudioStreamId stream_id;
  stream_id.bytes[0] = 0x2aU;
  AudioStreamSession session({stream_id, 2, 64});
  expect(session.start() == StreamResult::Started, "audio stream starts at chunk zero");

  const std::array<std::uint8_t, 4> first = {1, 2, 3, 4};
  expect(session.push(stream_id, first) == StreamResult::Accepted,
         "first bounded audio payload is accepted");
  expect(session.push(stream_id, first) == StreamResult::Accepted,
         "second bounded audio payload is accepted");
  expect(session.push(stream_id, first) == StreamResult::Backpressure,
         "bounded stream rejects a third payload instead of growing unbounded");

  const auto frame0 = session.pop_frame();
  const auto frame1 = session.pop_frame();
  expect(frame0.has_value() && frame1.has_value(), "queued chunks can be drained");
  if (frame0.has_value() && frame1.has_value()) {
    const auto decoded0 = decode_audio_frame(*frame0, 64);
    const auto decoded1 = decode_audio_frame(*frame1, 64);
    expect(decoded0.ok() && decoded1.ok() && decoded0.value.header.chunk_number == 0 &&
               decoded1.value.header.chunk_number == 1,
           "audio frames carry monotonic chunk numbers");
  }
  AudioStreamId unknown = stream_id;
  unknown.bytes[1] = 0xffU;
  expect(session.push(unknown, first) == StreamResult::UnknownStream,
         "unknown audio stream is rejected");
  expect(session.on_server_stop(stream_id) == StreamResult::Stopped,
         "server backpressure stop releases the active stream");
  expect(session.push(stream_id, first) == StreamResult::Stopped,
         "server stop prevents further capture");
  expect(session.start() == StreamResult::Started, "a new stream can be started explicitly");
  expect(session.on_disconnect() == StreamResult::Disconnected,
         "disconnect cancels and clears buffered audio");
  expect(session.pending_bytes() == 0 && !session.active(),
         "disconnect leaves no audio buffer or active stream");
}

void test_binary_transport_is_explicitly_injected() {
  BinaryWebSocket websocket;
  const std::array<std::uint8_t, 2> payload = {0xaaU, 0x55U};
  expect(send_companion_audio_frame(websocket, payload),
         "audio binary transport uses the injected websocket seam");
  expect(websocket.frames.size() == 1 && websocket.frames.front() ==
             std::vector<std::uint8_t>({0xaaU, 0x55U}),
         "binary transport forwards exactly one ephemeral frame");
  expect(!send_companion_audio_frame(websocket, {}),
         "empty binary frames are rejected before transport");
}

void test_transcript_confirmation_freezes_context() {
  InteractionState state = make_interaction_state(true);
  state = reduce_interaction(state, InteractionEvent::open(request("thread-a"), 10)).state;
  const TargetContext frozen = state.target;
  const Id stream_id = id("stream-a");
  state = reduce_interaction(state, InteractionEvent::stt_start(stream_id, frozen, 11)).state;
  state = reduce_interaction(state, InteractionEvent::open(request("thread-b"), 12)).state;
  state = reduce_interaction(state, InteractionEvent::transcript("hello", 13)).state;
  const auto submitted = reduce_interaction(
      state, InteractionEvent::gesture(Gesture::SwipeRight, 14));
  expect(submitted.effects.size() == 2 && submitted.effects[1].command.has_value(),
         "right swipe submits a final transcript exactly once");
  if (submitted.effects.size() == 2 && submitted.effects[1].command.has_value()) {
    const auto& command = *submitted.effects[1].command;
    expect(command.type == DeviceCommandType::SubmitTranscript &&
               command.context.thread_id == id("thread-a") && command.stt_stream_id == stream_id,
           "submit uses the frozen STT target and stream context");
    expect(submitted.effects[0].command.has_value() &&
               submitted.effects[0].command->transcript.empty(),
           "durable persistence effect excludes transcript text");
  }

  state = make_interaction_state(true);
  state = reduce_interaction(state, InteractionEvent::open(request("thread-a"), 20)).state;
  state = reduce_interaction(state, InteractionEvent::stt_start(stream_id, state.target, 21)).state;
  state = reduce_interaction(state, InteractionEvent::transcript("discard me", 22)).state;
  const auto discarded = reduce_interaction(state,
                                            InteractionEvent::gesture(Gesture::SwipeLeft, 23));
  expect(discarded.effects.size() == 1 && discarded.effects[0].kind == EffectKind::RestoreAmbient &&
             discarded.state.transcript == std::nullopt,
         "left swipe discards transcript without forwarding or retaining text");
}

}  // namespace

int main() {
  test_manifest_gate_and_fixture_downmix();
  test_saturating_pcm_encoder();
  test_bounded_stream_lifecycle_and_framing();
  test_binary_transport_is_explicitly_injected();
  test_transcript_confirmation_freezes_context();
  if (failures != 0) {
    std::cerr << failures << " audio capture test(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "T3 Companion audio capture tests passed\n";
  return EXIT_SUCCESS;
}
