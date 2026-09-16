#include "idf_adapters.hpp"

#include "axp_pkey.hpp"

#include "bsp/esp32_s3_touch_amoled_1_75c.h"

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <string_view>
#include <string>
#include <utility>
#include <vector>

namespace t3::companion::runtime {
namespace {

constexpr char kTag[] = "t3_companion";
constexpr std::uint8_t kAxpAddress = 0x34;
constexpr std::uint8_t kAxpEnableRegister = 0x41;
constexpr std::uint8_t kAxpStatusRegister = 0x49;
constexpr std::uint8_t kAxpPkeyEnableMask = 0x0F;
constexpr std::size_t kWifiSsidMax = 32;
constexpr std::size_t kWifiPasswordMax = 64;
constexpr std::size_t kProvisioningBodyMax = 2'048;

constexpr char kProvisioningHtml[] = R"HTML(<!doctype html>
<html lang="en"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Pocket-Code local setup</title>
<style>body{font:16px system-ui,sans-serif;max-width:34rem;margin:2rem auto;padding:0 1rem;background:#05070a;color:#f4f6f8}label{display:block;margin:1rem 0 .25rem;color:#94a0b1}input,select,button{box-sizing:border-box;width:100%;padding:.7rem;border-radius:.4rem;border:1px solid #394452;background:#111820;color:#f4f6f8}button{margin-top:1.5rem;background:#2d7ff9;border:0;font-weight:600}small{color:#94a0b1}code{color:#58a6ff}</style>
<h1>Pocket-Code</h1><p>Local setup for <code>{{SSID}}</code> (open SoftAP).</p>
<form action="/api/companion/v1/provisioning" method="post">
<label for="ssid">Wi-Fi network (SSID)</label><input id="ssid" name="ssid" maxlength="32" required autocomplete="off">
<label for="password">Wi-Fi password</label><input id="password" name="password" type="password" maxlength="64" autocomplete="off">
<label for="gateway_url">Companion gateway origin (optional)</label><input id="gateway_url" name="gateway_url" placeholder="http://gateway.local" maxlength="256" autocomplete="off"><small>Leave blank to save Wi-Fi only.</small>
<label for="transport">Gateway transport</label><select id="transport" name="transport"><option value="lan">Trusted LAN (HTTP/WS)</option><option value="remote">Remote (HTTPS/WSS)</option></select>
<button type="submit">Save local setup</button></form>
<p><small>Usage comes from CodexBar on your Mac. Saving Wi-Fi preserves existing pairing. The CodexBar adapter currently requires an already-enrolled device; see the Pocket-Code setup guide.</small></p>
<p><a href="/api/companion/v1/provisioning" style="color:#58a6ff">View sanitized status</a></p>
)HTML";

bool starts_with(std::string_view value, std::string_view prefix) {
  return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

bool is_https(std::string_view value) { return starts_with(value, "https://"); }
bool is_http(std::string_view value) { return starts_with(value, "http://"); }
bool is_wss(std::string_view value) { return starts_with(value, "wss://"); }
bool is_ws(std::string_view value) { return starts_with(value, "ws://"); }

int hex_digit(char value) {
  if (value >= '0' && value <= '9') return value - '0';
  if (value >= 'a' && value <= 'f') return value - 'a' + 10;
  if (value >= 'A' && value <= 'F') return value - 'A' + 10;
  return -1;
}

bool decode_form_component(std::string_view encoded, std::size_t max_bytes,
                           std::string& decoded) {
  decoded.clear();
  decoded.reserve(std::min(encoded.size(), max_bytes));
  for (std::size_t index = 0; index < encoded.size(); ++index) {
    char value = encoded[index];
    if (value == '%') {
      if (index + 2U >= encoded.size()) return false;
      const int high = hex_digit(encoded[index + 1U]);
      const int low = hex_digit(encoded[index + 2U]);
      if (high < 0 || low < 0) return false;
      value = static_cast<char>((high << 4) | low);
      index += 2U;
    } else if (value == '+') {
      value = ' ';
    }
    if (decoded.size() >= max_bytes) return false;
    decoded.push_back(value);
  }
  return true;
}

struct ProvisioningForm {
  std::string ssid;
  std::string password;
  std::string gateway_url;
  std::string transport;
  bool has_ssid = false;
  bool has_password = false;
  bool has_gateway_url = false;
  bool has_transport = false;
};

bool parse_provisioning_form(std::string_view body, ProvisioningForm& form) {
  std::size_t offset = 0;
  while (offset <= body.size()) {
    const auto separator = body.find('&', offset);
    const auto field = body.substr(offset, separator == std::string_view::npos
                                               ? body.size() - offset
                                               : separator - offset);
    const auto equals = field.find('=');
    const auto encoded_key = field.substr(0, equals);
    const auto encoded_value = equals == std::string_view::npos
                                   ? std::string_view{}
                                   : field.substr(equals + 1U);
    std::string key;
    if (!decode_form_component(encoded_key, 32U, key)) return false;
    std::size_t max_value = 512U;
    if (key == "ssid") max_value = kWifiSsidMax;
    else if (key == "password") max_value = kWifiPasswordMax;
    else if (key == "gateway_url") max_value = kMaxGatewayUrlBytes;
    else if (key == "transport") max_value = 8U;
    else if (key != "ssid" && key != "password" && key != "gateway_url" &&
             key != "transport") return false;
    std::string value;
    if (!decode_form_component(encoded_value, max_value, value)) return false;
    if (key == "ssid") {
      form.ssid = std::move(value);
      form.has_ssid = true;
    } else if (key == "password") {
      form.password = std::move(value);
      form.has_password = true;
    } else if (key == "gateway_url") {
      form.gateway_url = std::move(value);
      form.has_gateway_url = true;
    } else if (key == "transport") {
      form.transport = std::move(value);
      form.has_transport = true;
    }
    if (separator == std::string_view::npos) break;
    offset = separator + 1U;
  }
  return true;
}

const char* runtime_error_name(RuntimeErrorCode code) {
  switch (code) {
    case RuntimeErrorCode::None: return "none";
    case RuntimeErrorCode::AlreadyBooted: return "already_booted";
    case RuntimeErrorCode::BoardInitialization: return "board_initialization";
    case RuntimeErrorCode::Persistence: return "persistence";
    case RuntimeErrorCode::Provisioning: return "provisioning";
    case RuntimeErrorCode::Configuration: return "configuration";
    case RuntimeErrorCode::Network: return "network";
    case RuntimeErrorCode::Protocol: return "protocol";
    case RuntimeErrorCode::Display: return "display";
    case RuntimeErrorCode::InputQueue: return "input_queue";
  }
  return "unknown";
}

bool receive_body(httpd_req_t* request, std::string& body) {
  if (request == nullptr || request->content_len == 0U ||
      request->content_len > kProvisioningBodyMax) {
    return false;
  }
  body.assign(request->content_len, '\0');
  std::size_t received = 0;
  while (received < request->content_len) {
    const int result = httpd_req_recv(request, body.data() + received,
                                      request->content_len - received);
    if (result <= 0) return false;
    received += static_cast<std::size_t>(result);
  }
  return true;
}

esp_err_t respond(httpd_req_t* request, const char* status, const char* type,
                  std::string_view body) {
  if (request == nullptr) return ESP_FAIL;
  (void)httpd_resp_set_status(request, status);
  (void)httpd_resp_set_type(request, type);
  return httpd_resp_send(request, body.data(), static_cast<ssize_t>(body.size()));
}

esp_err_t respond_json_error(httpd_req_t* request, const char* code, int status_code) {
  const char* status = status_code == 413 ? "413 Payload Too Large" :
                       status_code == 405 ? "405 Method Not Allowed" :
                       "400 Bad Request";
  std::string body = "{\"ok\":false,\"error\":\"";
  body += code;
  body += "\"}";
  return respond(request, status, "application/json", body);
}

}  // namespace

std::uint64_t IdfClock::now_ms() const {
  return static_cast<std::uint64_t>(esp_timer_get_time() / 1'000);
}

void IdfClock::sleep_ms(std::uint32_t delay_ms) {
  vTaskDelay(pdMS_TO_TICKS(delay_ms));
}

IdfNvsSlotStorage::IdfNvsSlotStorage(std::string_view name_space)
    : namespace_(name_space) {}

IdfNvsSlotStorage::~IdfNvsSlotStorage() {
  if (handle_ != 0) nvs_close(handle_);
}

bool IdfNvsSlotStorage::begin() {
  if (handle_ != 0 || namespace_.empty() || namespace_.size() > 15U) return handle_ != 0;
  const auto result = nvs_open(namespace_.c_str(), NVS_READWRITE, &handle_);
  if (result != ESP_OK) {
    handle_ = 0;
    return false;
  }
  return true;
}

const char* IdfNvsSlotStorage::key_for(std::size_t slot) const {
  return slot == 0U ? "slot0" : (slot == 1U ? "slot1" : "");
}

std::optional<std::vector<std::uint8_t>> IdfNvsSlotStorage::read_slot(
    std::size_t slot) const {
  if (handle_ == 0 || slot > 1U) return std::nullopt;
  std::size_t size = 0;
  const auto key = key_for(slot);
  const auto result = nvs_get_blob(handle_, key, nullptr, &size);
  if (result == ESP_ERR_NVS_NOT_FOUND) return std::nullopt;
  if (result != ESP_OK || size == 0U) return std::nullopt;
  std::vector<std::uint8_t> bytes(size);
  if (nvs_get_blob(handle_, key, bytes.data(), &size) != ESP_OK || size != bytes.size()) {
    return std::nullopt;
  }
  return bytes;
}

bool IdfNvsSlotStorage::write_slot(std::size_t slot,
                                   std::span<const std::uint8_t> bytes) {
  if (handle_ == 0 || slot > 1U || bytes.empty()) return false;
  const auto key = key_for(slot);
  if (nvs_set_blob(handle_, key, bytes.data(), bytes.size()) != ESP_OK) return false;
  return nvs_commit(handle_) == ESP_OK;
}

IdfRuntimeQueue::IdfRuntimeQueue(std::size_t capacity)
    : capacity_(capacity == 0U ? FixedRuntimeInputQueue::kCapacity : capacity) {
  queue_ = xQueueCreate(static_cast<UBaseType_t>(capacity_), sizeof(RuntimeInputEvent));
}

IdfRuntimeQueue::~IdfRuntimeQueue() {
  if (queue_ != nullptr) vQueueDelete(queue_);
}

bool IdfRuntimeQueue::push(const RuntimeInputEvent& event) {
  return queue_ != nullptr && xQueueSend(queue_, &event, 0) == pdTRUE;
}

bool IdfRuntimeQueue::push_from_isr(const RuntimeInputEvent& event,
                                    BaseType_t* higher_priority_task_woken) {
  return queue_ != nullptr && xQueueSendFromISR(queue_, &event, higher_priority_task_woken) == pdTRUE;
}

bool IdfRuntimeQueue::pop(RuntimeInputEvent& event) {
  return queue_ != nullptr && xQueueReceive(queue_, &event, 0) == pdTRUE;
}

std::size_t IdfRuntimeQueue::pending() const {
  return queue_ == nullptr ? 0U : static_cast<std::size_t>(uxQueueMessagesWaiting(queue_));
}

IdfWifiStation::~IdfWifiStation() {
  if (initialized_) {
    (void)esp_wifi_stop();
    (void)esp_wifi_deinit();
  }
  if (handlers_registered_) {
    (void)esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, &IdfWifiStation::event_handler);
    (void)esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, &IdfWifiStation::event_handler);
  }
}

void IdfWifiStation::event_handler(void* arg, esp_event_base_t base, int32_t id, void* data) {
  auto* self = static_cast<IdfWifiStation*>(arg);
  if (self == nullptr) return;
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
    self->connected_ = false;
  } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    (void)data;
    self->connected_ = true;
  }
}

