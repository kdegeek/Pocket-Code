#include "audio_frame.hpp"
#include "envelope.hpp"
#include "types.hpp"

#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>

namespace {

using namespace t3::companion;

int failures = 0;

void expect(bool condition, const char* message) {
  if (!condition) {
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
  }
}

std::string read_fixture(const char* name) {
  std::ifstream input(std::filesystem::path(COMPANION_FIXTURE_DIR) / name,
                      std::ios::binary);
  std::ostringstream buffer;
  buffer << input.rdbuf();
  return buffer.str();
}

std::string repeated_json_array(std::size_t count) {
  std::string result = "[";
  for (std::size_t index = 0; index < count; ++index) {
    if (index != 0) {
      result += ',';
    }
    result += "\"command-" + std::to_string(index) + "\"";
  }
  result += ']';
  return result;
}

void test_fixture_decode_and_encode() {
  const auto hello = decode_hello(read_fixture("hello.json"));
  expect(hello.ok(), "canonical hello fixture decodes");
  if (!hello.ok()) {
    return;
  }
  expect(hello.value.device_id.view() == "device-fixture-01", "hello device id is typed");
  expect(hello.value.protocol_version == kProtocolVersion, "hello uses protocol v1");
  expect(hello.value.pending_command_count == 1, "hello pending commands are bounded");
  expect(hello.value.capabilities.audio.has_value(), "hello advertises audio capability");

  const auto encoded = encode_hello(hello.value);
  expect(encoded.ok(), "hello encoding succeeds");
  if (encoded.ok()) {
    const auto round_trip = decode_hello(encoded.value);
    expect(round_trip.ok(), "encoded hello decodes again");
    expect(round_trip.ok() && round_trip.value.device_id == hello.value.device_id,
           "hello round trip preserves identity");
  }
}

void test_snapshot_and_simultaneous_requests() {
  const auto snapshot = decode_snapshot(read_fixture("snapshot.json"));
  expect(snapshot.ok(), "canonical snapshot fixture decodes");
  if (!snapshot.ok()) {
    return;
  }
  expect(snapshot.value.work_item_count == 6, "snapshot preserves bounded work items");
  expect(snapshot.value.pending_request_count == 2,
         "snapshot preserves simultaneous pending requests");
  expect(snapshot.value.pending_requests[1].kind == PendingRequestKind::UserInput,
         "snapshot preserves user-input request kind");
  expect(snapshot.value.pending_requests[1].question_count == 2,
         "snapshot preserves multi-question input");
  expect(snapshot.value.pending_requests[1].questions[1].multi_select,
         "snapshot preserves multi-select question");
  expect(snapshot.value.usage.provider_count == 3, "snapshot preserves provider usage states");
  expect(snapshot.value.usage.providers[2].status == UsageStatus::Unavailable,
         "snapshot preserves unavailable provider state");
}

void test_snapshot_server_event_wrapper() {
  const auto snapshot = read_fixture("snapshot.json");
  const std::string wrapped =
      "{\"type\":\"snapshot\",\"protocolVersion\":1,\"sessionId\":\"session-1\","
      "\"adapterSequence\":7,\"eventId\":\"event-1\",\"payload\":" + snapshot + "}";
  const auto decoded = decode_envelope(wrapped);
  expect(decoded.ok(), "live snapshot server-event wrapper decodes");
  expect(decoded.ok() && decoded.value.type == Envelope::Type::ServerEvent,
         "wrapped snapshot remains a server event envelope");
  if (decoded.ok() && decoded.value.type == Envelope::Type::ServerEvent) {
    const auto& event = std::get<ServerEvent>(decoded.value.payload);
    expect(event.snapshot.has_value(), "wrapped snapshot payload is retained");
    expect(event.snapshot.has_value() && event.snapshot->work_item_count == 6,
           "wrapped snapshot payload preserves bounded state");
  }
}

void test_version_type_bounds_and_malformed_json() {
  const auto unknown_version = decode_envelope(
      R"({"type":"hello","protocolVersion":2,"deviceId":"device","supportedVersions":{"min":1,"max":2},"lastAcceptedAdapterSequence":0,"pendingCommandIds":[],"firmware":{"version":"1"},"capabilities":{"maxMessageBytes":1,"maxPendingCommands":1,"maxPendingRequests":1}})");
  expect(!unknown_version.ok() && unknown_version.error.code == DecodeError::UnsupportedVersion,
         "unknown protocol version is rejected");

  const auto unknown_type = decode_envelope(R"({"type":"future","protocolVersion":1})");
  expect(!unknown_type.ok() && unknown_type.error.code == DecodeError::UnknownType,
         "unknown message type is rejected");

  const auto malformed = decode_envelope(R"({"type":"hello","protocolVersion":1)");
  expect(!malformed.ok() && malformed.error.code == DecodeError::MalformedJson,
         "malformed JSON is rejected");

  const auto oversized_array = decode_hello(
      "{\"type\":\"hello\",\"protocolVersion\":1,\"deviceId\":\"device\","
      "\"supportedVersions\":{\"min\":1,\"max\":1},\"lastAcceptedAdapterSequence\":0,"
      "\"pendingCommandIds\":" + repeated_json_array(kMaxPendingCommands + 1) +
      ",\"firmware\":{\"version\":\"1\"},\"capabilities\":{\"maxMessageBytes\":1,"
      "\"maxPendingCommands\":1,\"maxPendingRequests\":1}}");
  expect(!oversized_array.ok() && oversized_array.error.code == DecodeError::LimitExceeded,
         "oversized arrays are rejected at the protocol bound");

  const auto oversized_string = decode_hello(
      "{\"type\":\"hello\",\"protocolVersion\":1,\"deviceId\":\"" +
      std::string(kMaxIdBytes + 1, 'd') +
      "\",\"supportedVersions\":{\"min\":1,\"max\":1},\"lastAcceptedAdapterSequence\":0,"
      "\"pendingCommandIds\":[],\"firmware\":{\"version\":\"1\"},\"capabilities\":{\"maxMessageBytes\":1,"
      "\"maxPendingCommands\":1,\"maxPendingRequests\":1}}");
  expect(!oversized_string.ok() && oversized_string.error.code == DecodeError::LimitExceeded,
         "oversized strings are rejected at the protocol bound");
}

void test_command_context_status_and_provisioning() {
  const auto valid_cancel = decode_device_command(
      R"({"type":"cancel_turn","protocolVersion":1,"commandId":"command-1","deviceId":"device-1","environmentId":"env-1","projectId":"project-1","threadId":"thread-1","expectedTurnId":"turn-1"})");
  expect(valid_cancel.ok(), "cancel command with immutable context decodes");
  expect(valid_cancel.ok() && valid_cancel.value.context.thread_id.view() == "thread-1",
         "command retains captured thread context");

  const auto missing_context = decode_device_command(
      R"({"type":"cancel_turn","protocolVersion":1,"commandId":"command-1","deviceId":"device-1","threadId":"thread-1"})");
  expect(!missing_context.ok() && missing_context.error.code == DecodeError::MissingField,
         "mutating command without immutable context is rejected");

  const auto decision = decode_device_command(
      R"({"type":"answer_prompt","protocolVersion":1,"commandId":"command-2","deviceId":"device-1","environmentId":"env-1","projectId":"project-1","threadId":"thread-1","expectedTurnId":"turn-1","requestId":"request-1","decision":"accept"})");
  expect(decision.ok() && decision.value.payload_kind == DeviceCommandPayload::Decision,
         "answer prompt accepts exactly one decision payload");

  const auto both_payloads = decode_device_command(
      R"({"type":"answer_prompt","protocolVersion":1,"commandId":"command-2","deviceId":"device-1","environmentId":"env-1","projectId":"project-1","threadId":"thread-1","expectedTurnId":"turn-1","requestId":"request-1","decision":"accept","answers":{"question":"yes"}})");
  expect(!both_payloads.ok() && both_payloads.error.code == DecodeError::InvalidValue,
         "answer prompt rejects decision and answers together");

  const auto statuses = decode_command_statuses(read_fixture("command-statuses.json"));
  expect(statuses.ok() && statuses.value.count() == 4, "command status fixture decodes");
  expect(statuses.ok() && statuses.value.records[2].status == CommandStatus::Applied,
         "command status lifecycle includes applied");

  const auto duplicate_statuses = decode_command_statuses(
      R"([{"commandId":"same","status":"received","updatedAt":"now"},{"commandId":"same","status":"rejected","updatedAt":"later"}])");
  expect(!duplicate_statuses.ok() && duplicate_statuses.error.code == DecodeError::Duplicate,
         "duplicate command statuses are rejected");

  const auto provisioning = decode_device_command(
      R"({"type":"provisioning_status","protocolVersion":1,"commandId":"command-3","deviceId":"device-1","provisioningRequestId":"provision-1","status":"completed"})");
  expect(provisioning.ok() && provisioning.value.payload_kind == DeviceCommandPayload::Provisioning,
         "provisioning lifecycle status decodes");
}

void test_replay_and_audio_ordering() {
  ReplayGuard replay;
  expect(replay.accept(7), "first replay sequence is accepted");
  expect(!replay.accept(7), "duplicate replay sequence is rejected");
  expect(!replay.accept(6), "out-of-order replay sequence is rejected");
  expect(replay.accept(8), "next replay sequence is accepted");

  const auto audio = read_fixture("audio-frame.bin");
  const auto frame = decode_audio_frame(
      std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(audio.data()), audio.size()),
      4096);
  expect(frame.ok(), "canonical audio fixture decodes");
  expect(frame.ok() && frame.value.header.chunk_number == 2, "audio chunk number is monotonic metadata");
  expect(frame.ok() && frame.value.payload.size() == 8, "audio payload is bounded and framed");
  if (frame.ok()) {
    const auto encoded = encode_audio_frame(frame.value.header, frame.value.payload, 4096);
    expect(encoded.ok(), "audio fixture re-encodes");
    expect(encoded.ok() && encoded.value.size() == audio.size() &&
               std::equal(encoded.value.begin(), encoded.value.end(),
                          reinterpret_cast<const std::uint8_t*>(audio.data())),
           "audio encode preserves the canonical fixed frame");
  }

