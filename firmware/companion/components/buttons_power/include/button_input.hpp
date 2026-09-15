#pragma once

#include "debounce.hpp"
#include "double_click.hpp"

#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

namespace t3::companion {

// Task 2 verified mapping: upper-right is AXP2101 PKEY (active-high status),
// bottom-right is ESP32 GPIO0 BOOT (active-low level).
enum class ButtonId : std::uint8_t {
  UpperRight,
  BottomRight,

  Pwr = UpperRight,
  Boot = BottomRight,
};

struct ButtonHardwareBinding {
  ButtonId button = ButtonId::BottomRight;
  std::string_view source;
  std::int8_t gpio = -1;
  bool active_high = false;
};

inline constexpr ButtonHardwareBinding kVerifiedUpperRightBinding{
    ButtonId::UpperRight, "AXP2101.PKEY", -1, true};
inline constexpr ButtonHardwareBinding kVerifiedBottomRightBinding{
    ButtonId::BottomRight, "ESP32.GPIO0", 0, false};

[[nodiscard]] constexpr const ButtonHardwareBinding& verified_binding(ButtonId button) {
  return button == ButtonId::UpperRight ? kVerifiedUpperRightBinding
                                        : kVerifiedBottomRightBinding;
}

struct RawButtonSample {
  ButtonId button = ButtonId::BottomRight;
  bool raw_level = false;
  std::uint64_t timestamp_ms = 0;
};

struct ButtonEvent {
  ButtonId button = ButtonId::BottomRight;
  ButtonEventKind kind = ButtonEventKind::Released;
  std::uint64_t timestamp_ms = 0;
};

struct ButtonInputConfig {
  DebounceConfig debounce{};
  ClickConfig click{};
  bool upper_right_active_high = kVerifiedUpperRightBinding.active_high;
  bool bottom_right_active_high = kVerifiedBottomRightBinding.active_high;
};

/**
 * Board-neutral button adapter.  It applies the verified polarity, debounces
 * each physical channel independently, and maps the resulting edges to
 * semantic click/hold events.  No GPIO, I2C, PMIC, reset, or power-cut call is
 * made here; an ESP-IDF adapter only supplies RawButtonSample values.
 */
class ButtonInput {
 public:
  explicit ButtonInput(ButtonInputConfig config = {});

  [[nodiscard]] std::vector<ButtonEvent> feed(const RawButtonSample& sample);
  [[nodiscard]] std::vector<ButtonEvent> feed(ButtonId button, bool raw_level,
                                              std::uint64_t now_ms);
  [[nodiscard]] std::vector<ButtonEvent> tick(std::uint64_t now_ms);
  [[nodiscard]] std::vector<ButtonEvent> cancel(ButtonId button,
                                                 std::uint64_t now_ms);
  void reset();

  [[nodiscard]] bool stable_pressed(ButtonId button) const;
  [[nodiscard]] bool active_level(ButtonId button, bool raw_level) const;

 private:
  struct Channel {
    Debouncer debounce;
    DoubleClickRecognizer clicks;

    Channel(const DebounceConfig& debounce_config, const ClickConfig& click_config)
        : debounce(debounce_config), clicks(click_config) {}
  };

  [[nodiscard]] static std::size_t index(ButtonId button);
  [[nodiscard]] bool active_high(ButtonId button) const;
  [[nodiscard]] std::vector<ButtonEvent> translate(ButtonId button,
                                                    const std::vector<ClickEvent>& events) const;

  ButtonInputConfig config_;
  std::array<Channel, 2> channels_;
};

}  // namespace t3::companion
