#include "board_manifest.h"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

void expect(bool condition, const char *message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}

}  // namespace

int main() {
  const auto &manifest = t3::board::verified_manifest();

  expect(manifest.sku == "ESP32-S3-Touch-AMOLED-1.75C",
         "manifest keeps the verified Waveshare 1.75C SKU");
  expect(manifest.display.controller == "CO5300",
         "manifest records the CO5300 display controller");
  expect(manifest.display.width == 466 && manifest.display.height == 466,
         "manifest records the 466x466 display geometry");
  expect(manifest.display.column_gap == 6,
         "manifest records the CO5300 column gap");
  expect(manifest.touch.controller == "CST9217",
         "manifest records the CST9217 touch controller");
  expect(manifest.touch.i2c_sda == 15 && manifest.touch.i2c_scl == 14 &&
             manifest.touch.irq_gpio == 11 && manifest.touch.reset_gpio == 2,
         "manifest records the official 1.75C touch pins");
  expect(manifest.buttons.pwr.source == "AXP2101.PKEY" &&
             manifest.buttons.pwr.active_high,
         "manifest records the verified AXP2101 PWR source");
  expect(manifest.buttons.boot.gpio == 0 && !manifest.buttons.boot.active_high,
         "manifest records the active-low BOOT input");
  expect(manifest.audio.codec == "ES7210" && manifest.audio.i2s_mclk == 16 &&
             manifest.audio.i2s_bclk == 9 && manifest.audio.i2s_ws == 45 &&
             manifest.audio.i2s_tx == 8 && manifest.audio.i2s_rx == 10,
         "manifest records the official 1.75C audio pins");
  expect(manifest.audio.front_mic_channels[0] == 0 &&
             manifest.audio.front_mic_channels[1] == 1,
         "manifest records front microphones on STD stereo channels 0 and 1");

  expect(manifest.verified, "manifest is fully verified after STD stereo capture");
  expect(manifest.chip.model == "ESP32-S3" && manifest.chip.mac ==
             std::array<std::uint8_t, 6>{0x28, 0x84, 0x85, 0x90, 0x61, 0x1C},
         "chip model, revision identity, and MAC are verified");
  expect(manifest.flash.size_bytes == 0x02000000,
         "physical flash geometry is verified as 32 MiB");
  expect(manifest.flash.detected_id == 0x00C84019 && manifest.flash.mode == "DIO" &&
             manifest.flash.frequency_hz == 80000000,
         "flash ID, mode, and frequency are verified");
  expect(manifest.psram.size_bytes == 0x00800000,
         "embedded PSRAM geometry is verified as 8 MiB");
  expect(manifest.chip.revision == 0x0002,
         "ESP32-S3 revision 0.2 is verified");
  expect(manifest.buttons.upper_right == t3::board::ButtonPosition::Pwr,
         "upper-right physical button is verified");
  expect(manifest.buttons.bottom_right == t3::board::ButtonPosition::Boot,
         "bottom-right physical button is verified");
  expect(manifest.touch.mirror_x && manifest.touch.mirror_y &&
             !manifest.touch.swap_xy,
         "touch orientation is verified");
  expect(manifest.display.brightness_display_off_verified,
         "display-off brightness behavior is verified");
  expect(manifest.display.hard_power_off_avoided,
         "display probe avoided hard power-off");
  expect(manifest.buttons.transitions_observed,
         "both physical button transitions were observed");
  expect(manifest.audio.front_mic_channels_verified && !manifest.audio.mic_unresolved,
         "front microphone STD stereo channels are verified");

  std::cout << "board manifest assertions passed\n";
  return 0;
}
