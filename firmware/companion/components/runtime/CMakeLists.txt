idf_component_register(
  SRCS "runtime.cpp" "idf_adapters.cpp" "ws_text_assembler.cpp"
  INCLUDE_DIRS "include"
  REQUIRES companion_protocol companion_state persistence wifi_provisioning transport
           ui touch_gestures buttons_power audio_capture board_waveshare
           esp_http_client esp_http_server espressif__esp_websocket_client esp_netif esp_event esp_wifi
           nvs_flash esp_timer app_update esp_driver_i2c esp_driver_gpio espressif__esp_codec_dev
           waveshare__esp32_s3_touch_amoled_1_75c lvgl__lvgl
)
