#include "ambient_view.hpp"

#include "overlays.hpp"
#include "pixel_shift.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <string_view>

namespace t3::ui {
namespace {

constexpr Color kBackground{0x05, 0x07, 0x0A, 0xFF};
constexpr Color kWhite{0xF4, 0xF6, 0xF8, 0xFF};
constexpr Color kMuted{0x94, 0xA0, 0xB1, 0xFF};
constexpr Color kKicker{0x77, 0x85, 0x98, 0xFF};
constexpr Color kBlue{0x58, 0xA6, 0xFF, 0xFF};
constexpr Color kGreen{0xA2, 0xFF, 0xD7, 0xFF};
constexpr Color kError{0xEB, 0x5F, 0x71, 0xFF};
constexpr float kPi = 3.14159265358979323846F;

std::array<std::uint8_t, 7> glyph(char value) {
  switch (value) {
    case 'A':
      return {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11};
    case 'B':
      return {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E};
    case 'C':
      return {0x0F, 0x10, 0x10, 0x10, 0x10, 0x10, 0x0F};
    case 'D':
      return {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E};
    case 'E':
      return {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F};
    case 'F':
      return {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10};
    case 'G':
      return {0x0F, 0x10, 0x10, 0x17, 0x11, 0x11, 0x0F};
    case 'H':
      return {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11};
    case 'I':
      return {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x1F};
    case 'J':
      return {0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C};
    case 'K':
      return {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11};
    case 'L':
      return {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F};
    case 'M':
      return {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11};
    case 'N':
      return {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11};
    case 'O':
      return {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E};
    case 'P':
      return {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10};
    case 'Q':
      return {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D};
    case 'R':
      return {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11};
    case 'S':
      return {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E};
    case 'T':
      return {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04};
    case 'U':
      return {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E};
    case 'V':
      return {0x11, 0x11, 0x11, 0x11, 0x0A, 0x0A, 0x04};
    case 'W':
      return {0x11, 0x11, 0x11, 0x15, 0x15, 0x1B, 0x11};
    case 'X':
      return {0x11, 0x0A, 0x0A, 0x04, 0x0A, 0x0A, 0x11};
    case 'Y':
      return {0x11, 0x0A, 0x0A, 0x04, 0x04, 0x04, 0x04};
    case 'Z':
      return {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F};
    case '0':
      return {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E};
    case '1':
      return {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E};
    case '2':
      return {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F};
    case '3':
      return {0x1E, 0x01, 0x01, 0x0E, 0x01, 0x01, 0x1E};
    case '4':
      return {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02};
    case '5':
      return {0x1F, 0x10, 0x10, 0x1E, 0x01, 0x01, 0x1E};
    case '6':
      return {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E};
    case '7':
      return {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08};
    case '8':
      return {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E};
    case '9':
      return {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C};
    case '-':
      return {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00};
    case '/':
      return {0x01, 0x02, 0x02, 0x04, 0x08, 0x08, 0x10};
    case '%':
      return {0x19, 0x19, 0x02, 0x04, 0x08, 0x13, 0x13};
    case ':':
      return {0x00, 0x04, 0x04, 0x00, 0x04, 0x04, 0x00};
    case '.':
      return {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C};
    case '<':
      return {0x00, 0x04, 0x08, 0x10, 0x08, 0x04, 0x00};
    case '>':
      return {0x00, 0x10, 0x08, 0x04, 0x08, 0x10, 0x00};
    case '|':
      return {0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04};
    case '?':
      return {0x0E, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04};
    case ' ':
    default:
      return {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  }
}

void draw_text(Framebuffer& framebuffer, std::string_view text, int x, int y, int scale,
               Color color) {
  int cursor = x;
  const int unit = std::max(scale, 1);
  for (const char raw : text) {
    const char value = raw >= 'a' && raw <= 'z' ? static_cast<char>(raw - 'a' + 'A') : raw;
    const auto bitmap = glyph(value);
    for (int row = 0; row < 7; ++row) {
      for (int column = 0; column < 5; ++column) {
        if ((bitmap[static_cast<std::size_t>(row)] & (1U << (4 - column))) == 0U) {
          continue;
        }
        for (int dy = 0; dy < unit; ++dy) {
          for (int dx = 0; dx < unit; ++dx) {
            framebuffer.set(cursor + column * unit + dx, y + row * unit + dy, color);
          }
        }
      }
    }
    cursor += 6 * unit;
  }
}

void draw_centered(Framebuffer& framebuffer, std::string_view text, int y, int scale,
                   Color color) {
  const int width = static_cast<int>(text.size()) * 6 * std::max(scale, 1);
  draw_text(framebuffer, text, static_cast<int>(kDisplayCenter) - width / 2, y, scale, color);
}

void draw_round_rect(Framebuffer& framebuffer, int left, int top, int right, int bottom,
                     int radius, Color color) {
  const int radius_squared = radius * radius;
  for (int y = top; y <= bottom; ++y) {
    for (int x = left; x <= right; ++x) {
      const int nearest_x = std::clamp(x, left + radius, right - radius);
      const int nearest_y = std::clamp(y, top + radius, bottom - radius);
      const int dx = x - nearest_x;
      const int dy = y - nearest_y;
      if (dx * dx + dy * dy <= radius_squared) {
        framebuffer.set(x, y, color);
      }
    }
  }
}

void draw_ring(Framebuffer& framebuffer, const RingGeometry& geometry, const UsageValue& value,
               int center_x, int center_y) {
  const int outer = static_cast<int>(geometry.radius) + geometry.stroke / 2 + 1;
  const int inner = static_cast<int>(geometry.radius) - geometry.stroke / 2 - 1;
  const float fraction = value.available && !value.stale ?
                             static_cast<float>(std::clamp(value.percent, 0.0, 100.0) / 100.0)
                                                                  : 0.0F;
  for (int y = center_y - outer; y <= center_y + outer; ++y) {
    for (int x = center_x - outer; x <= center_x + outer; ++x) {
      const float dx = static_cast<float>(x - center_x);
      const float dy = static_cast<float>(y - center_y);
      const float distance = std::sqrt(dx * dx + dy * dy);
      if (distance < static_cast<float>(inner) || distance > static_cast<float>(outer)) {
        continue;
      }
      const float clockwise = std::atan2(dx, -dy) < 0.0F ?
                                  std::atan2(dx, -dy) + 2.0F * kPi : std::atan2(dx, -dy);
      const float progress = clockwise / (2.0F * kPi);
      framebuffer.set(x, y, progress <= fraction ? geometry.color : geometry.track);
    }
  }
}

const UsageValue& weekly(const RingValues& values, Provider provider) {
  switch (provider) {
    case Provider::Codex:
      return values.codex.weekly;
    case Provider::Claude:
      return values.claude.weekly;
    case Provider::Xai:
      return values.xai.weekly;
  }
  return values.codex.weekly;
}

void draw_provider_row(Framebuffer& framebuffer, const ProviderRow& row) {
  const std::array<std::string, 3> values = {
      row.claude.glyph + " " + row.claude.window + " " + row.claude.value,
      row.codex.glyph + " " + row.codex.window + " " + row.codex.value,
      row.xai.glyph + " " + row.xai.window + " " + row.xai.value,
  };
  const std::array<Color, 3> colors = {
      Color{0xE8, 0x89, 0x62, 0xFF},
      Color{0x58, 0xA6, 0xFF, 0xFF},
      Color{0xF4, 0xF6, 0xF8, 0xFF},
  };
  const std::array<int, 3> x = {83, 193, 303};
  for (std::size_t index = 0; index < values.size(); ++index) {
    draw_text(framebuffer, values[index], x[index], 86, 1, colors[index]);
  }
}

void draw_center_content(Framebuffer& framebuffer, const UiModel& model) {
  draw_provider_row(framebuffer, model.provider_row);
  draw_centered(framebuffer, model.center.project, 157, 1, kKicker);
  draw_centered(framebuffer, model.center.state, 183, model.center.state.size() > 14U ? 2 : 3,
                kWhite);
  draw_centered(framebuffer, model.center.activity, 226, 1, kMuted);
  draw_centered(framebuffer, model.center.provider + " | " + model.connection_note, 248, 1,
                kMuted);
}

void draw_prompt(Framebuffer& framebuffer, const UiModel& model) {
  draw_centered(framebuffer, model.prompt.kicker, 133, 1, kKicker);
  draw_centered(framebuffer, model.prompt.title, 154, 2, kWhite);
  draw_round_rect(framebuffer, 108, 190, 358, 295, 14, Color{0x18, 0x21, 0x2E, 0xFF});
  draw_centered(framebuffer, model.prompt.answer, 222, 2, kWhite);
  if (model.prompt.count > 0U) {
    const std::string count = std::to_string(model.prompt.index + 1U) + "/" +
                              std::to_string(model.prompt.count);
    draw_centered(framebuffer, count, 256, 1, kMuted);
  }
  draw_centered(framebuffer, model.prompt.pending ? "PENDING ACK" : model.prompt.gesture_hint,
                313, 1, kMuted);
}

void draw_transcript(Framebuffer& framebuffer, const UiModel& model) {
  draw_centered(framebuffer, model.transcript.kicker, 137, 1, kKicker);
  draw_round_rect(framebuffer, 92, 175, 374, 288, 14, Color{0x18, 0x21, 0x2E, 0xFF});
  draw_centered(framebuffer, model.transcript.transcript, 226, 2, kWhite);
  draw_centered(framebuffer, model.transcript.gesture_hint, 315, 1, kMuted);
}

void draw_overlay(Framebuffer& framebuffer, const OverlayModel& overlay) {
  if (!overlay.visible) {
    return;
  }
  Color tone = kWhite;
  if (overlay.tone == OverlayTone::Success) {
    tone = kGreen;
  } else if (overlay.tone == OverlayTone::Error) {
    tone = kError;
  } else if (overlay.tone == OverlayTone::Info) {
    tone = kBlue;
  }
  if (overlay.tone == OverlayTone::Success || overlay.tone == OverlayTone::Error) {
    for (const int radius : {120, 82}) {
      for (int y = static_cast<int>(kDisplayCenter) - radius;
           y <= static_cast<int>(kDisplayCenter) + radius; ++y) {
        for (int x = static_cast<int>(kDisplayCenter) - radius;
             x <= static_cast<int>(kDisplayCenter) + radius; ++x) {
          const float d = std::sqrt(static_cast<float>((x - static_cast<int>(kDisplayCenter)) *
                                                       (x - static_cast<int>(kDisplayCenter)) +
                                                       (y - static_cast<int>(kDisplayCenter)) *
                                                           (y - static_cast<int>(kDisplayCenter))));
          if (std::abs(d - static_cast<float>(radius)) <= 2.0F) {
            framebuffer.set(x, y, tone);
          }
        }
      }
    }
  }
  draw_centered(framebuffer, overlay.title, 198, overlay.title.size() > 9U ? 2 : 3, tone);
  draw_centered(framebuffer, overlay.detail, 244, 1, kMuted);
}

}  // namespace

Framebuffer::Framebuffer() : Framebuffer(kDisplayWidth, kDisplayHeight) {}

Framebuffer::Framebuffer(std::uint16_t width, std::uint16_t height)
    : width_(width), height_(height), pixels_(static_cast<std::size_t>(width) * height) {}

void Framebuffer::clear(Color color) {
  std::fill(pixels_.begin(), pixels_.end(), color);
}

void Framebuffer::set(int x, int y, Color color) {
  if (x < 0 || y < 0 || x >= static_cast<int>(width_) || y >= static_cast<int>(height_)) {
    return;
  }
  pixels_[static_cast<std::size_t>(y) * width_ + static_cast<std::size_t>(x)] = color;
}

Color Framebuffer::at(int x, int y) const {
  if (x < 0 || y < 0 || x >= static_cast<int>(width_) || y >= static_cast<int>(height_)) {
    return {};
  }
  return pixels_[static_cast<std::size_t>(y) * width_ + static_cast<std::size_t>(x)];
}

bool Framebuffer::write_ppm(const std::string& path) const {
  std::ofstream output(path, std::ios::binary);
  if (!output) {
    return false;
  }
  output << "P6\n" << width_ << ' ' << height_ << "\n255\n";
  for (const auto& pixel : pixels_) {
    output.put(static_cast<char>(pixel.red));
    output.put(static_cast<char>(pixel.green));
    output.put(static_cast<char>(pixel.blue));
  }
  return static_cast<bool>(output);
}

void render_ring_layers(Framebuffer& framebuffer, const RingValues& values, int center_x,
                        int center_y, int shift_x, int shift_y) {
  const auto geometry = ring_geometry();
  for (const auto& ring : geometry) {
    draw_ring(framebuffer, ring, weekly(values, ring.provider), center_x + shift_x,
              center_y + shift_y);
  }
}

void render_frame(const UiModel& model, Framebuffer& framebuffer, RenderOptions options) {
  framebuffer.clear(kBackground);
  const PixelShift shift = pixel_shift(options.pixel_shift_seed, options.pixel_shift_enabled,
                                       options.snapshot);
  const int center_x = static_cast<int>(kDisplayCenter) + shift.x;
  const int center_y = static_cast<int>(kDisplayCenter) + shift.y;
  // Keep the round panel's center treatment restrained and static.  It is a
  // gradient, not an animation, so a frame is safe for PSRAM double-buffering.
  for (int y = 0; y < static_cast<int>(kDisplayHeight); ++y) {
    for (int x = 0; x < static_cast<int>(kDisplayWidth); ++x) {
      const int dx = x - center_x;
      const int dy = y - center_y;
      const int distance_squared = dx * dx + dy * dy;
      if (distance_squared <= 220 * 220) {
        const float distance = std::sqrt(static_cast<float>(distance_squared));
        const float amount = std::max(0.0F, 1.0F - distance / 220.0F);
        const auto red = static_cast<std::uint8_t>(5.0F + 17.0F * amount);
        const auto green = static_cast<std::uint8_t>(7.0F + 22.0F * amount);
        const auto blue = static_cast<std::uint8_t>(10.0F + 30.0F * amount);
        framebuffer.set(x, y, Color{red, green, blue, 0xFF});
      }
    }
  }
  render_ring_layers(framebuffer, model.rings, kDisplayCenter, kDisplayCenter, shift.x, shift.y);

  if (model.view_state == ViewState::Prompt || model.view_state == ViewState::BoundedChoice) {
    draw_prompt(framebuffer, model);
  } else if (model.view_state == ViewState::Transcript) {
    draw_transcript(framebuffer, model);
  } else if (model.view_state != ViewState::Locked && model.view_state != ViewState::Incompatible &&
             model.view_state != ViewState::Provisioning) {
    draw_center_content(framebuffer, model);
  }
  draw_overlay(framebuffer, overlay_for(model.view_state));
}

}  // namespace t3::ui
