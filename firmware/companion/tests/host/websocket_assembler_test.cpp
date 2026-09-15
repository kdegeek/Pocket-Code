#include "ws_text_assembler.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

using t3::companion::runtime::WsTextMessageAssembler;

int failures = 0;

void expect(bool condition, const char* message) {
  if (!condition) {
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
  }
}

void test_split_large_text_emits_only_complete_payload() {
  const std::string payload =
      "{\"type\":\"snapshot\",\"padding\":\"" +
      std::string(1310U, 'x') + "\"}";
  expect(payload.size() > 1024U, "fixture is larger than the IDF receive buffer");

  WsTextMessageAssembler assembler(65'536U);
  std::vector<std::string> delivered;
  const auto first = assembler.accept(payload.substr(0, 1024U),
                                      static_cast<std::int32_t>(payload.size()),
                                      0, WsTextMessageAssembler::kTextOpcode, true);
  if (first.has_value()) delivered.emplace_back(*first);
  expect(delivered.empty(), "first buffer-sized chunk is not emitted as JSON");

  const auto second = assembler.accept(
      payload.substr(1024U), static_cast<std::int32_t>(payload.size()), 1024,
      WsTextMessageAssembler::kTextOpcode, true);
  if (second.has_value()) delivered.emplace_back(*second);
  expect(delivered.size() == 1U, "one complete payload produces one callback");
  expect(delivered.size() == 1U && delivered.front() == payload,
         "callback contains the complete split payload");
}

void test_malformed_offset_rejects_and_clears_partial() {
  const std::string payload = "{\"ok\":true}";
  WsTextMessageAssembler assembler(65'536U);
  const auto first = assembler.accept(payload.substr(0, 3U),
                                      static_cast<std::int32_t>(payload.size()),
                                      0, WsTextMessageAssembler::kTextOpcode, true);
  expect(!first.has_value() && assembler.active(),
         "partial text payload remains pending before the next offset");

  const auto malformed = assembler.accept(
      payload.substr(3U), static_cast<std::int32_t>(payload.size()), 4,
      WsTextMessageAssembler::kTextOpcode, true);
  expect(!malformed.has_value() && !assembler.active() &&
             assembler.buffered_bytes() == 0U,
         "out-of-order offset rejects and clears the partial payload");

  const auto recovered = assembler.accept(
      payload, static_cast<std::int32_t>(payload.size()), 0,
      WsTextMessageAssembler::kTextOpcode, true);
  expect(recovered.has_value() && *recovered == payload,
         "a valid message can start after malformed input");
}

void test_declared_oversize_is_rejected_before_allocation() {
  WsTextMessageAssembler assembler(64U);
  const auto oversize = assembler.accept("x", 65, 0,
                                        WsTextMessageAssembler::kTextOpcode, true);
  expect(!oversize.has_value() && !assembler.active() &&
             assembler.buffered_bytes() == 0U,
         "oversize declared payload is rejected before buffering");

  const std::string allowed(64U, 'y');
  const auto accepted = assembler.accept(
      allowed, 64, 0, WsTextMessageAssembler::kTextOpcode, true);
  expect(accepted.has_value() && *accepted == allowed,
         "payload at the configured bound remains accepted");
}

}  // namespace

int main() {
  test_split_large_text_emits_only_complete_payload();
  test_malformed_offset_rejects_and_clears_partial();
  test_declared_oversize_is_rejected_before_allocation();
  if (failures != 0) {
    std::cerr << failures << " websocket assembler test(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "T3 Companion websocket assembler tests passed\n";
  return EXIT_SUCCESS;
}
