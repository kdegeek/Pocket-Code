#pragma once

#include "ambient_view_fwd.hpp"
#include "focus_queue.hpp"
#include "interaction_fsm.hpp"
#include "prompt_cards.hpp"
#include "ring_renderer.hpp"
#include "transcript_card.hpp"

#include <cstdint>
#include <string>

namespace t3::ui {

using t3::companion::FocusQueueState;
using t3::companion::InteractionState;
using t3::companion::SnapshotModel;
using t3::companion::UsageSnapshot;

struct CenterContent {
  std::string project;
  std::string state;
  std::string activity;
  std::string provider;
};

struct GlyphMetric {
  std::string glyph;
  std::string window;
  std::string value;
  bool available = false;
};

struct ProviderRow {
  GlyphMetric claude;
  GlyphMetric codex;
  GlyphMetric xai;
};

struct UiOptions {
  bool locked = false;
  bool protocol_incompatible = false;
  bool provisioning = false;
  bool pixel_shift_enabled = true;
  std::uint32_t pixel_shift_seed = 0;
};

struct UiModel {
  ViewState view_state = ViewState::Ambient;
  CenterContent center;
  ProviderRow provider_row;
  RingValues rings;
  PromptCardModel prompt;
  TranscriptCardModel transcript;
  std::string connection_note;
  bool live_handshake = false;
  UiOptions options;
};

struct RenderOptions {
  bool snapshot = true;
  bool pixel_shift_enabled = true;
  std::uint32_t pixel_shift_seed = 0;
};

[[nodiscard]] UiModel compose_ui_model(const SnapshotModel& model,
                                       const FocusQueueState& focus,
                                       const InteractionState& interaction,
                                       const UsageSnapshot& usage,
                                       UiOptions options = {});

void render_frame(const UiModel& model, Framebuffer& framebuffer,
                  RenderOptions options = {});

}  // namespace t3::ui
