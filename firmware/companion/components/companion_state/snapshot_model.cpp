#include "snapshot_model.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <limits>
#include <string>

namespace t3::companion {
namespace {

std::uint64_t decimal(std::string_view value, std::size_t offset, std::size_t width,
                      bool& valid) {
  if (offset + width > value.size()) {
    valid = false;
    return 0;
  }
  std::uint64_t output = 0;
  for (std::size_t index = 0; index < width; ++index) {
    const unsigned char byte = static_cast<unsigned char>(value[offset + index]);
    if (byte < '0' || byte > '9') {
      valid = false;
      return 0;
    }
    const std::uint64_t digit = byte - static_cast<unsigned char>('0');
    if (output > (std::numeric_limits<std::uint64_t>::max() - digit) / 10U) {
      valid = false;
      return 0;
    }
    output = output * 10U + digit;
  }
  return output;
}

std::int64_t days_from_civil(std::int64_t year, unsigned month, unsigned day) {
  year -= month <= 2U ? 1 : 0;
  const std::int64_t era = (year >= 0 ? year : year - 399) / 400;
  const unsigned year_of_era = static_cast<unsigned>(year - era * 400);
  const unsigned shifted_month = static_cast<unsigned>(static_cast<int>(month) +
                                                        (month > 2U ? -3 : 9));
  const unsigned day_of_year = (153U * shifted_month + 2U) / 5U + day - 1U;
  const unsigned day_of_era = year_of_era * 365U + year_of_era / 4U - year_of_era / 100U + day_of_year;
  return era * 146097 + static_cast<std::int64_t>(day_of_era) - 719468;
}

bool separator(std::string_view value, std::size_t offset, char expected) {
  return offset < value.size() && value[offset] == expected;
}

int priority_for_pending(const SnapshotModel& model, const PendingRequest& request,
                         std::uint64_t now_ms) {
  for (const auto& work_item : model.work_items) {
    if (work_item.item.thread_id == request.thread_id) {
      return work_item_priority(work_item, now_ms);
    }
  }
  return 6;
}

}  // namespace

std::uint64_t parse_datetime_ms(std::string_view value) {
  bool numeric = !value.empty();
  std::uint64_t numeric_value = 0;
  for (const unsigned char byte : value) {
    if (!std::isdigit(byte)) {
      numeric = false;
      break;
    }
    if (numeric_value > (std::numeric_limits<std::uint64_t>::max() - (byte - '0')) / 10U) {
      return 0;
    }
    numeric_value = numeric_value * 10U + (byte - '0');
  }
  if (numeric) {
    return numeric_value;
  }

  // RFC3339 UTC and numeric-offset timestamps are sufficient for the compact
  // adapter model.  Invalid timestamps sort at zero rather than consulting a
  // device clock or throwing from a reducer.
  if (value.size() < 20 || !separator(value, 4, '-') || !separator(value, 7, '-') ||
      (value[10] != 'T' && value[10] != 't') || !separator(value, 13, ':') ||
      !separator(value, 16, ':')) {
    return 0;
  }
  bool valid = true;
  const std::int64_t year = static_cast<std::int64_t>(decimal(value, 0, 4, valid));
  const unsigned month = static_cast<unsigned>(decimal(value, 5, 2, valid));
  const unsigned day = static_cast<unsigned>(decimal(value, 8, 2, valid));
  const unsigned hour = static_cast<unsigned>(decimal(value, 11, 2, valid));
  const unsigned minute = static_cast<unsigned>(decimal(value, 14, 2, valid));
  const unsigned second = static_cast<unsigned>(decimal(value, 17, 2, valid));
  if (!valid || month < 1U || month > 12U || day < 1U || day > 31U || hour > 23U ||
      minute > 59U || second > 60U) {
    return 0;
  }

  std::size_t cursor = 19;
  std::uint64_t milliseconds = 0;
  if (cursor < value.size() && value[cursor] == '.') {
    ++cursor;
    std::size_t digits = 0;
    while (cursor < value.size() && std::isdigit(static_cast<unsigned char>(value[cursor]))) {
      if (digits < 3U) {
        milliseconds = milliseconds * 10U + static_cast<unsigned>(value[cursor] - '0');
      }
      ++digits;
      ++cursor;
    }
    if (digits == 0U) {
      return 0;
    }
    while (digits < 3U) {
      milliseconds *= 10U;
      ++digits;
    }
  }

  std::int64_t offset_minutes = 0;
  if (cursor >= value.size()) {
    return 0;
  }
  if (value[cursor] == 'Z' || value[cursor] == 'z') {
    ++cursor;
  } else if (value[cursor] == '+' || value[cursor] == '-') {
    const bool negative = value[cursor] == '-';
    ++cursor;
    const auto offset_hour = decimal(value, cursor, 2, valid);
    cursor += 2;
    if (!separator(value, cursor, ':')) {
      return 0;
    }
    ++cursor;
    const auto offset_minute = decimal(value, cursor, 2, valid);
    cursor += 2;
    if (!valid || offset_hour > 23U || offset_minute > 59U) {
      return 0;
    }
    offset_minutes = static_cast<std::int64_t>(offset_hour * 60U + offset_minute);
    if (negative) {
      offset_minutes = -offset_minutes;
    }
  } else {
    return 0;
  }
  if (cursor != value.size()) {
    return 0;
  }

  const std::int64_t days = days_from_civil(year, month, day);
  const std::int64_t local_ms = days * 86'400'000LL + static_cast<std::int64_t>(hour) * 3'600'000LL +
                                static_cast<std::int64_t>(minute) * 60'000LL +
                                static_cast<std::int64_t>(second) * 1'000LL +
                                static_cast<std::int64_t>(milliseconds);
  const std::int64_t utc_ms = local_ms - offset_minutes * 60'000LL;
  return utc_ms < 0 ? 0U : static_cast<std::uint64_t>(utc_ms);
}

bool retained(const WorkItemRecord& item, std::uint64_t now_ms) {
  if (item.item.state != WorkItemState::Completed || now_ms <= item.authoritative_updated_ms) {
    return true;
  }
  return now_ms - item.authoritative_updated_ms <= kCompletionRetentionMs;
}

int work_item_priority(const WorkItemRecord& item, std::uint64_t now_ms) {
  if (!retained(item, now_ms)) {
    return 0;
  }
  switch (item.item.state) {
    case WorkItemState::NeedsInput:
      return 5;
    case WorkItemState::Failed:
    case WorkItemState::Blocked:
      return 4;
    case WorkItemState::Working:
      return 3;
    case WorkItemState::Completed:
      return 2;
    case WorkItemState::Idle:
      return 1;
  }
  return 0;
}

std::vector<WorkItemRecord> rank_work_items(const SnapshotModel& model, std::uint64_t now_ms) {
  std::vector<WorkItemRecord> ranked;
  ranked.reserve(model.work_items.size());
  for (const auto& item : model.work_items) {
    if (retained(item, now_ms)) {
      ranked.push_back(item);
    }
  }
  std::sort(ranked.begin(), ranked.end(), [now_ms](const WorkItemRecord& left,
                                                    const WorkItemRecord& right) {
    const int left_priority = work_item_priority(left, now_ms);
    const int right_priority = work_item_priority(right, now_ms);
    if (left_priority != right_priority) {
      return left_priority > right_priority;
    }
    if (left.authoritative_updated_ms != right.authoritative_updated_ms) {
      return left.authoritative_updated_ms > right.authoritative_updated_ms;
    }
    return left.item.thread_id.view() < right.item.thread_id.view();
  });
  return ranked;
}

std::vector<PendingRequest> rank_pending_requests(const SnapshotModel& model, std::uint64_t now_ms) {
  std::vector<PendingRequest> ranked = model.pending_requests;
  std::sort(ranked.begin(), ranked.end(), [&model, now_ms](const PendingRequest& left,
                                                            const PendingRequest& right) {
    const int left_priority = priority_for_pending(model, left, now_ms);
    const int right_priority = priority_for_pending(model, right, now_ms);
    if (left_priority != right_priority) {
      return left_priority > right_priority;
    }
    const auto left_created = parse_datetime_ms(left.created_at.view());
    const auto right_created = parse_datetime_ms(right.created_at.view());
    if (left_created != right_created) {
      return left_created < right_created;
    }
    return left.request_id.view() < right.request_id.view();
  });
  return ranked;
}

SnapshotModel project_snapshot(const Snapshot& snapshot, std::uint64_t now_ms,
                               std::span<const std::uint64_t> authoritative_updates) {
  SnapshotModel model;
  model.now_ms = now_ms;
  model.adapter_sequence = snapshot.adapter_sequence;
  model.connectivity = snapshot.connectivity;
  model.live_handshake = snapshot.connectivity.status == ConnectivityStatus::Connected;
  model.work_items.reserve(snapshot.work_items.count);
  for (std::size_t index = 0; index < snapshot.work_items.count; ++index) {
    WorkItemRecord record;
    record.item = snapshot.work_items[index];
    record.authoritative_updated_ms = index < authoritative_updates.size()
                                          ? authoritative_updates[index]
                                          : parse_datetime_ms(record.item.updated_at.view());
    model.work_items.push_back(record);
  }
  model.pending_requests.reserve(snapshot.pending_requests.count);
  for (std::size_t index = 0; index < snapshot.pending_requests.count; ++index) {
    model.pending_requests.push_back(snapshot.pending_requests[index]);
  }
  const auto ranked = rank_work_items(model, now_ms);
  if (!ranked.empty()) {
    model.automatic_focus_thread_id = ranked.front().item.thread_id;
  }
  return model;
}

}  // namespace t3::companion
