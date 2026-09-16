#include "obsidian_display.hpp"
#include "obsidian_assets.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace pocket_code {
namespace {

constexpr int kCenter = 233;
constexpr std::array<int, 3> kRadii{209, 187, 165};
constexpr std::array<int, 3> kColumnCenters{156, 233, 310};
constexpr std::array<uint32_t, 3> kProviderColors{0x91CAFF, 0xE8B595, 0xD5E4F6};

lv_obj_t* label(lv_obj_t* parent, const lv_font_t* font, uint32_t color,
                const char* text) {
  auto* obj = lv_label_create(parent);
  lv_obj_remove_style_all(obj);
  lv_obj_set_style_text_font(obj, font, 0);
  lv_obj_set_style_text_color(obj, lv_color_hex(color), 0);
  lv_obj_set_style_text_opa(obj, LV_OPA_COVER, 0);
  lv_label_set_text(obj, text);
  return obj;
}

void text(lv_obj_t* obj, const std::string& value) {
  if (value != lv_label_get_text(obj)) lv_label_set_text(obj, value.c_str());
}

void center_at(lv_obj_t* obj, int center_x, int top) {
  lv_obj_update_layout(obj);
  lv_obj_set_pos(obj, center_x - lv_obj_get_width(obj) / 2, top);
}

lv_obj_t* arc(lv_obj_t* parent, int radius, int width, uint32_t color,
              lv_opa_t opacity, const lv_image_dsc_t* texture = nullptr) {
  auto* obj = lv_arc_create(parent);
  lv_obj_remove_style_all(obj);
  lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_size(obj, radius * 2 + width, radius * 2 + width);
  lv_obj_center(obj);
  lv_obj_set_style_arc_width(obj, width, LV_PART_INDICATOR);
  lv_obj_set_style_arc_opa(obj, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, LV_PART_KNOB);
  lv_obj_set_style_arc_color(obj, lv_color_hex(color), LV_PART_INDICATOR);
  lv_obj_set_style_arc_opa(obj, opacity, LV_PART_INDICATOR);
  lv_obj_set_style_arc_rounded(obj, true, LV_PART_INDICATOR);
  if (texture) lv_obj_set_style_arc_image_src(obj, texture, LV_PART_INDICATOR);
  lv_arc_set_bg_angles(obj, 0, 360);
  lv_arc_set_rotation(obj, 270);
  lv_arc_set_range(obj, 0, 1000);
  lv_arc_set_value(obj, 0);
  return obj;
}

}  // namespace

