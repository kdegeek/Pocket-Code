#include "board_manifest.h"

namespace t3::board {

const BoardManifest &verified_manifest() {
  // Task 2 freezes the live 1.75C board.  Official BSP wiring and the
  // corrected-MCLK16 capture establish ES7210 MIC1|MIC2 as STD channels 0/1;
  // no physical left/right-side attribution is claimed.
  static const BoardManifest manifest{
      .sku = "ESP32-S3-Touch-AMOLED-1.75C",
      .hardware_revision = "V1.0",
      .verified = true,
      .chip = {
          .model = "ESP32-S3",
          .revision = 0x0002,
          .mac = {0x28, 0x84, 0x85, 0x90, 0x61, 0x1C},
      },
      .flash = {
          .size_bytes = 0x02000000,
          .detected_id = 0x00C84019,
          .mode = "DIO",
          .frequency_hz = 80000000,
      },
      .psram = {
          .size_bytes = 0x00800000,
          .mode = "embedded",
      },
      .display = {
          .controller = "CO5300",
          .width = 466,
          .height = 466,
          .column_gap = 6,
          .rotation = 0,
          .brightness_display_off_verified = true,
          .hard_power_off_avoided = true,
      },
      .touch = {
          .controller = "CST9217",
          .i2c_sda = 15,
          .i2c_scl = 14,
          .irq_gpio = 11,
          .reset_gpio = 2,
          .irq_active_low = true,
          .swap_xy = false,
          .mirror_x = true,
          .mirror_y = true,
      },
      .buttons = {
          .pwr = {"AXP2101.PKEY", -1, true},
          .boot = {"ESP32.GPIO0", 0, false},
          .upper_right = ButtonPosition::Pwr,
          .bottom_right = ButtonPosition::Boot,
          .debounce_ms = 30,
          .transitions_observed = true,
      },
      .audio = {
          .codec = "ES7210",
          .i2s_mclk = 16,
          .i2s_bclk = 9,
          .i2s_ws = 45,
          .i2s_tx = 8,
          .i2s_rx = 10,
          .front_mic_channels = {0, 1},
          .last_rms = {39.4F, 38.7F},
          .last_peak = {122, 115},
          .front_mic_channels_verified = true,
          .mic_unresolved = false,
      },
  };
  return manifest;
}

}  // namespace t3::board