  const auto truncated = decode_audio_frame(
      std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(audio.data()),
                                    kAudioFrameHeaderBytes - 1),
      4096);
  expect(!truncated.ok() && truncated.error.code == DecodeError::Truncated,
         "truncated audio header is rejected");

  AudioStreamGuard stream;
  expect(stream.start(frame.value.header.stream_id, frame.value.header.chunk_number), "audio stream starts");
  expect(stream.accept(frame.value.header.chunk_number), "first active audio chunk is accepted");
  expect(!stream.accept(frame.value.header.chunk_number), "duplicate audio chunk is rejected");
  expect(!stream.accept(frame.value.header.chunk_number - 1), "out-of-order audio chunk is rejected");
  expect(stream.accept(frame.value.header.chunk_number + 1), "next audio chunk is accepted");
  expect(!stream.accept(3), "stream does not accept a stale reset chunk");

  AudioStreamId unknown_stream = frame.value.header.stream_id;
  unknown_stream.bytes[0] ^= 0xffU;
  expect(!stream.accept(unknown_stream, 4), "unknown audio stream is rejected");
}

void test_manifest_hash_mismatch() {
  const auto temporary = std::filesystem::temp_directory_path() / "t3-companion-fixture-mismatch";
  std::error_code cleanup_error;
  std::filesystem::remove_all(temporary, cleanup_error);
  std::filesystem::create_directories(temporary);
  for (const auto& entry : std::filesystem::directory_iterator(COMPANION_FIXTURE_DIR)) {
    std::filesystem::copy_file(entry.path(), temporary / entry.path().filename(),
                               std::filesystem::copy_options::overwrite_existing);
  }
  const auto manifest_path = temporary / "manifest.json";
  std::ifstream input(manifest_path, std::ios::binary);
  std::ostringstream bytes;
  bytes << input.rdbuf();
  input.close();
  std::string manifest = bytes.str();
  const std::string needle = "e78d03fe5198cfceff84e9d97298e7f72fce87657a9672e576215f32dba9c57f";
  const auto offset = manifest.find(needle);
  if (offset != std::string::npos) manifest.replace(offset, needle.size(), std::string(needle.size(), '0'));
  std::ofstream output(manifest_path, std::ios::binary | std::ios::trunc);
  output << manifest;
  output.close();
  const std::string command = std::string("\"") + COMPANION_FIXTURE_RUNNER + "\" \"" +
                              temporary.string() + "\" >/dev/null 2>&1";
  const int status = std::system(command.c_str());
  expect(status != 0, "fixture runner rejects a manifest hash mismatch");
  std::filesystem::remove_all(temporary, cleanup_error);
}

}  // namespace

int main() {
  test_fixture_decode_and_encode();
  test_snapshot_and_simultaneous_requests();
  test_snapshot_server_event_wrapper();
  test_version_type_bounds_and_malformed_json();
  test_command_context_status_and_provisioning();
  test_replay_and_audio_ordering();
  test_manifest_hash_mismatch();

  if (failures != 0) {
    std::cerr << failures << " protocol test(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "T3 Companion protocol tests passed\n";
  return EXIT_SUCCESS;
}