bool IdfWifiStation::start() {
  if (initialized_) return true;
  const auto netif_result = esp_netif_init();
  if (netif_result != ESP_OK && netif_result != ESP_ERR_INVALID_STATE) return false;
  const auto loop_result = esp_event_loop_create_default();
  if (loop_result != ESP_OK && loop_result != ESP_ERR_INVALID_STATE) return false;
  sta_netif_ = esp_netif_create_default_wifi_sta();
  if (sta_netif_ == nullptr) return false;
  wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
  if (esp_wifi_init(&config) != ESP_OK) return false;
  if (esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &IdfWifiStation::event_handler,
                                 this) != ESP_OK ||
      esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &IdfWifiStation::event_handler,
                                 this) != ESP_OK) {
    (void)esp_wifi_deinit();
    return false;
  }
  handlers_registered_ = true;
  if (esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK || esp_wifi_start() != ESP_OK) return false;
  initialized_ = true;
  return true;
}

bool IdfWifiStation::connect(std::string_view ssid, std::string_view password) {
  if (!initialized_ || ssid.empty() || ssid.size() > kWifiSsidMax ||
      password.size() > kWifiPasswordMax) {
    return false;
  }
  wifi_config_t config{};
  std::memcpy(config.sta.ssid, ssid.data(), ssid.size());
  std::memcpy(config.sta.password, password.data(), password.size());
  config.sta.threshold.authmode = password.empty() ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
  connected_ = false;
  return esp_wifi_set_mode(WIFI_MODE_STA) == ESP_OK &&
         esp_wifi_set_config(WIFI_IF_STA, &config) == ESP_OK && esp_wifi_connect() == ESP_OK;
}

