#pragma once

#include "ambient_view.hpp"
#include "lvgl.h"

#include <array>

namespace pocket_code {

// The same native view is used by the board and the headless LVGL preview.
// All calls must run under the BSP display lock on the board.
class ObsidianDisplay {
 public:
  void create(lv_obj_t* parent);
  void render(const t3::ui::UiModel& model, bool animate = true);
  void set_visible(bool visible);

 private:
  struct Ring {
    lv_obj_t* arc = nullptr;
    lv_obj_t* glow = nullptr;
    lv_obj_t* edge = nullptr;
    lv_obj_t* glint = nullptr;
    int radius = 0;
    int value = 0;
    int target = 0;
  };
  static void animate_ring(void* ring, int32_t value);
  void set_total(const std::string& value, const std::string& unit);

  lv_obj_t* root_ = nullptr;
  lv_obj_t* total_ = nullptr;
  lv_obj_t* unit_ = nullptr;
  lv_obj_t* month_ = nullptr;
  lv_obj_t* month_caption_ = nullptr;
  lv_obj_t* note_ = nullptr;
  bool total_initialized_ = false;
  std::array<lv_obj_t*, 3> percentages_{};
  std::array<Ring, 3> rings_{};
};

}  // namespace pocket_code