void ObsidianDisplay::create(lv_obj_t* parent) {
  root_ = lv_obj_create(parent);
  lv_obj_remove_style_all(root_);
  lv_obj_set_size(root_, 466, 466);
  lv_obj_remove_flag(root_, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(root_, LV_OBJ_FLAG_CLICKABLE);
  auto* background = lv_image_create(root_);
  lv_image_set_src(background, &pc_obsidian_background);
  lv_obj_set_pos(background, 0, 0);

  const std::array<const lv_image_dsc_t*, 3> textures{
      &pc_ring_codex, &pc_ring_claude, &pc_ring_grok};
  for (size_t i = 0; i < rings_.size(); ++i) {
    auto& ring = rings_[i];
    ring.radius = kRadii[i];
    ring.glow = arc(root_, ring.radius, 16, kProviderColors[i], 16);
    ring.arc = arc(root_, ring.radius, 10, kProviderColors[i], LV_OPA_COVER, textures[i]);
    ring.edge = arc(root_, ring.radius, 1, 0xDAEDFF, 65);
    lv_obj_align(ring.edge, LV_ALIGN_CENTER, 0, -2);
    ring.glint = lv_obj_create(root_);
    lv_obj_remove_style_all(ring.glint);
    lv_obj_set_size(ring.glint, 4, 4);
    lv_obj_set_style_radius(ring.glint, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(ring.glint, lv_color_hex(0xEAF6FF), 0);
    lv_obj_set_style_bg_opa(ring.glint, 133, 0);
    animate_ring(&ring, 0);
  }

  total_ = label(root_, &pc_inter_84, 0xF4F8FF, "--");
  lv_obj_set_style_text_letter_space(total_, -4, 0);
  unit_ = label(root_, &pc_inter_34, 0xB5C8DD, "");
  auto* today = label(root_, &pc_inter_18, 0xA9BED4, "tokens today");
  center_at(today, kCenter, 216);
  month_ = label(root_, &pc_inter_17, 0xD6E2F0, "--");
  month_caption_ = label(root_, &pc_inter_17, 0x9BAFC6, "in 30 days");
  const std::array<const char*, 3> names{"Codex", "Claude", "Grok"};
  const std::array<const lv_image_dsc_t*, 3> glyphs{
      &pc_glyph_codex, &pc_glyph_claude, &pc_glyph_grok};
  for (size_t i = 0; i < names.size(); ++i) {
    auto* name = label(root_, &pc_inter_14, 0xB4C6D9, names[i]);
    lv_obj_update_layout(name);
    const int start = kColumnCenters[i] - (18 + 5 + lv_obj_get_width(name)) / 2;
    auto* glyph = lv_image_create(root_);
    lv_image_set_src(glyph, glyphs[i]);
    lv_obj_set_style_image_recolor(glyph, lv_color_hex(kProviderColors[i]), 0);
    lv_obj_set_style_image_recolor_opa(glyph, LV_OPA_COVER, 0);
    lv_obj_set_pos(glyph, start, 285);
    lv_obj_set_pos(name, start + 23, 285);
    percentages_[i] = label(root_, &pc_inter_22, 0xEEF5FF, "--");
    lv_obj_set_style_text_letter_space(percentages_[i], -1, 0);
    center_at(percentages_[i], kColumnCenters[i], 307);
  }
  note_ = label(root_, &pc_inter_11, 0x8FA6BD, "WEEKLY USAGE");
  lv_obj_set_style_text_letter_space(note_, 1, 0);
  center_at(note_, kCenter, 339);
  set_total("--", "");
}

void ObsidianDisplay::animate_ring(void* context, int32_t value) {
  auto& ring = *static_cast<Ring*>(context);
  ring.value = value;
  for (auto* obj : {ring.arc, ring.glow, ring.edge}) {
    // LVGL 9.5's image-backed arc mask treats 0 and 360 as the same angle.
    // A 359-degree textured arc closes seamlessly with its rounded 10px caps.
    lv_arc_set_value(obj, obj == ring.arc ? std::min<int32_t>(value, 999) : value);
    // A zero-length indicator already draws nothing. Hiding/showing the object
    // invalidates its entire bounding square, turning the first tiny animation
    // step into a full-face redraw and consuming the animation's time budget.
  }
  if (value == 0) lv_obj_add_flag(ring.glint, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_remove_flag(ring.glint, LV_OBJ_FLAG_HIDDEN);
  const double angle = value * 2.0 * 3.141592653589793 / 1000.0;
  lv_obj_set_pos(ring.glint, kCenter + std::lround(ring.radius * std::sin(angle)) - 2,
                 kCenter - std::lround(ring.radius * std::cos(angle)) - 2);
}

void ObsidianDisplay::set_total(const std::string& value, const std::string& unit) {
  if (total_initialized_ && value == lv_label_get_text(total_) && unit == lv_label_get_text(unit_)) return;
  total_initialized_ = true;
  text(total_, value);
  text(unit_, unit);
  lv_obj_set_width(total_, LV_SIZE_CONTENT);
  lv_obj_update_layout(unit_);
  const int unit_width = unit.empty() ? 0 : 4 + lv_obj_get_width(unit_);
  const lv_font_t* font = &pc_inter_84;
  int width = 0;
  // Long valid totals (e.g. 999.9M) must fit the circular lens too.
  for (const auto* candidate : {&pc_inter_84, &pc_inter_64, &pc_inter_34}) {
    font = candidate;
    lv_obj_set_style_text_font(total_, font, 0);
    lv_obj_set_style_text_letter_space(total_, font == &pc_inter_84 ? -4 :
                                               font == &pc_inter_64 ? -3 : 0, 0);
    lv_obj_update_layout(total_);
    width = lv_obj_get_width(total_) + unit_width;
    if (width <= 238) break;
  }
  if (width > 238) {
    lv_label_set_long_mode(total_, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(total_, 238 - unit_width);
    lv_obj_update_layout(total_);
    width = 238;
  }
  const int x = kCenter - width / 2;
  // Both Inter fonts share the same baseline; their bitmap line heights differ.
  const int baseline = 199;
  lv_obj_set_pos(total_, x, baseline - (font->line_height - font->base_line));
  lv_obj_set_pos(unit_, x + lv_obj_get_width(total_) + 4,
                 baseline - (pc_inter_34.line_height - pc_inter_34.base_line));
}

void ObsidianDisplay::render(const t3::ui::UiModel& model, bool animate) {
  if (!root_) return;
  const auto& tokens = model.tokens;
  set_total(tokens.today_value, tokens.today_unit);
  text(month_, tokens.month_value);
  lv_obj_update_layout(month_);
  lv_obj_update_layout(month_caption_);
  const int month_width = lv_obj_get_width(month_) + 5 + lv_obj_get_width(month_caption_);
  lv_obj_set_pos(month_, kCenter - month_width / 2, 251);
  lv_obj_set_pos(month_caption_, kCenter - month_width / 2 + lv_obj_get_width(month_) + 5, 251);
  const std::array<const t3::ui::UsageValue*, 3> values{
      &model.rings.codex.weekly, &model.rings.claude.weekly, &model.rings.xai.weekly};
  for (size_t i = 0; i < values.size(); ++i) {
    const auto& value = *values[i];
    text(percentages_[i], value.display_text());
    center_at(percentages_[i], kColumnCenters[i], 307);
    auto& ring = rings_[i];
    const int target = value.available && !value.stale
        ? static_cast<int>(std::lround(std::clamp(value.percent, 0.0, 100.0) * 10)) : 0;
    if (target == ring.target) continue;
    ring.target = target;
    lv_anim_delete(&ring, animate_ring);
    if (!animate || !value.available || value.stale) {
      animate_ring(&ring, target);
      continue;
    }
    lv_anim_t animation;
    lv_anim_init(&animation);
    lv_anim_set_var(&animation, &ring);
    lv_anim_set_exec_cb(&animation, animate_ring);
    lv_anim_set_values(&animation, ring.value, target);
    lv_anim_set_duration(&animation, 900);
    lv_anim_set_path_cb(&animation, lv_anim_path_ease_out);
    lv_anim_start(&animation);
  }
  text(note_, model.live_handshake ? "WEEKLY USAGE" : "RECONNECTING");
  center_at(note_, kCenter, 339);
}

void ObsidianDisplay::set_visible(bool visible) {
  if (!root_) return;
  if (visible) lv_obj_remove_flag(root_, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);
}

}  // namespace pocket_code
