#include "button_input.hpp"

namespace t3::companion {

ButtonInput::ButtonInput(ButtonInputConfig config)
    : config_(config),
      channels_{Channel(config.debounce, config.click),
                Channel(config.debounce, config.click)} {}

std::size_t ButtonInput::index(ButtonId button) {
  return button == ButtonId::UpperRight ? 0U : 1U;
}

bool ButtonInput::active_high(ButtonId button) const {
  return button == ButtonId::UpperRight ? config_.upper_right_active_high
                                        : config_.bottom_right_active_high;
}

bool ButtonInput::active_level(ButtonId button, bool raw_level) const {
  return active_high(button) ? raw_level : !raw_level;
}

std::vector<ButtonEvent> ButtonInput::translate(
    ButtonId button, const std::vector<ClickEvent>& events) const {
  std::vector<ButtonEvent> translated;
  translated.reserve(events.size());
  for (const auto& event : events) {
    translated.push_back(ButtonEvent{button, event.kind, event.timestamp_ms});
  }
  return translated;
}

std::vector<ButtonEvent> ButtonInput::feed(const RawButtonSample& sample) {
  Channel& channel = channels_[index(sample.button)];
  const auto edge = channel.debounce.feed(active_level(sample.button, sample.raw_level),
                                          sample.timestamp_ms);
  if (!edge.has_value()) return {};
  return translate(sample.button, channel.clicks.feed(edge->pressed, edge->timestamp_ms));
}

std::vector<ButtonEvent> ButtonInput::feed(ButtonId button, bool raw_level,
                                            std::uint64_t now_ms) {
  return feed(RawButtonSample{button, raw_level, now_ms});
}

std::vector<ButtonEvent> ButtonInput::tick(std::uint64_t now_ms) {
  std::vector<ButtonEvent> events;
  for (std::size_t channel_index = 0; channel_index < channels_.size(); ++channel_index) {
    Channel& channel = channels_[channel_index];
    const ButtonId button = channel_index == 0U ? ButtonId::UpperRight
                                                : ButtonId::BottomRight;
    if (const auto edge = channel.debounce.tick(now_ms); edge.has_value()) {
      const auto click_events = channel.clicks.feed(edge->pressed, edge->timestamp_ms);
      const auto translated = translate(button, click_events);
      events.insert(events.end(), translated.begin(), translated.end());
    }
    const auto click_events = channel.clicks.tick(now_ms);
    const auto translated = translate(button, click_events);
    events.insert(events.end(), translated.begin(), translated.end());
  }
  return events;
}

std::vector<ButtonEvent> ButtonInput::cancel(ButtonId button, std::uint64_t now_ms) {
  Channel& channel = channels_[index(button)];
  return translate(button, channel.clicks.cancel(now_ms));
}

void ButtonInput::reset() {
  for (auto& channel : channels_) {
    channel.debounce.reset();
    channel.clicks.reset();
  }
}

bool ButtonInput::stable_pressed(ButtonId button) const {
  return channels_[index(button)].debounce.stable_pressed();
}

}  // namespace t3::companion
