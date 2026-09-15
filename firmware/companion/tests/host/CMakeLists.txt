cmake_minimum_required(VERSION 3.16)

project(t3_companion_protocol_host LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

enable_testing()

add_library(companion_protocol STATIC
  ${CMAKE_CURRENT_SOURCE_DIR}/../../components/companion_protocol/codec.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../../components/companion_protocol/audio_frame.cpp
)
target_include_directories(companion_protocol PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/../../components/companion_protocol/include
)
target_compile_options(companion_protocol PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_executable(protocol_test protocol_test.cpp)
target_link_libraries(protocol_test PRIVATE companion_protocol)
target_compile_definitions(protocol_test PRIVATE
  COMPANION_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../fixtures/companion/v1"
  COMPANION_FIXTURE_RUNNER="$<TARGET_FILE:fixture_runner>"
)
add_test(NAME companion_protocol_test COMMAND protocol_test)

add_executable(fixture_runner
  ${CMAKE_CURRENT_SOURCE_DIR}/../../simulator/fixture_runner.cpp
)
target_link_libraries(fixture_runner PRIVATE companion_protocol)
target_compile_options(fixture_runner PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_dependencies(protocol_test fixture_runner)
add_test(NAME companion_fixture_runner
  COMMAND fixture_runner ${CMAKE_CURRENT_SOURCE_DIR}/../../fixtures/companion/v1
)

add_library(companion_state STATIC
  ${CMAKE_CURRENT_SOURCE_DIR}/../../components/companion_state/snapshot_model.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../../components/companion_state/focus_queue.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../../components/companion_state/command_outbox.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../../components/companion_state/interaction_fsm.cpp
)
target_include_directories(companion_state PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/../../components/companion_state/include
  ${CMAKE_CURRENT_SOURCE_DIR}/../../components/companion_protocol/include
)
target_compile_options(companion_state PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(companion_state PUBLIC companion_protocol)

add_executable(focus_queue_test focus_queue_test.cpp)
target_link_libraries(focus_queue_test PRIVATE companion_state)
target_compile_options(focus_queue_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME companion_focus_queue_test COMMAND focus_queue_test)

add_executable(interaction_fsm_test interaction_fsm_test.cpp)
target_link_libraries(interaction_fsm_test PRIVATE companion_state)
target_compile_options(interaction_fsm_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME companion_interaction_fsm_test COMMAND interaction_fsm_test)

add_executable(command_outbox_test command_outbox_test.cpp)
target_link_libraries(command_outbox_test PRIVATE companion_state)
target_compile_options(command_outbox_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME companion_command_outbox_test COMMAND command_outbox_test)

add_library(companion_persistence STATIC
  ${CMAKE_CURRENT_SOURCE_DIR}/../../components/persistence/credential_store.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../../components/persistence/device_identity.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../../components/persistence/pending_commands.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../../components/persistence/snapshot_store.cpp
)
target_include_directories(companion_persistence PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/../../components/persistence/include
  ${CMAKE_CURRENT_SOURCE_DIR}/../../components/companion_protocol/include
)
target_compile_options(companion_persistence PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(companion_persistence PUBLIC companion_protocol)

add_library(companion_wifi_provisioning STATIC
  ${CMAKE_CURRENT_SOURCE_DIR}/../../components/wifi_provisioning/network_store.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../../components/wifi_provisioning/softap_portal.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../../components/wifi_provisioning/roaming.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../../components/wifi_provisioning/gateway_discovery.cpp
)
target_include_directories(companion_wifi_provisioning PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/../../components/wifi_provisioning/include
  ${CMAKE_CURRENT_SOURCE_DIR}/../../components/persistence/include
  ${CMAKE_CURRENT_SOURCE_DIR}/../../components/companion_protocol/include
)
target_compile_options(companion_wifi_provisioning PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(companion_wifi_provisioning PUBLIC companion_persistence)

add_executable(network_store_test network_store_test.cpp)
target_link_libraries(network_store_test PRIVATE companion_wifi_provisioning companion_persistence)
target_compile_options(network_store_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME companion_network_store_test COMMAND network_store_test)

add_executable(persistence_test persistence_test.cpp)
target_link_libraries(persistence_test PRIVATE companion_persistence companion_wifi_provisioning)
target_compile_options(persistence_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME companion_persistence_test COMMAND persistence_test)

add_library(companion_transport STATIC
  ${CMAKE_CURRENT_SOURCE_DIR}/../../components/transport/reconnect.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../../components/transport/http_client.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../../components/transport/wifi_ws_client.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../../components/transport/transport.cpp
)
target_include_directories(companion_transport PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/../../components/transport/include
  ${CMAKE_CURRENT_SOURCE_DIR}/../../components/persistence/include
  ${CMAKE_CURRENT_SOURCE_DIR}/../../components/companion_state/include
  ${CMAKE_CURRENT_SOURCE_DIR}/../../components/companion_protocol/include
)
target_compile_options(companion_transport PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(companion_transport PUBLIC companion_persistence companion_state)

add_executable(transport_test transport_test.cpp)
target_link_libraries(transport_test PRIVATE companion_transport)
target_compile_options(transport_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME companion_transport_test COMMAND transport_test)

add_library(companion_ota STATIC
  ${CMAKE_CURRENT_SOURCE_DIR}/../../components/ota/manifest.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../../components/ota/https_ota.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../../components/ota/rollback.cpp
)
target_include_directories(companion_ota PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/../../components/ota/include
)
target_compile_options(companion_ota PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_executable(ota_test ota_test.cpp)
target_link_libraries(ota_test PRIVATE companion_ota)
target_compile_options(ota_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME companion_ota_test COMMAND ota_test)

add_executable(board_manifest_test
  ${CMAKE_CURRENT_SOURCE_DIR}/../../components/board_waveshare/board_manifest.cpp
  board_manifest_test.cpp
)
target_include_directories(board_manifest_test PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../../components/board_waveshare/include
)
target_compile_options(board_manifest_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME companion_board_manifest_test COMMAND board_manifest_test)

add_library(companion_ui STATIC
  ${CMAKE_CURRENT_SOURCE_DIR}/../../components/ui/ambient_view.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../../components/ui/prompt_cards.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../../components/ui/transcript_card.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../../components/ui/ring_renderer.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../../components/ui/overlays.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../../components/ui/pixel_shift.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../../simulator/framebuffer_renderer.cpp
)
target_include_directories(companion_ui PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/../../components/ui/include
  ${CMAKE_CURRENT_SOURCE_DIR}/../../components/companion_state/include
  ${CMAKE_CURRENT_SOURCE_DIR}/../../components/companion_protocol/include
)
target_compile_options(companion_ui PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(companion_ui PUBLIC companion_state)

add_executable(ui_model_test ui_model_test.cpp)
target_link_libraries(ui_model_test PRIVATE companion_ui)
target_compile_options(ui_model_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME companion_ui_model_test COMMAND ui_model_test)

add_library(companion_touch_gestures STATIC
  ${CMAKE_CURRENT_SOURCE_DIR}/../../components/touch_gestures/touch_reader.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../../components/touch_gestures/gesture_recognizer.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../../components/touch_gestures/card_physics.cpp
)
target_include_directories(companion_touch_gestures PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/../../components/touch_gestures/include
  ${CMAKE_CURRENT_SOURCE_DIR}/../../components/companion_state/include
  ${CMAKE_CURRENT_SOURCE_DIR}/../../components/companion_protocol/include
)
target_compile_options(companion_touch_gestures PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(companion_touch_gestures PUBLIC companion_state)

add_executable(gesture_recognizer_test gesture_recognizer_test.cpp)
target_link_libraries(gesture_recognizer_test PRIVATE companion_touch_gestures)
target_compile_options(gesture_recognizer_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME companion_gesture_recognizer_test COMMAND gesture_recognizer_test)

add_executable(card_physics_test card_physics_test.cpp)
target_link_libraries(card_physics_test PRIVATE companion_touch_gestures)
target_compile_options(card_physics_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME companion_card_physics_test COMMAND card_physics_test)

add_library(companion_buttons_power STATIC
  ${CMAKE_CURRENT_SOURCE_DIR}/../../components/buttons_power/button_input.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../../components/buttons_power/debounce.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../../components/buttons_power/double_click.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../../components/buttons_power/sleep_lock.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../../components/buttons_power/ptt_button.cpp
)
target_include_directories(companion_buttons_power PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/../../components/buttons_power/include
)
target_compile_options(companion_buttons_power PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_executable(buttons_power_test buttons_power_test.cpp)
target_link_libraries(buttons_power_test PRIVATE companion_buttons_power)
target_compile_options(buttons_power_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME companion_buttons_power_test COMMAND buttons_power_test)

add_library(companion_audio_capture STATIC
  ${CMAKE_CURRENT_SOURCE_DIR}/../../components/board_waveshare/board_manifest.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../../components/audio_capture/es7210_capture.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../../components/audio_capture/pcm_downmix.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../../components/audio_capture/encoder.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../../components/audio_capture/backpressure.cpp
)
target_include_directories(companion_audio_capture PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/../../components/audio_capture/include
  ${CMAKE_CURRENT_SOURCE_DIR}/../../components/board_waveshare/include
  ${CMAKE_CURRENT_SOURCE_DIR}/../../components/companion_protocol/include
  ${CMAKE_CURRENT_SOURCE_DIR}/../../components/transport/include
)
target_compile_options(companion_audio_capture PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(companion_audio_capture PUBLIC companion_protocol)

add_executable(audio_capture_test audio_capture_test.cpp)
target_link_libraries(audio_capture_test PRIVATE companion_audio_capture companion_state companion_transport)
target_compile_options(audio_capture_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME companion_audio_capture_test COMMAND audio_capture_test)

add_executable(integration_test integration_test.cpp)
target_link_libraries(integration_test PRIVATE
  companion_transport
  companion_wifi_provisioning
  companion_ui
  companion_touch_gestures
  companion_buttons_power
  companion_audio_capture
  companion_ota
)
target_compile_definitions(integration_test PRIVATE
  COMPANION_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../fixtures/companion/v1"
  COMPANION_FIXTURE_RUNNER="$<TARGET_FILE:fixture_runner>"
)
target_compile_options(integration_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_dependencies(integration_test fixture_runner)
add_test(NAME companion_integration_test COMMAND integration_test)

add_executable(runtime_coordinator_test runtime_coordinator_test.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../../components/runtime/runtime.cpp)
target_include_directories(runtime_coordinator_test PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../../components/runtime/include
  ${CMAKE_CURRENT_SOURCE_DIR}/../../components/transport/include
  ${CMAKE_CURRENT_SOURCE_DIR}/../../components/persistence/include
  ${CMAKE_CURRENT_SOURCE_DIR}/../../components/wifi_provisioning/include
  ${CMAKE_CURRENT_SOURCE_DIR}/../../components/companion_state/include
  ${CMAKE_CURRENT_SOURCE_DIR}/../../components/companion_protocol/include
  ${CMAKE_CURRENT_SOURCE_DIR}/../../components/ui/include
  ${CMAKE_CURRENT_SOURCE_DIR}/../../components/touch_gestures/include
  ${CMAKE_CURRENT_SOURCE_DIR}/../../components/buttons_power/include
  ${CMAKE_CURRENT_SOURCE_DIR}/../../components/audio_capture/include
  ${CMAKE_CURRENT_SOURCE_DIR}/../../components/board_waveshare/include)
target_compile_options(runtime_coordinator_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_compile_definitions(runtime_coordinator_test PRIVATE
  COMPANION_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../fixtures/companion/v1")
target_link_libraries(runtime_coordinator_test PRIVATE
  companion_transport
  companion_wifi_provisioning
  companion_ui
  companion_touch_gestures
  companion_buttons_power
  companion_audio_capture)
add_test(NAME companion_runtime_coordinator_test COMMAND runtime_coordinator_test)

add_library(companion_ws_text_assembler STATIC
  ${CMAKE_CURRENT_SOURCE_DIR}/../../components/runtime/ws_text_assembler.cpp
)
target_include_directories(companion_ws_text_assembler PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/../../components/runtime/include
)
target_compile_options(companion_ws_text_assembler PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_executable(websocket_assembler_test websocket_assembler_test.cpp)
target_link_libraries(websocket_assembler_test PRIVATE companion_ws_text_assembler)
target_compile_options(websocket_assembler_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME companion_websocket_assembler_test COMMAND websocket_assembler_test)
