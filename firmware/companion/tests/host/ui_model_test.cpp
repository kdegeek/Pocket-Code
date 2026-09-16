#include "ambient_view.hpp"
#include "pixel_shift.hpp"
#include "ring_renderer.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

using namespace t3::companion;
using namespace t3::ui;

int failures = 0;

void expect(bool condition, const char* message) {
  if (!condition) {
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
  }
}

Id id(const char* value) {
  Id result;
  (void)result.assign(value);
  return result;
}

UsageProvider usage(Provider provider, UsageStatus status, double weekly,
                    double five_hour) {
  UsageProvider result;
  result.provider = provider;
  result.status = status;
  RateWindow week;
  week.period = RateWindow::Period::Weekly;
  week.used_percent = weekly;
  (void)result.windows.push(week);
  RateWindow five;
  five.period = RateWindow::Period::FiveHour;
  five.used_percent = five_hour;
  (void)result.windows.push(five);
  return result;
}

UsageSnapshot sample_usage() {
  UsageSnapshot result;
  (void)result.providers.push(usage(Provider::Codex, UsageStatus::Available, 54.0, 61.0));
  (void)result.providers.push(usage(Provider::Claude, UsageStatus::Available, 31.0, 28.0));
  UsageProvider xai;
  xai.provider = Provider::Xai;
  xai.status = UsageStatus::Available;
  RateWindow xai_week;
  xai_week.period = RateWindow::Period::Weekly;
  xai_week.used_percent = 18.0;
  (void)xai.windows.push(xai_week);
  (void)result.providers.push(xai);
  result.provider_count = result.providers.count;
  return result;
}

void test_ring_contract_and_explicit_missing_values() {
  const auto geometry = ring_geometry();
  expect(geometry[0].radius > geometry[1].radius && geometry[1].radius > geometry[2].radius,
         "rings are ordered outer to inner");
  expect(geometry[0].stroke == geometry[1].stroke && geometry[1].stroke == geometry[2].stroke,
         "rings have equal stroke thickness");
  expect(geometry[0].provider == Provider::Codex && geometry[1].provider == Provider::Claude &&
             geometry[2].provider == Provider::Xai,
         "rings use Codex, Claude, and xAI order");
  expect(kDisplayWidth == 466 && kDisplayHeight == 466 && kColumnGap == 6 &&
             kDisplayRotation == 0,
         "CO5300 uses the verified 466px logical upright surface and gap");
  const auto flush = round_flush_rect(3, 4, 8, 9);
  expect(flush.x_start % 2 == 0 && flush.y_start % 2 == 0 && flush.x_end % 2 == 1 &&
             flush.y_end % 2 == 1,
         "CO5300 flush rectangles round to even starts and odd ends");

  const auto rings = make_ring_values(sample_usage());
  expect(rings.codex.weekly.available && rings.codex.weekly.percent == 54.0,
         "Codex weekly usage drives the outer ring");
  expect(rings.claude.weekly.available && rings.claude.weekly.percent == 31.0,
         "Claude weekly usage drives the middle ring");
  expect(rings.xai.weekly.available && rings.xai.weekly.percent == 18.0,
         "xAI weekly usage drives the inner ring");
  expect(rings.xai.five_hour.display_text() == "--",
         "xAI has no invented five-hour value");

  UsageSnapshot stale;
  (void)stale.providers.push(usage(Provider::Codex, UsageStatus::Stale, 42.0, 42.0));
  stale.provider_count = stale.providers.count;
  const auto stale_rings = make_ring_values(stale);
  expect(!stale_rings.codex.weekly.available && stale_rings.codex.weekly.display_text() == "--",
         "stale usage is explicit and never rendered as zero");
}

