#pragma once

/*
 * Compiled display resources for the 466 x 466 Obsidian face.
 *
 * Images use LVGL 9's native RGB565 and A8 formats.  The three provider
 * textures are full-face, stable x/y gradients; the native renderer applies
 * its arc mask when it draws them.  The background is opaque, including the
 * square pixels outside the circular face, so a full-screen image draw cannot
 * expose an uninitialized framebuffer corner.
 */

#include <lvgl.h>

#define PC_OBSIDIAN_FACE_WIDTH 466u
#define PC_OBSIDIAN_FACE_HEIGHT 466u
#define PC_OBSIDIAN_FACE_STRIDE 932u
#define PC_OBSIDIAN_GLYPH_WIDTH 18u
#define PC_OBSIDIAN_GLYPH_HEIGHT 18u
#define PC_OBSIDIAN_GLYPH_STRIDE 18u

#ifdef __cplusplus
extern "C" {
#endif

extern const lv_image_dsc_t pc_obsidian_background;
extern const lv_image_dsc_t pc_ring_codex;
extern const lv_image_dsc_t pc_ring_claude;
extern const lv_image_dsc_t pc_ring_grok;

extern const lv_image_dsc_t pc_glyph_codex;
extern const lv_image_dsc_t pc_glyph_claude;
extern const lv_image_dsc_t pc_glyph_grok;

extern const lv_font_t pc_inter_64;
extern const lv_font_t pc_inter_84;
extern const lv_font_t pc_inter_34;
extern const lv_font_t pc_inter_22;
extern const lv_font_t pc_inter_18;
extern const lv_font_t pc_inter_17;
extern const lv_font_t pc_inter_14;
extern const lv_font_t pc_inter_11;

#ifdef __cplusplus
} /* extern "C" */
#endif