bool IdfWifiStation::start_provisioning_ap(std::string_view ssid) {
  if (!initialized_ || ssid.empty() || ssid.size() > kWifiSsidMax) return false;
  if (ap_netif_ == nullptr) ap_netif_ = esp_netif_create_default_wifi_ap();
  if (ap_netif_ == nullptr) return false;
  wifi_config_t config{};
  std::memcpy(config.ap.ssid, ssid.data(), ssid.size());
  config.ap.ssid_len = static_cast<std::uint8_t>(ssid.size());
  config.ap.max_connection = 1;
  config.ap.authmode = WIFI_AUTH_OPEN;
  if (esp_wifi_set_mode(WIFI_MODE_APSTA) != ESP_OK ||
      esp_wifi_set_config(WIFI_IF_AP, &config) != ESP_OK) {
    return false;
  }
  const auto result = esp_wifi_start();
  return result == ESP_OK || result == ESP_ERR_INVALID_STATE;
}

HttpResponse IdfHttpClient::get(std::string_view url, std::string_view bearer_token) {
  HttpResponse response;
  if ((config_.remote && !is_https(url)) || (!config_.remote && !is_http(url)) ||
      url.empty() || url.size() > kMaxGatewayUrlBytes) {
    return response;
  }
  if (config_.remote && config_.ca_pem == nullptr) {
    // A remote session must use either an explicitly supplied CA or the IDF
    // certificate bundle; builds without the bundle fail closed below.
#if !CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
    return response;
#endif
  }
  std::string url_copy(url);
  esp_http_client_config_t client_config{};
  client_config.url = url_copy.c_str();
  client_config.method = HTTP_METHOD_GET;
  client_config.timeout_ms = 5'000;
  client_config.disable_auto_redirect = true;
  client_config.keep_alive_enable = true;
  if (config_.ca_pem != nullptr) {
    client_config.cert_pem = config_.ca_pem;
  } else if (config_.remote) {
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
    client_config.crt_bundle_attach = esp_crt_bundle_attach;
#endif
  }
  auto client = esp_http_client_init(&client_config);
  if (client == nullptr) return response;
  std::string authorization;
  if (!bearer_token.empty()) {
    authorization = "Bearer ";
    authorization.append(bearer_token);
    (void)esp_http_client_set_header(client, "Authorization", authorization.c_str());
  }
  const auto result = esp_http_client_perform(client);
  if (result == ESP_OK) {
    response.status = esp_http_client_get_status_code(client);
    const auto length = esp_http_client_get_content_length(client);
    if (length >= 0 && static_cast<std::size_t>(length) <= config_.max_body_bytes) {
      std::vector<char> body(static_cast<std::size_t>(length) + 1U, '\0');
      const int read = esp_http_client_read_response(client, body.data(), static_cast<int>(length));
      if (read >= 0 && static_cast<std::size_t>(read) <= config_.max_body_bytes) {
        response.body.assign(body.data(), static_cast<std::size_t>(read));
      } else {
        response.status = 413;
      }
    } else if (length > static_cast<long long>(config_.max_body_bytes)) {
      response.status = 413;
    }
  }
  (void)esp_http_client_cleanup(client);
  return response;
}

IdfWebSocketClient::~IdfWebSocketClient() { close(); }

void IdfWebSocketClient::set_callbacks(TextCallback text, BinaryCallback binary,
                                       ClosedCallback closed) {
  text_callback_ = std::move(text);
  binary_callback_ = std::move(binary);
  closed_callback_ = std::move(closed);
}