void test_view_state_and_center_content() {
  SnapshotModel model;
  WorkItemRecord work;
  work.item.thread_id = id("thread");
  (void)work.item.project_name.assign("Millwork");
  work.item.state = WorkItemState::Working;
  work.item.activity = ShortString{};
  (void)work.item.activity->assign("Provider runtime events");
  work.item.provider = Provider::Codex;
  model.work_items.push_back(work);
  model.connectivity.status = ConnectivityStatus::Connected;
  model.live_handshake = true;
  FocusQueueState focus = make_focus_queue(model, 100);
  InteractionState interaction = make_interaction_state(true, "device");

  UiModel ambient = compose_ui_model(model, focus, interaction, sample_usage());
  expect(ambient.view_state == ViewState::Ambient, "connected idle interaction renders ambient");
  expect(ambient.center.project == "Millwork", "center carries project name");
  expect(ambient.center.activity == "Provider runtime events", "center carries activity");
  expect(ambient.center.provider == "Codex", "center carries provider context");
  expect(ambient.provider_row.claude.window == "5H" && ambient.provider_row.codex.window == "5H" &&
             ambient.provider_row.xai.window == "7D",
         "glyph row uses Claude/Codex 5H and xAI 7D");

  interaction.mode = InteractionMode::AppliedGlow;
  UiModel applied = compose_ui_model(model, focus, interaction, sample_usage());
  expect(applied.view_state == ViewState::AppliedGlow, "applied acknowledgement is explicit");
  interaction.mode = InteractionMode::ErrorGlow;
  UiModel rejected = compose_ui_model(model, focus, interaction, sample_usage());
  expect(rejected.view_state == ViewState::RejectedGlow, "rejected acknowledgement is explicit");

  model.connectivity.status = ConnectivityStatus::Reconnecting;
  UiModel reconnecting = compose_ui_model(model, focus, interaction, sample_usage());
  expect(reconnecting.view_state == ViewState::Reconnecting, "reconnecting is not ambient success");

  UiOptions locked_options;
  locked_options.locked = true;
  UiModel locked = compose_ui_model(model, focus, interaction, sample_usage(), locked_options);
  expect(locked.view_state == ViewState::Locked, "locked presentation is explicit");
}

void test_codexbar_usage_is_not_an_agent_task() {
  SnapshotModel model;
  WorkItemRecord work;
  work.item.thread_id = id("usage-display");
  work.item.project_id = id("codexbar");
  work.item.state = WorkItemState::Idle;
  work.item.activity = ShortString{};
  (void)work.item.activity->assign("Today 12.3M | 30d 456.7M tokens");
  model.work_items.push_back(work);
  model.connectivity.status = ConnectivityStatus::Connected;
  model.live_handshake = true;
  auto focus = make_focus_queue(model, 0);
  auto interaction = make_interaction_state(true, "device");
  const auto ui = compose_ui_model(model, focus, interaction, sample_usage());
  expect(ui.center.project == "CODEXBAR" && ui.center.state == "USAGE",
         "usage snapshot does not present a fake Idle agent task");
  expect(ui.center.activity == "Today 12.3M | 30d 456.7M tokens",
         "usage presentation retains actual token totals");
  const auto unavailable = compose_ui_model(model, focus, interaction, UsageSnapshot{});
  expect(unavailable.center.state == "NO DATA", "missing quotas have a data status");
}

void test_deterministic_framebuffer_and_pixel_shift() {
  SnapshotModel model;
  model.connectivity.status = ConnectivityStatus::Connected;
  model.live_handshake = true;
  FocusQueueState focus = make_focus_queue(model, 0);
  InteractionState interaction = make_interaction_state(true, "device");
  UiModel ui = compose_ui_model(model, focus, interaction, sample_usage());
  Framebuffer first;
  Framebuffer second;
  render_frame(ui, first, RenderOptions{.snapshot = true, .pixel_shift_seed = 19});
  render_frame(ui, second, RenderOptions{.snapshot = true, .pixel_shift_seed = 19});
  expect(first.width() == 466 && first.height() == 466, "framebuffer is the CO5300 logical size");
  expect(first.pixels() == second.pixels(), "snapshot rendering is deterministic");
  expect(pixel_shift(19, true, true).x == 0 && pixel_shift(19, true, true).y == 0,
         "pixel shift is disabled for snapshots");
  expect(pixel_shift(19, false, false).x == 0 && pixel_shift(19, false, false).y == 0,
         "pixel shift can be disabled for runtime");
  const auto active_shift = pixel_shift(19, true, false);
  expect(active_shift.x >= -1 && active_shift.x <= 1 && active_shift.y >= -1 &&
             active_shift.y <= 1,
         "runtime pixel shift stays within one pixel");
}

}  // namespace

int main() {
  test_ring_contract_and_explicit_missing_values();
  test_view_state_and_center_content();
  test_codexbar_usage_is_not_an_agent_task();
  test_deterministic_framebuffer_and_pixel_shift();
  if (failures != 0) {
    std::cerr << failures << " UI model test(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "T3 Companion UI model tests passed\n";
  return EXIT_SUCCESS;
}
