#include "ambient_view.hpp"

#include "overlays.hpp"

#include <algorithm>
#include <utility>

namespace t3::ui {
namespace {

std::string provider_name(const std::optional<t3::companion::Provider>& provider) {
  if (!provider.has_value()) {
    return "T3";
  }
  switch (*provider) {
    case t3::companion::Provider::Codex:
      return "Codex";
    case t3::companion::Provider::Claude:
      return "Claude";
    case t3::companion::Provider::Xai:
      return "xAI";
  }
  return "T3";
}

std::string state_name(t3::companion::WorkItemState state) {
  switch (state) {
    case t3::companion::WorkItemState::NeedsInput:
      return "Input required";
    case t3::companion::WorkItemState::Failed:
      return "Failed";
    case t3::companion::WorkItemState::Blocked:
      return "Blocked";
    case t3::companion::WorkItemState::Working:
      return "Working";
    case t3::companion::WorkItemState::Completed:
      return "Completed";
    case t3::companion::WorkItemState::Idle:
      return "Idle";
  }
  return "Idle";
}

const t3::companion::WorkItem* focused_item(const FocusQueueState& focus) {
  const auto focused = focused_thread(focus);
  if (!focused.has_value()) {
    return nullptr;
  }
  for (const auto& record : focus.model.work_items) {
    if (record.item.thread_id == *focused) {
      return &record.item;
    }
  }
  return nullptr;
}

GlyphMetric metric(const ProviderRingValues& values, std::string glyph, std::string window,
                   bool weekly) {
  const UsageValue& value = weekly ? values.weekly : values.five_hour;
  GlyphMetric result;
  result.glyph = std::move(glyph);
  result.window = std::move(window);
  result.value = value.display_text();
  result.available = value.available && !value.stale;
  return result;
}

}  // namespace

UiModel compose_ui_model(const SnapshotModel& model, const FocusQueueState& focus,
                         const InteractionState& interaction, const UsageSnapshot& usage,
                         UiOptions options) {
  UiModel result;
  result.options = options;
  result.live_handshake = interaction.live_handshake && model.live_handshake;
  result.rings = make_ring_values(usage);
  result.provider_row.claude = metric(result.rings.claude, "Claude", "5H", false);
  result.provider_row.codex = metric(result.rings.codex, "Codex", "5H", false);
  result.provider_row.xai = metric(result.rings.xai, "xAI", "7D", true);
  result.center = {"NO ACTIVE PROJECT", "Idle", "Waiting for T3 work", "T3"};

  if (const auto* item = focused_item(focus); item != nullptr) {
    result.center.project = item->project_name.empty() ? "T3 PROJECT"
                                                        : std::string(item->project_name.view());
    result.center.state = state_name(item->state);
    result.center.activity = item->activity.has_value() && !item->activity->empty()
                                 ? std::string(item->activity->view())
                                 : "Waiting for provider activity";
    result.center.provider = provider_name(item->provider);
  }

  result.prompt = make_prompt_card(interaction);
  result.transcript = make_transcript_card(interaction);
  result.connection_note = result.live_handshake ? "LIVE" : "STALE";

  if (options.locked) {
    result.view_state = ViewState::Locked;
  } else if (options.protocol_incompatible) {
    result.view_state = ViewState::Incompatible;
  } else if (options.provisioning) {
    result.view_state = ViewState::Provisioning;
  } else if (model.connectivity.status != t3::companion::ConnectivityStatus::Connected ||
             !result.live_handshake) {
    result.view_state = ViewState::Reconnecting;
  } else {
    switch (interaction.mode) {
      case InteractionMode::Approval:
      case InteractionMode::AwaitingAck:
        result.view_state = interaction.request.has_value() &&
                                    interaction.request->kind == PendingRequestKind::UserInput
                                ? ViewState::BoundedChoice
                                : ViewState::Prompt;
        break;
      case InteractionMode::UserInput:
        result.view_state = ViewState::BoundedChoice;
        break;
      case InteractionMode::Transcript:
        result.view_state = ViewState::Transcript;
        break;
      case InteractionMode::AppliedGlow:
        result.view_state = ViewState::AppliedGlow;
        break;
      case InteractionMode::ErrorGlow:
        result.view_state = ViewState::RejectedGlow;
        break;
      case InteractionMode::Ambient:
        result.view_state = ViewState::Ambient;
        break;
    }
  }
  return result;
}

}  // namespace t3::ui