bool IdfWebSocketClient::allowed_route(std::string_view url) const {
  const bool route = url.find("/api/companion/v1/ws") != std::string_view::npos ||
                     url.find("/api/companion/v1/enroll") != std::string_view::npos;
  const bool scheme = config_.remote ? is_wss(url) : is_ws(url);
  return route && scheme;
}

bool IdfWebSocketClient::open(std::string_view url, std::string_view bearer_token) {
  if (!allowed_route(url)) return false;
  close();
  std::string url_copy(url);
  auth_header_.clear();
  esp_websocket_client_config_t config{};
  config.uri = url_copy.c_str();
  config.disable_auto_reconnect = true;
  config.network_timeout_ms = 5'000;
  // Enrollment callbacks parse bounded JSON and update the LVGL surface on
  // the websocket task. The component default is 4 KiB, which overflows on
  // the verified board as soon as the pairing offer is rendered.
  // Hello acknowledgement decoding is deeper than the enrollment offer
  // parser (bounded nested capability/status objects); 8 KiB still overflowed
  // the websocket task on the verified board, so reserve 16 KiB.
  config.task_stack = 16 * 1024;
  if (!bearer_token.empty()) {
    auth_header_ = "Authorization: Bearer ";
    auth_header_.append(bearer_token);
    // esp_transport_ws appends one final CRLF after the configured headers;
    // terminate this custom header explicitly so that the final CRLF becomes
    // the required blank line ending the HTTP upgrade request.
    auth_header_.append("\r\n");
    config.headers = auth_header_.c_str();
  }
  if (config_.ca_pem != nullptr) {
    config.cert_pem = config_.ca_pem;
  } else if (config_.remote) {
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
    config.crt_bundle_attach = esp_crt_bundle_attach;
#else
    return false;
#endif
  }
  client_ = esp_websocket_client_init(&config);
  if (client_ == nullptr) return false;
  if (esp_websocket_register_events(client_, WEBSOCKET_EVENT_ANY, &IdfWebSocketClient::event_handler,
                                     this) != ESP_OK) {
    close();
    return false;
  }
  return esp_websocket_client_start(client_) == ESP_OK;
}

