#include "ambient_view.hpp"

#include "overlays.hpp"

#include <algorithm>
#include <cstddef>
#include <string_view>
#include <utility>

namespace t3::ui {
namespace {

constexpr std::size_t kMaxTokenSummaryBytes = t3::companion::kMaxShortStringBytes;
constexpr std::size_t kMaxTokenValueBytes = 32;

struct ParsedTokenValue {
  std::string value;
  std::string unit;
};

bool is_token_unit(char value) {
  return value == 'K' || value == 'M' || value == 'B' || value == 'T';
}

bool parse_token_value(std::string_view input, ParsedTokenValue& output) {
  if (input.empty() || input.size() > kMaxTokenValueBytes) {
    return false;
  }

  std::size_t numeric_size = input.size();
  if (is_token_unit(input.back())) {
    numeric_size -= 1U;
    output.unit.assign(1U, input.back());
  }
  if (numeric_size == 0U) {
    return false;
  }

  const std::string_view numeric = input.substr(0, numeric_size);
  const std::size_t decimal = numeric.find('.');
  const std::size_t integral_size = decimal == std::string_view::npos ? numeric.size() : decimal;
  if (integral_size > 1U && numeric.front() == '0') {
    return false;
  }

  std::size_t digits = 0U;
  while (digits < numeric.size() && numeric[digits] >= '0' && numeric[digits] <= '9') {
    ++digits;
  }
  if (digits == 0U) {
    return false;
  }

  if (digits < numeric.size()) {
    if (numeric[digits] != '.' || !is_token_unit(input.back())) {
      return false;
    }
    const std::size_t fraction_digits = numeric.size() - digits - 1U;
    if (fraction_digits == 0U || fraction_digits > 2U) {
      return false;
    }
    for (std::size_t index = digits + 1U; index < numeric.size(); ++index) {
      if (numeric[index] < '0' || numeric[index] > '9') {
        return false;
      }
    }
  }

  output.value.assign(numeric.data(), numeric.size());
  return true;
}

TokenContent parse_token_summary(std::string_view summary) {
  TokenContent result;
  if (summary.empty() || summary.size() > kMaxTokenSummaryBytes) {
    return result;
  }

  constexpr std::string_view kTokensSuffix = " tokens";
  if (summary.ends_with(kTokensSuffix)) {
    summary.remove_suffix(kTokensSuffix.size());
  }

  constexpr std::string_view kPipeSeparator = " | ";
  constexpr std::string_view kMiddleDotSeparator = " \xC2\xB7 ";
  const std::size_t pipe = summary.find(kPipeSeparator);
  const std::size_t middle_dot = summary.find(kMiddleDotSeparator);
  std::size_t separator = std::string_view::npos;
  std::size_t separator_size = 0U;
  if (pipe != std::string_view::npos &&
      (middle_dot == std::string_view::npos || pipe < middle_dot)) {
    separator = pipe;
    separator_size = kPipeSeparator.size();
  } else if (middle_dot != std::string_view::npos) {
    separator = middle_dot;
    separator_size = kMiddleDotSeparator.size();
  }

  ParsedTokenValue today;
  ParsedTokenValue month;
  bool has_today = false;
  bool has_month = false;

  if (summary.starts_with("Today ")) {
    constexpr std::size_t kTodayPrefixSize = 6U;
    if (separator == std::string_view::npos) {
      if (!parse_token_value(summary.substr(kTodayPrefixSize), today)) {
        return TokenContent{};
      }
      has_today = true;
    } else {
      if (separator <= kTodayPrefixSize ||
          !summary.substr(separator + separator_size).starts_with("30d ") ||
          !parse_token_value(summary.substr(kTodayPrefixSize,
                                             separator - kTodayPrefixSize), today) ||
          !parse_token_value(summary.substr(separator + separator_size + 4U), month)) {
        return TokenContent{};
      }
      has_today = true;
      has_month = true;
    }
  } else if (summary.starts_with("30d ")) {
    if (separator != std::string_view::npos ||
        !parse_token_value(summary.substr(4U), month)) {
      return TokenContent{};
    }
    has_month = true;
  } else {
    return result;
  }

  result.available = has_today || has_month;
  if (has_today) {
    result.today_value = std::move(today.value);
    result.today_unit = std::move(today.unit);
  }
  if (has_month) {
    result.month_value.assign(month.value);
    if (!month.unit.empty()) {
      result.month_value.append(month.unit);
    }
  }
  return result;
}

std::string provider_name(const std::optional<t3::companion::Provider>& provider) {
  if (!provider.has_value()) {
    return "CodexBar";
  }
  switch (*provider) {
    case t3::companion::Provider::Codex:
      return "Codex";
    case t3::companion::Provider::Claude:
      return "Claude";
    case t3::companion::Provider::Xai:
      return "xAI";
  }
  return "CodexBar";
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
  result.provider_row.xai = metric(result.rings.xai, "Grok", "7D", true);
  result.center = {"CODEXBAR", "USAGE", "Waiting for usage data", "CodexBar"};

  if (const auto* item = focused_item(focus); item != nullptr) {
    result.center.project = item->project_name.empty() ? "POCKET CODE"
                                                        : std::string(item->project_name.view());
    result.center.state = state_name(item->state);
    result.center.activity = item->activity.has_value() && !item->activity->empty()
                                 ? std::string(item->activity->view())
                                 : "Waiting for provider activity";
    result.center.provider = provider_name(item->provider);
    // The adapter uses one synthetic work item to carry token totals. It does
    // not represent an agent task, so never show Idle/Blocked as its status.
    if (item->project_id.view() == "codexbar" && item->thread_id.view() == "usage-display") {
      const bool available = result.provider_row.claude.available ||
                             result.provider_row.codex.available ||
                             result.provider_row.xai.available ||
                             result.rings.codex.weekly.available ||
                             result.rings.claude.weekly.available;
      result.center.project = "CODEXBAR";
      result.center.state = available ? "USAGE" : "NO DATA";
      result.center.provider = "CodexBar";
      // Token totals arrive in the synthetic activity field.  Cached or
      // reconnecting models are deliberately rendered as unavailable even if
      // their last activity text was a well-formed summary.
      if (result.live_handshake &&
          model.connectivity.status == t3::companion::ConnectivityStatus::Connected &&
          item->activity.has_value() && !item->activity->empty()) {
        result.tokens = parse_token_summary(item->activity->view());
      }
    }
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
