idf_component_register(
  SRCS
    "obsidian_background.c"
    "ring_gradients.c"
    "provider_glyphs.c"
    "pc_inter_64.c"
    "pc_inter_84.c"
    "pc_inter_34.c"
    "pc_inter_22.c"
    "pc_inter_18.c"
    "pc_inter_17.c"
    "pc_inter_14.c"
    "pc_inter_11.c"
  INCLUDE_DIRS "include"
  REQUIRES lvgl__lvgl
)
