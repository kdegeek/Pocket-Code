#pragma once

#include "runtime.hpp"
#include "ws_text_assembler.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "driver/i2c_master.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_codec_dev.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "nvs.h"
#include "lvgl.h"

namespace t3::companion::runtime {

/** ESP timer + FreeRTOS delay adapter. */
class IdfClock final : public RuntimeClock {
 public:
  [[nodiscard]] std::uint64_t now_ms() const override;
  void sleep_ms(std::uint32_t delay_ms) override;
};

/** A two-slot journal backing store in a private NVS namespace. */
class IdfNvsSlotStorage final : public SlotStorage {
 public:
  explicit IdfNvsSlotStorage(std::string_view name_space);
  ~IdfNvsSlotStorage() override;

  [[nodiscard]] bool begin();
  [[nodiscard]] bool ready() const { return handle_ != 0; }
  [[nodiscard]] std::optional<std::vector<std::uint8_t>> read_slot(
      std::size_t slot) const override;
  [[nodiscard]] bool write_slot(std::size_t slot,
                                std::span<const std::uint8_t> bytes) override;

 private:
  [[nodiscard]] const char* key_for(std::size_t slot) const;

  std::string namespace_;
  nvs_handle_t handle_ = 0;
};

/** FreeRTOS queue adapter for the fixed-size ISR/task input messages. */
class IdfRuntimeQueue final : public RuntimeInputQueue {
 public:
  explicit IdfRuntimeQueue(std::size_t capacity = FixedRuntimeInputQueue::kCapacity);
  ~IdfRuntimeQueue() override;

  [[nodiscard]] bool valid() const { return queue_ != nullptr; }
  [[nodiscard]] bool push(const RuntimeInputEvent& event) override;
  [[nodiscard]] bool push_from_isr(const RuntimeInputEvent& event,
                                   BaseType_t* higher_priority_task_woken = nullptr);
  [[nodiscard]] bool pop(RuntimeInputEvent& event) override;
  [[nodiscard]] std::size_t pending() const override;
  [[nodiscard]] std::size_t capacity() const override { return capacity_; }

 private:
  QueueHandle_t queue_ = nullptr;
  std::size_t capacity_ = 0;
};

/** Wi-Fi station/AP adapter. It never calls esp_wifi_connect from start(). */
class IdfWifiStation {
 public:
  IdfWifiStation() = default;
  ~IdfWifiStation();

  [[nodiscard]] bool start();
  [[nodiscard]] bool connect(std::string_view ssid, std::string_view password);
  [[nodiscard]] bool connected() const { return connected_; }
  [[nodiscard]] bool start_provisioning_ap(std::string_view ssid);

 private:
  static void event_handler(void* arg, esp_event_base_t base, int32_t id, void* data);

  esp_netif_t* sta_netif_ = nullptr;
  esp_netif_t* ap_netif_ = nullptr;
  bool initialized_ = false;
  bool handlers_registered_ = false;
  bool connected_ = false;
};

/** Strictly bounded HTTP GET adapter for the dedicated snapshot route. */
class IdfHttpClient final : public HttpClient {
 public:
  struct Config {
    bool remote = false;
    const char* ca_pem = nullptr;
    std::size_t max_body_bytes = kMaxJsonBytes;
  };

  IdfHttpClient() = default;
  explicit IdfHttpClient(Config config) : config_(config) {}
  [[nodiscard]] HttpResponse get(std::string_view url,
                                 std::string_view bearer_token) override;

 private:
  Config config_;
};

/**
 * ESP WebSocket client adapter. Incoming frames are delivered to callbacks
 * supplied by the application task; no frame is logged or persisted.
 */
class IdfWebSocketClient final : public WebSocketClient {
 public:
  using TextCallback = std::function<void(std::string_view)>;
  using BinaryCallback = std::function<void(std::span<const std::uint8_t>)>;
  using ClosedCallback = std::function<void()>;

  struct Config {
    bool remote = false;
    const char* ca_pem = nullptr;
    std::size_t max_frame_bytes = kMaxJsonBytes;
  };

  IdfWebSocketClient() : text_assembler_(config_.max_frame_bytes) {}
  explicit IdfWebSocketClient(Config config)
      : config_(config), text_assembler_(config_.max_frame_bytes) {}
  ~IdfWebSocketClient() override;

  void set_callbacks(TextCallback text, BinaryCallback binary, ClosedCallback closed);
  [[nodiscard]] bool open(std::string_view url,
                          std::string_view bearer_token) override;
  [[nodiscard]] bool send_text(std::string_view frame) override;
  [[nodiscard]] bool send_binary(std::span<const std::uint8_t> frame) override;
  void close() override;

 private:
  static void event_handler(void* arg, esp_event_base_t base, int32_t event_id,
                            void* event_data);
  [[nodiscard]] bool allowed_route(std::string_view url) const;

  Config config_;
  WsTextMessageAssembler text_assembler_;
  esp_websocket_client_handle_t client_ = nullptr;
  std::string auth_header_;
  TextCallback text_callback_;
  BinaryCallback binary_callback_;
  ClosedCallback closed_callback_;
};

/**
 * Bounded device-local HTTP setup surface served only while the companion
 * SoftAP is advertising.  It accepts explicit form fields for remembered
 * Wi-Fi and gateway origin/security. Enrollment is completed through the
 * dedicated T3 Code pairing flow; raw companion tokens are not a portal
 * input. Secrets are never echoed or logged.
 */
class IdfProvisioningServer final {
 public:
  IdfProvisioningServer() = default;
  ~IdfProvisioningServer();