bool IdfWebSocketClient::send_text(std::string_view frame) {
  if (client_ == nullptr || frame.empty() || frame.size() > config_.max_frame_bytes) return false;
  return esp_websocket_client_send_text(client_, frame.data(), static_cast<int>(frame.size()),
                                        5'000) >= 0;
}

bool IdfWebSocketClient::send_binary(std::span<const std::uint8_t> frame) {
  if (client_ == nullptr || frame.empty() || frame.size() > config_.max_frame_bytes) return false;
  return esp_websocket_client_send_bin(client_, reinterpret_cast<const char*>(frame.data()),
                                       static_cast<int>(frame.size()), 5'000) >= 0;
}

void IdfWebSocketClient::close() {
  text_assembler_.reset();
  if (client_ == nullptr) return;
  (void)esp_websocket_client_close(client_, 5'000);
  (void)esp_websocket_client_destroy(client_);
  client_ = nullptr;
}

void IdfWebSocketClient::event_handler(void* arg, esp_event_base_t /*base*/, int32_t event_id,
                                       void* event_data) {
  auto* self = static_cast<IdfWebSocketClient*>(arg);
  auto* event = static_cast<esp_websocket_event_data_t*>(event_data);
  if (self == nullptr || event == nullptr) return;
  if (event_id == WEBSOCKET_EVENT_CONNECTED) {
    self->text_assembler_.reset();
    return;
  }
  if (event_id == WEBSOCKET_EVENT_DATA) {
    if (event->data_len < 0) {
      self->text_assembler_.reset();
      return;
    }
    if (event->op_code == WsTextMessageAssembler::kTextOpcode ||
        event->op_code == WsTextMessageAssembler::kContinuationOpcode) {
      if (event->data_ptr == nullptr && event->data_len != 0) {
        self->text_assembler_.reset();
        return;
      }
      const auto chunk = std::string_view(
          event->data_ptr == nullptr ? "" : event->data_ptr,
          static_cast<std::size_t>(event->data_len));
      const auto complete = self->text_assembler_.accept(
          chunk, event->payload_len, event->payload_offset, event->op_code,
          event->fin);
      if (complete.has_value() && self->text_callback_) {
        self->text_callback_(*complete);
      }
    } else if (event->op_code == 0x2U && event->data_ptr != nullptr &&
               static_cast<std::size_t>(event->data_len) <=
                   self->config_.max_frame_bytes && self->binary_callback_) {
      self->text_assembler_.reset();
      self->binary_callback_(std::span<const std::uint8_t>(
          reinterpret_cast<const std::uint8_t*>(event->data_ptr),
          static_cast<std::size_t>(event->data_len)));
    } else if ((event->op_code & 0x8U) == 0U) {
      self->text_assembler_.reset();
    }
  } else if (event_id == WEBSOCKET_EVENT_DISCONNECTED ||
             event_id == WEBSOCKET_EVENT_CLOSED) {
    // A clean server shutdown emits CLOSED, not DISCONNECTED. Both must
    // clear the live state and let the coordinator reconnect.
    self->text_assembler_.reset();
    if (self->closed_callback_) self->closed_callback_();
  }
}

IdfProvisioningServer::~IdfProvisioningServer() { stop(); }

esp_err_t IdfProvisioningServer::root_handler(httpd_req_t* request) {
  auto* self = request == nullptr ? nullptr
                                  : static_cast<IdfProvisioningServer*>(request->user_ctx);
  return self == nullptr ? ESP_FAIL : self->serve_root(request);
}

esp_err_t IdfProvisioningServer::status_handler(httpd_req_t* request) {
  auto* self = request == nullptr ? nullptr
                                  : static_cast<IdfProvisioningServer*>(request->user_ctx);
  return self == nullptr ? ESP_FAIL : self->serve_status(request);
}

esp_err_t IdfProvisioningServer::submit_handler(httpd_req_t* request) {
  auto* self = request == nullptr ? nullptr
                                  : static_cast<IdfProvisioningServer*>(request->user_ctx);
  return self == nullptr ? ESP_FAIL : self->submit(request);
}

esp_err_t IdfProvisioningServer::captive_probe_handler(httpd_req_t* request) {
  auto* self = request == nullptr ? nullptr
                                  : static_cast<IdfProvisioningServer*>(request->user_ctx);
  return self == nullptr ? ESP_FAIL : self->serve_captive_probe(request);
}

esp_err_t IdfProvisioningServer::serve_root(httpd_req_t* request) {
  std::string html(kProvisioningHtml);
  const auto marker = html.find("{{SSID}}");
  if (marker != std::string::npos) html.replace(marker, 8U, ap_ssid_);
  return respond(request, "200 OK", "text/html; charset=utf-8", html);
}

esp_err_t IdfProvisioningServer::serve_status(httpd_req_t* request) {
  if (coordinator_ == nullptr) return respond_json_error(request, "not_ready", 400);
  return respond(request, "200 OK", "application/json", coordinator_->provisioning_status_json());
}

esp_err_t IdfProvisioningServer::serve_captive_probe(httpd_req_t* request) {
  // Returning HTML rather than a successful connectivity probe causes phone
  // captive-network assistants to offer the local setup page.
  return serve_root(request);
}

esp_err_t IdfProvisioningServer::submit(httpd_req_t* request) {
  if (coordinator_ == nullptr) return respond_json_error(request, "not_ready", 400);
  if (request == nullptr || request->content_len > kProvisioningBodyMax) {
    return respond_json_error(request, "body_too_large", 413);
  }
  std::string body;
  if (!receive_body(request, body)) return respond_json_error(request, "invalid_form", 400);
  ProvisioningForm form;
  if (!parse_provisioning_form(body, form)) {
    return respond_json_error(request, "invalid_form", 400);
  }
  if (form.has_password && !form.has_ssid) {
    return respond_json_error(request, "ssid_required", 400);
  }
  const bool gateway_present = form.has_gateway_url && !form.gateway_url.empty();
  if (gateway_present && form.has_transport && form.transport != "lan" &&
      form.transport != "remote") {
    return respond_json_error(request, "invalid_transport", 400);
  }

  if (!gateway_present && !form.has_ssid) {
    return respond_json_error(request, "no_supported_fields", 400);
  }
  // Apply durable mutations in explicit form order. An empty optional gateway
  // is deliberately passed through as empty so a browser's always-submitted
  // transport selector cannot turn a Wi-Fi-only setup into an error.
  const auto security = form.transport == "remote" ? TransportSecurity::Remote
                                                     : TransportSecurity::TrustedLan;
  const auto result = coordinator_->submit_provisioning(
      form.ssid, form.password, gateway_present ? std::string_view(form.gateway_url)
                                                : std::string_view{},
      security);
  if (!result.ok()) return respond_json_error(request, runtime_error_name(result.error.code), 400);
  return respond(request, "200 OK", "application/json",
                 "{\"ok\":true,\"saved\":true,\"status\":\"/api/companion/v1/provisioning\"}");
}

bool IdfProvisioningServer::start(std::string_view ap_ssid) {
  if (server_ != nullptr) return true;
  if (coordinator_ == nullptr || ap_ssid.empty() || ap_ssid.size() > kWifiSsidMax) return false;
  ap_ssid_.assign(ap_ssid);
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;
  config.max_uri_handlers = 8;
  config.stack_size = 6'144;
  if (httpd_start(&server_, &config) != ESP_OK) {
    server_ = nullptr;
    return false;
  }

  httpd_uri_t root{};
  root.uri = "/";
  root.method = HTTP_GET;
  root.handler = &IdfProvisioningServer::root_handler;
  root.user_ctx = this;
  httpd_uri_t status{};
  status.uri = "/api/companion/v1/provisioning";
  status.method = HTTP_GET;
  status.handler = &IdfProvisioningServer::status_handler;
  status.user_ctx = this;
  httpd_uri_t submit_route{};
  submit_route.uri = "/api/companion/v1/provisioning";
  submit_route.method = HTTP_POST;
  submit_route.handler = &IdfProvisioningServer::submit_handler;
  submit_route.user_ctx = this;
  httpd_uri_t generate204{};
  generate204.uri = "/generate_204";
  generate204.method = HTTP_GET;
  generate204.handler = &IdfProvisioningServer::captive_probe_handler;
  generate204.user_ctx = this;
  httpd_uri_t hotspot{};
  hotspot.uri = "/hotspot-detect.html";
  hotspot.method = HTTP_GET;
  hotspot.handler = &IdfProvisioningServer::captive_probe_handler;
  hotspot.user_ctx = this;
  httpd_uri_t connecttest{};
  connecttest.uri = "/connecttest.txt";
  connecttest.method = HTTP_GET;
  connecttest.handler = &IdfProvisioningServer::captive_probe_handler;
  connecttest.user_ctx = this;
  httpd_uri_t ncsi{};
  ncsi.uri = "/ncsi.txt";
  ncsi.method = HTTP_GET;
  ncsi.handler = &IdfProvisioningServer::captive_probe_handler;
  ncsi.user_ctx = this;
  const httpd_uri_t* routes[] = {&root, &status, &submit_route, &generate204,
                                 &hotspot, &connecttest, &ncsi};
  for (const auto* route : routes) {
    if (httpd_register_uri_handler(server_, route) != ESP_OK) {
      stop();
      return false;
    }
  }
  ESP_LOGI(kTag, "provisioning HTTP server ready at 192.168.4.1 (AP SSID %s, open)",
           ap_ssid_.c_str());
  return true;
}

void IdfProvisioningServer::stop() {
  if (server_ != nullptr) {
    (void)httpd_stop(server_);
    server_ = nullptr;
  }
  ap_ssid_.clear();
}

bool IdfTlsIdentityVerifier::verify(std::string_view host,
                                    std::string_view peer_identity) {
  if (host.empty()) return false;
  if (!peer_identity.empty() && peer_identity != host) return false;
  if (expected_host_.empty()) {
    expected_host_.assign(host);
    return true;
  }
  return expected_host_ == host;
}

void IdfOtaHealth::mark_board_initialized() {
  board_ = true;
  maybe_mark_valid();
}

void IdfOtaHealth::mark_display_initialized() {
  display_ = true;
  maybe_mark_valid();
}

void IdfOtaHealth::mark_touch_initialized() {
  touch_ = true;
  maybe_mark_valid();
}

void IdfOtaHealth::mark_persistence_mounted() {
  persistence_ = true;
  maybe_mark_valid();
}

void IdfOtaHealth::mark_handshake_succeeded() {
  handshake_ = true;
  maybe_mark_valid();
}

void IdfOtaHealth::mark_offline_health_timeout() {
  offline_timeout_ = true;
  maybe_mark_valid();
}

void IdfOtaHealth::maybe_mark_valid() {
  if (finalized_ || !board_ || !display_ || !touch_ || !persistence_ ||
      (!handshake_ && !offline_timeout_)) {
    return;
  }
  const auto result = esp_ota_mark_app_valid_cancel_rollback();
  // When the running image is not a rollback candidate IDF reports an invalid
  // state.  That is already a healthy boot from the bootloader's perspective,
  // so avoid retrying or turning a normal boot into a fault.
  if (result == ESP_OK || result == ESP_ERR_OTA_ROLLBACK_INVALID_STATE) {
    finalized_ = true;
  }
}

IdfBspPlatform::IdfBspPlatform() = default;

IdfBspPlatform::~IdfBspPlatform() {
  provisioning_server_.stop();
  audio_stop();
  if (axp_ != nullptr) {
    (void)i2c_master_bus_rm_device(axp_);
    axp_ = nullptr;
  }
}

bool IdfBspPlatform::init_pkey_device() {
  const auto bus = bsp_i2c_get_handle();
  if (bus == nullptr) return false;
  i2c_device_config_t config{};
  config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
  config.device_address = kAxpAddress;
  config.scl_speed_hz = 400'000;
  const auto rc = i2c_master_bus_add_device(bus, &config, &axp_);
  return rc == ESP_OK;
}

bool IdfBspPlatform::clear_pkey_status(std::uint8_t status_mask) {
  if (axp_ == nullptr || status_mask == 0U) return false;
  const std::uint8_t payload[2] = {kAxpStatusRegister, status_mask};
  return i2c_master_transmit(axp_, payload, sizeof(payload), 20) == ESP_OK;
}

bool IdfBspPlatform::configure_pkey_device() {
  if (axp_ == nullptr) return false;
  std::uint8_t reg = kAxpEnableRegister;
  std::uint8_t enabled = 0;
  const auto read_rc = i2c_master_transmit_receive(axp_, &reg, 1, &enabled, 1, 20);
  if (read_rc != ESP_OK) return false;
  enabled = static_cast<std::uint8_t>(enabled | kAxpPkeyEnableMask);
  const std::uint8_t payload[2] = {kAxpEnableRegister, enabled};
  const auto write_rc = i2c_master_transmit(axp_, payload, sizeof(payload), 20);
  return write_rc == ESP_OK;
}

bool IdfBspPlatform::initialize() {
  if (initialized_) return true;
  if (bsp_i2c_init() != ESP_OK) return false;
  bsp_display_cfg_t display_config{};
  // The BSP's zero-initialized display config is not a valid LVGL adapter
  // configuration: it would create a worker with a zero delay and starve the
  // idle task.  Start from the official adapter defaults, then override only
  // the verified panel/touch settings below.
  display_config.lv_adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG();
  display_config.rotation = ESP_LV_ADAPTER_ROTATE_0;
  display_config.touch_flags.swap_xy = 0;
  display_config.touch_flags.mirror_x = 1;
  display_config.touch_flags.mirror_y = 1;
  display_config.tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_DEFAULT;
  display_ = bsp_display_start_with_config(&display_config);
  if (display_ == nullptr) return false;
  input_ = bsp_display_get_input_dev();
  (void)bsp_display_brightness_set(70);
  if (init_pkey_device()) {
    const bool pkey_configured = configure_pkey_device();
    // INTSTS2 is write-one-to-clear. Discard any boot/stale edge before the
    // application input loop so the first real PKEY transition is observable.
    if (pkey_configured) (void)clear_pkey_status(kAxpPkeyStatusMask);
  }

  // The verified 1.75C capture path is the public BSP STD duplex interface:
  // MCLK16/BCLK9/WS45/DOUT8/DIN10, 24 kHz, 16-bit stereo (channels 0/1).
  // Passing NULL here would select the BSP's 22050 Hz mono default and would
  // violate the board manifest even though the codec constructor succeeds.
  i2s_std_config_t audio_config = {
      .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(kPcmCaptureSampleRateHz),
      .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                       I2S_SLOT_MODE_STEREO),
      .gpio_cfg = {
          .mclk = GPIO_NUM_16,
          .bclk = BSP_I2S_SCLK,
          .ws = BSP_I2S_LCLK,
          .dout = BSP_I2S_DOUT,
          .din = BSP_I2S_DSIN,
          .invert_flags = {
              .mclk_inv = false,
              .bclk_inv = false,
              .ws_inv = false,
          },
      },
  };
  audio_config.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
  audio_configured_ = bsp_audio_init(&audio_config) == ESP_OK;
  if (!audio_configured_) {
    ESP_LOGW(kTag, "verified microphone audio remains unavailable");
  }
  if (bsp_display_lock(1000) != ESP_OK) return false;
  auto* screen = lv_screen_active();
  // Match the approved ambient renderer's dark first frame.  The BSP leaves
  // LVGL's default white screen active, which is an unsafe/blank-looking
  // provisioning state until the first model render arrives.
  lv_obj_set_style_bg_color(screen, lv_color_hex(0x05070A), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
  title_label_ = lv_label_create(lv_screen_active());
  state_label_ = lv_label_create(lv_screen_active());
  activity_label_ = lv_label_create(lv_screen_active());
  provider_label_ = lv_label_create(lv_screen_active());
  note_label_ = lv_label_create(lv_screen_active());
  lv_obj_set_style_text_color(title_label_, lv_color_hex(0x778598), LV_PART_MAIN);
  lv_obj_set_style_text_color(state_label_, lv_color_hex(0xF4F6F8), LV_PART_MAIN);
  lv_obj_set_style_text_color(activity_label_, lv_color_hex(0x94A0B1), LV_PART_MAIN);
  lv_obj_set_style_text_color(provider_label_, lv_color_hex(0x94A0B1), LV_PART_MAIN);
  lv_obj_set_style_text_color(note_label_, lv_color_hex(0x94A0B1), LV_PART_MAIN);
  lv_obj_align(title_label_, LV_ALIGN_CENTER, 0, -72);
  lv_obj_align(state_label_, LV_ALIGN_CENTER, 0, -32);
  lv_obj_align(activity_label_, LV_ALIGN_CENTER, 0, 8);
  lv_obj_align(provider_label_, LV_ALIGN_CENTER, 0, 48);
  lv_obj_align(note_label_, LV_ALIGN_CENTER, 0, 86);
  for (int index = 0; index < 3; ++index) {
    rings_[index] = lv_arc_create(lv_screen_active());
    lv_obj_set_size(rings_[index], 410 - index * 36, 410 - index * 36);
    lv_obj_center(rings_[index]);
    lv_arc_set_bg_angles(rings_[index], 0, 360);
    lv_arc_set_rotation(rings_[index], 270);
    lv_obj_set_style_arc_color(rings_[index], lv_color_hex(0x1B2430), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(rings_[index], LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_arc_color(rings_[index],
                               lv_color_hex(index == 0 ? 0x58A6FF
                                                        : (index == 1 ? 0xE88962 : 0xF4F6F8)),
                               LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(rings_[index], LV_OPA_COVER, LV_PART_INDICATOR);
  }
  bsp_display_unlock();
  initialized_ = true;
  return true;
}

std::string IdfBspPlatform::device_id() const {
  std::array<std::uint8_t, 6> mac{};
  esp_read_mac(mac.data(), ESP_MAC_WIFI_STA);
  char value[32]{};
  std::snprintf(value, sizeof(value), "t3-%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2],
                mac[3], mac[4], mac[5]);
  return value;
}

void IdfBspPlatform::render_arcs(const t3::ui::UiModel& model) {
  const std::array<double, 3> values = {
      model.rings.codex.weekly.available && !model.rings.codex.weekly.stale
          ? model.rings.codex.weekly.percent
          : 0.0,
      model.rings.claude.weekly.available && !model.rings.claude.weekly.stale
          ? model.rings.claude.weekly.percent
          : 0.0,
      model.rings.xai.weekly.available && !model.rings.xai.weekly.stale
          ? model.rings.xai.weekly.percent
          : 0.0};
  for (std::size_t index = 0; index < values.size(); ++index) {
    if (rings_[index] != nullptr) lv_arc_set_value(rings_[index], static_cast<int>(values[index]));
  }
}

bool IdfBspPlatform::render(const t3::ui::UiModel& model) {
  if (!initialized_ || bsp_display_lock(100) != ESP_OK) return false;
  std::string title;
  std::string state = model.center.state;
  std::string activity = model.center.activity;
  std::string providers = model.provider_row.claude.glyph + " " +
                           model.provider_row.claude.value + "  " +
                           model.provider_row.codex.glyph + " " + model.provider_row.codex.value +
                           "  " + model.provider_row.xai.glyph + " " + model.provider_row.xai.value;
  std::string note = model.connection_note;
  if (model.view_state == t3::ui::ViewState::Provisioning) {
    title += "LOCAL SETUP";
    state = "PROVISIONING";
    activity = "Add a remembered network";
    providers = "DEVICE UNPAIRED";
    note = "OFFLINE";
  } else {
    title += model.center.project;
  }
  if (title_label_ != nullptr) {
    lv_label_set_text(title_label_, title.c_str());
  }
  if (state_label_ != nullptr) lv_label_set_text(state_label_, state.c_str());
  if (activity_label_ != nullptr) lv_label_set_text(activity_label_, activity.c_str());
  if (provider_label_ != nullptr) lv_label_set_text(provider_label_, providers.c_str());
  if (note_label_ != nullptr) lv_label_set_text(note_label_, note.c_str());
  render_arcs(model);
  bsp_display_unlock();
  return true;
}

bool IdfBspPlatform::render_enrollment_pairing(std::string_view enrollment_id,
                                                std::string_view pairing_code,
                                                std::string_view expires_at) {
  if (!initialized_ || enrollment_id.empty() || pairing_code.empty() ||
      bsp_display_lock(100) != ESP_OK) {
    return false;
  }
  std::string title = "POCKET CODE // PAIR";
  std::string state = "PAIR WITH GATEWAY";
  std::string activity = std::string(pairing_code);
  std::string provider = "ID " + std::string(enrollment_id);
  std::string note = "EXPIRES " + std::string(expires_at);
  if (title_label_ != nullptr) lv_label_set_text(title_label_, title.c_str());
  if (state_label_ != nullptr) lv_label_set_text(state_label_, state.c_str());
  if (activity_label_ != nullptr) lv_label_set_text(activity_label_, activity.c_str());
  if (provider_label_ != nullptr) lv_label_set_text(provider_label_, provider.c_str());
  if (note_label_ != nullptr) lv_label_set_text(note_label_, note.c_str());
  bsp_display_unlock();
  return true;
}

bool IdfBspPlatform::set_display_power(bool on) {
  return bsp_display_brightness_set(on ? 70 : 0) == ESP_OK;
}

bool IdfBspPlatform::wifi_start() { return wifi_.start(); }

bool IdfBspPlatform::wifi_connect(std::string_view ssid, std::string_view password) {
  return wifi_.connect(ssid, password);
}

bool IdfBspPlatform::wifi_connected() const { return wifi_.connected(); }

bool IdfBspPlatform::start_provisioning_ap(std::string_view ssid) {
  if (!wifi_.start()) return false;
  if (!wifi_.start_provisioning_ap(ssid)) return false;
  return provisioning_server_.start(ssid);
}

void IdfBspPlatform::attach_provisioning_handler(RuntimeCoordinator& coordinator) {
  provisioning_server_.attach(coordinator);
}

bool IdfBspPlatform::audio_start(std::uint32_t sample_rate_hz) {
  if (!audio_configured_ || sample_rate_hz != kPcmCaptureSampleRateHz) return false;
  if (microphone_ == nullptr) microphone_ = bsp_audio_codec_microphone_init();
  if (microphone_ == nullptr) return false;
  esp_codec_dev_sample_info_t sample{};
  sample.bits_per_sample = 16;
  sample.channel = 2;
  sample.channel_mask = 0;
  sample.sample_rate = sample_rate_hz;
  if (esp_codec_dev_open(microphone_, &sample) != ESP_CODEC_DEV_OK) return false;
  audio_open_ = true;
  return true;
}

int IdfBspPlatform::audio_read(std::span<std::int16_t> interleaved_samples) {
  if (!audio_open_ || microphone_ == nullptr || interleaved_samples.empty()) return 0;
  const int bytes = esp_codec_dev_read(microphone_, interleaved_samples.data(),
                                       static_cast<int>(interleaved_samples.size_bytes()));
  return bytes > 0 ? bytes / static_cast<int>(sizeof(std::int16_t)) : 0;
}

void IdfBspPlatform::audio_stop() {
  if (microphone_ != nullptr && audio_open_) (void)esp_codec_dev_close(microphone_);
  audio_open_ = false;
}

void IdfBspPlatform::attach_input_queue(RuntimeInputQueue& queue) { input_queue_ = &queue; }

bool IdfBspPlatform::read_pkey(std::uint8_t& edges) {
  edges = 0;
  if (axp_ == nullptr) return false;
  std::uint8_t reg = kAxpStatusRegister;
  std::uint8_t status = 0;
  if (i2c_master_transmit_receive(axp_, &reg, 1, &status, 1, 20) != ESP_OK) return false;
  const auto observed = static_cast<std::uint8_t>(status & kAxpPkeyStatusMask);
  if (observed == 0U || !clear_pkey_status(observed)) return false;
  edges = observed;
  return true;
}

bool IdfBspPlatform::read_touch(RuntimeInputEvent& event) {
  if (input_ == nullptr) return false;
  lv_indev_read(input_);
  const bool pressed = lv_indev_get_state(input_) == LV_INDEV_STATE_PRESSED;
  lv_point_t point{};
  lv_indev_get_point(input_, &point);
  if (!pressed && !touch_pressed_) return false;
  event.kind = RuntimeInputKind::Touch;
  event.touch.phase = pressed ? (touch_pressed_ ? TouchPhase::Move : TouchPhase::Down)
                              : TouchPhase::Up;
  event.touch.point.x = point.x;
  event.touch.point.y = point.y;
  event.touch.point.pressure = 1;
  event.touch.timestamp_ms = static_cast<std::uint64_t>(esp_timer_get_time() / 1'000);
  touch_pressed_ = pressed;
  return true;
}

void IdfBspPlatform::service_inputs() {
  if (input_queue_ == nullptr || !initialized_) return;
  RuntimeInputEvent touch_event;
  if (read_touch(touch_event)) (void)input_queue_->push(touch_event);

  const auto now = static_cast<std::uint64_t>(esp_timer_get_time() / 1'000);
  if (now == last_input_sample_ms_) return;
  last_input_sample_ms_ = now;
  const bool boot_active = gpio_get_level(GPIO_NUM_0) == 0;
  if (boot_active != boot_pressed_) {
    RuntimeInputEvent event;
    event.kind = RuntimeInputKind::Button;
    event.button = {ButtonId::BottomRight, boot_active, now};
    (void)input_queue_->push(event);
    boot_pressed_ = boot_active;
  }
  std::uint8_t pkey_edges = 0;
  if (read_pkey(pkey_edges)) {
    const auto decoded = decode_axp_pkey_status(pkey_edges);
    if (decoded.pressed) {
      RuntimeInputEvent event;
      event.kind = RuntimeInputKind::Button;
      event.button = {ButtonId::UpperRight, true, now};
      (void)input_queue_->push(event);
    }
    if (decoded.released) {
      RuntimeInputEvent event;
      event.kind = RuntimeInputKind::Button;
      event.button = {ButtonId::UpperRight, false, now};
      (void)input_queue_->push(event);
    }
  }
}

}  // namespace t3::companion::runtime
