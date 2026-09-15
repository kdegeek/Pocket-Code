idf_component_register(
  SRCS "board_manifest.cpp" "board_probe.cpp" "audio_diagnostic.cpp"
  INCLUDE_DIRS "include"
  REQUIRES waveshare__esp32_s3_touch_amoled_1_75c
           espressif__esp_codec_dev
           espressif__esp_io_expander
           espressif__esp_lcd_touch
           esp_system
           esp_hw_support
           spi_flash
           heap
           esp_partition
           esp_driver_gpio
           esp_driver_i2c
           esp_driver_i2s
           esp_lcd
           esp_timer
)