  void attach(RuntimeCoordinator& coordinator) { coordinator_ = &coordinator; }
  [[nodiscard]] bool start(std::string_view ap_ssid);
  void stop();
  [[nodiscard]] bool running() const { return server_ != nullptr; }
  [[nodiscard]] const std::string& ap_ssid() const { return ap_ssid_; }

 private:
  static esp_err_t root_handler(httpd_req_t* request);
  static esp_err_t status_handler(httpd_req_t* request);
  static esp_err_t submit_handler(httpd_req_t* request);
  static esp_err_t captive_probe_handler(httpd_req_t* request);

  esp_err_t serve_root(httpd_req_t* request);
  esp_err_t serve_status(httpd_req_t* request);
  esp_err_t submit(httpd_req_t* request);
  esp_err_t serve_captive_probe(httpd_req_t* request);

  RuntimeCoordinator* coordinator_ = nullptr;
  httpd_handle_t server_ = nullptr;
  std::string ap_ssid_;
};

/** Hostname pin/identity seam paired with the TLS-verified ESP clients. */
class IdfTlsIdentityVerifier final : public TlsIdentityVerifier {
 public:
  [[nodiscard]] bool verify(std::string_view host,
                            std::string_view peer_identity) override;

 private:
  std::string expected_host_;
};

/**
 * Reports runtime health gates to ESP-IDF's rollback/otadata boundary.  The
 * candidate is accepted only after board/display/touch/persistence are ready
 * and either a companion hello succeeds or the explicit offline grace window
 * expires.  No image or credential data is logged or persisted here.
 */
class IdfOtaHealth final : public RuntimeHealthSink {
 public:
  void mark_board_initialized() override;
  void mark_display_initialized() override;
  void mark_touch_initialized() override;
  void mark_persistence_mounted() override;
  void mark_handshake_succeeded() override;
  void mark_offline_health_timeout() override;

 private:
  void maybe_mark_valid();

  bool board_ = false;
  bool display_ = false;
  bool touch_ = false;
  bool persistence_ = false;
  bool handshake_ = false;
  bool offline_timeout_ = false;
  bool finalized_ = false;
};

/**
 * Verified Waveshare 1.75C board adapter. The BSP owns the touch IRQ driver;
 * service_inputs() converts its touch samples and AXP/GPIO button edges into
 * the bounded runtime queue consumed by RuntimeCoordinator::tick().
 */
class IdfBspPlatform final : public RuntimePlatform {
 public:
  IdfBspPlatform();
  ~IdfBspPlatform() override;

  [[nodiscard]] bool initialize() override;
  [[nodiscard]] std::string device_id() const override;
  [[nodiscard]] bool render(const t3::ui::UiModel& model) override;
  [[nodiscard]] bool set_display_power(bool on) override;
  [[nodiscard]] bool wifi_start() override;
  [[nodiscard]] bool wifi_connect(std::string_view ssid,
                                  std::string_view password) override;
  [[nodiscard]] bool wifi_connected() const override;
  [[nodiscard]] bool start_provisioning_ap(std::string_view ssid) override;
  [[nodiscard]] bool render_enrollment_pairing(std::string_view enrollment_id,
                                               std::string_view pairing_code,
                                               std::string_view expires_at) override;
  void attach_provisioning_handler(RuntimeCoordinator& coordinator) override;
  [[nodiscard]] bool audio_start(std::uint32_t sample_rate_hz) override;
  [[nodiscard]] int audio_read(std::span<std::int16_t> interleaved_samples) override;
  void audio_stop() override;
  void attach_input_queue(RuntimeInputQueue& queue) override;

  // Call once per application task iteration. It is deliberately separate
  // from RuntimeCoordinator so the ISR never performs I2C, LVGL, or allocation.
  void service_inputs();

 private:
  bool read_pkey(std::uint8_t& edges);
  bool read_touch(RuntimeInputEvent& event);
  bool init_pkey_device();
  bool configure_pkey_device();
  bool clear_pkey_status(std::uint8_t status_mask);
  void render_arcs(const t3::ui::UiModel& model);

  RuntimeInputQueue* input_queue_ = nullptr;
  IdfWifiStation wifi_;
  IdfProvisioningServer provisioning_server_;
  i2c_master_dev_handle_t axp_ = nullptr;
  lv_display_t* display_ = nullptr;
  lv_indev_t* input_ = nullptr;
  lv_obj_t* title_label_ = nullptr;
  lv_obj_t* state_label_ = nullptr;
  lv_obj_t* activity_label_ = nullptr;
  lv_obj_t* provider_label_ = nullptr;
  lv_obj_t* note_label_ = nullptr;
  lv_obj_t* rings_[3] = {nullptr, nullptr, nullptr};
  esp_codec_dev_handle_t microphone_ = nullptr;
  bool initialized_ = false;
  bool audio_open_ = false;
  bool audio_configured_ = false;
  bool touch_pressed_ = false;
  bool boot_pressed_ = false;
  std::uint64_t last_input_sample_ms_ = 0;
};

}  // namespace t3::companion::runtime
