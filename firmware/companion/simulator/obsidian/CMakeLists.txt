cmake_minimum_required(VERSION 3.16)
project(pocket_code_obsidian_preview LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(COMPANION_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../..")
set(LVGL_DIR "${COMPANION_DIR}/managed_components/lvgl__lvgl" CACHE PATH "LVGL 9.5 source")
set(LV_BUILD_CONF_PATH "${CMAKE_CURRENT_SOURCE_DIR}/lv_conf.h" CACHE PATH "" FORCE)
set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_USE_THORVG_INTERNAL OFF CACHE BOOL "" FORCE)
add_subdirectory("${LVGL_DIR}" lvgl-build)
file(GLOB_RECURSE OBSIDIAN_ASSETS CONFIGURE_DEPENDS "${COMPANION_DIR}/components/obsidian_assets/*.c")
add_executable(obsidian_preview
  main.cpp
  ${COMPANION_DIR}/components/obsidian_display/obsidian_display.cpp
  ${COMPANION_DIR}/components/ui/ring_renderer.cpp
  ${OBSIDIAN_ASSETS}
)
target_include_directories(obsidian_preview PRIVATE
  ${COMPANION_DIR}/components/obsidian_display/include
  ${COMPANION_DIR}/components/obsidian_assets/include
  ${COMPANION_DIR}/components/ui/include
  ${COMPANION_DIR}/components/companion_state/include
  ${COMPANION_DIR}/components/companion_protocol/include
)
target_link_libraries(obsidian_preview PRIVATE lvgl)
enable_testing()
add_test(NAME obsidian_ring_limits COMMAND obsidian_preview "${CMAKE_CURRENT_BINARY_DIR}/limits.ppm" limits)
add_test(NAME obsidian_animation_damage COMMAND obsidian_preview "${CMAKE_CURRENT_BINARY_DIR}/animation-damage.ppm" animation-damage)
