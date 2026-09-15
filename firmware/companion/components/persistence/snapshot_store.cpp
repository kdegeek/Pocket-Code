#include "snapshot_store.hpp"

#include <cmath>
#include <cstdint>
#include <limits>

namespace t3::companion {
namespace {

void put_u16(std::vector<std::uint8_t>& bytes, std::uint16_t value) {
  bytes.push_back(static_cast<std::uint8_t>(value & 0xffU));
  bytes.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xffU));
}

void put_u64(std::vector<std::uint8_t>& bytes, std::uint64_t value) {
  for (unsigned shift = 0; shift < 64U; shift += 8U) {
    bytes.push_back(static_cast<std::uint8_t>((value >> shift) & 0xffU));
  }
}

[[nodiscard]] std::uint16_t get_u16(std::span<const std::uint8_t> bytes, std::size_t offset) {
  return static_cast<std::uint16_t>(bytes[offset]) |
         static_cast<std::uint16_t>(bytes[offset + 1U] << 8U);
}

[[nodiscard]] std::uint64_t get_u64(std::span<const std::uint8_t> bytes, std::size_t offset) {
  std::uint64_t value = 0;
  for (unsigned shift = 0; shift < 64U; shift += 8U) {
    value |= static_cast<std::uint64_t>(bytes[offset + shift / 8U]) << shift;
  }
  return value;
}

[[nodiscard]] bool valid_text(std::string_view value, std::size_t max_bytes,
                              bool non_empty = false) {
  return value.size() <= max_bytes && (!non_empty || !value.empty());
}

void put_string(std::vector<std::uint8_t>& bytes, std::string_view value) {
  put_u16(bytes, static_cast<std::uint16_t>(value.size()));
  bytes.insert(bytes.end(), value.begin(), value.end());
}

[[nodiscard]] bool read_string(std::span<const std::uint8_t> bytes, std::size_t& cursor,
                               std::size_t max_bytes, bool non_empty, std::string& output) {
  if (cursor + 2U > bytes.size()) {
    return false;
  }
  const auto size = get_u16(bytes, cursor);
  cursor += 2U;
  if (size > max_bytes || (non_empty && size == 0U) || cursor + size > bytes.size()) {
    return false;
  }
  output.assign(reinterpret_cast<const char*>(bytes.data() + cursor), size);
  cursor += size;
  return true;
}

[[nodiscard]] bool copy_id(const Id& input, Id& output) {
  return output.assign(input.view()) && valid_text(input.view(), kMaxPersistedSnapshotTextBytes, true);
}

[[nodiscard]] bool copy_short(const ShortString& input, ShortString& output) {
  return output.assign(input.view(), false) &&
         valid_text(input.view(), kMaxPersistedSnapshotTextBytes);
}

[[nodiscard]] bool copy_date(const DateTime& input, DateTime& output) {
  return output.assign(input.view(), false) &&
         valid_text(input.view(), kMaxPersistedSnapshotTextBytes);
}

[[nodiscard]] bool read_id(std::span<const std::uint8_t> bytes, std::size_t& cursor, Id& output) {
  std::string value;
  if (!read_string(bytes, cursor, kMaxPersistedSnapshotTextBytes, true, value)) {
    return false;
  }
  return output.assign(value);
}

[[nodiscard]] bool read_short(std::span<const std::uint8_t> bytes, std::size_t& cursor,
                              ShortString& output, bool non_empty = false) {
  std::string value;
  if (!read_string(bytes, cursor, kMaxPersistedSnapshotTextBytes, non_empty, value)) {
    return false;
  }
  return output.assign(value, non_empty);
}

[[nodiscard]] bool read_date(std::span<const std::uint8_t> bytes, std::size_t& cursor,
                             DateTime& output) {
  std::string value;
  if (!read_string(bytes, cursor, kMaxPersistedSnapshotTextBytes, false, value)) {
    return false;
  }
  return output.assign(value, false);
}

}  // namespace

SnapshotStore::SnapshotStore(SlotStorage& storage) : SnapshotStore(&storage) {}

SnapshotStore::SnapshotStore(SlotStorage* storage) : storage_(storage) {
  if (storage_ != nullptr) {
    journal_.emplace(*storage_, kSnapshotJournalMaxBytes);
    (void)load();
  }
}

bool SnapshotStore::project(const Snapshot& snapshot, CompactSnapshot& output) {
  if (snapshot.work_items.count > kMaxPersistedWorkItems ||
      snapshot.usage.providers.count > kMaxPersistedProviders) {
    return false;
  }
  CompactSnapshot projected;
  projected.protocol_version = snapshot.protocol_version;
  projected.adapter_sequence = snapshot.adapter_sequence;
  projected.connectivity = snapshot.connectivity.status;
  if (snapshot.connectivity.last_seen_at.has_value()) {
    DateTime last_seen;
    if (!copy_date(*snapshot.connectivity.last_seen_at, last_seen)) {
      return false;
    }
    projected.last_seen_at = std::move(last_seen);
  }
  if (snapshot.automatic_focus_thread_id.has_value()) {
    Id focus;
    if (!copy_id(*snapshot.automatic_focus_thread_id, focus)) {
      return false;
    }
    projected.automatic_focus_thread_id = std::move(focus);
  }
  for (std::size_t index = 0; index < snapshot.work_items.count; ++index) {
    const auto& source = snapshot.work_items[index];
    CompactWorkItem item;
    if (!copy_id(source.environment_id, item.environment_id) ||
        !copy_id(source.project_id, item.project_id) || !copy_id(source.thread_id, item.thread_id) ||
        !copy_short(source.project_name, item.project_name) ||
        !copy_short(source.thread_name, item.thread_name) || !copy_date(source.updated_at, item.updated_at)) {
      return false;
    }
    if (source.turn_id.has_value()) {
      Id turn;
      if (!copy_id(*source.turn_id, turn)) {
        return false;
      }
      item.turn_id = std::move(turn);
    }
    item.provider = source.provider;
    item.state = source.state;
    if (source.activity.has_value()) {
      ShortString activity;
      if (!copy_short(*source.activity, activity)) {
        return false;
      }
      item.activity = std::move(activity);
    }
    projected.work_items.push_back(std::move(item));
  }
  for (std::size_t index = 0; index < snapshot.usage.providers.count; ++index) {
    const auto& source = snapshot.usage.providers[index];
    if (source.windows.count > kMaxPersistedWindows) {
      return false;
    }
    CompactUsageProvider provider;
    provider.provider = source.provider;
    provider.status = source.status;
    if (!copy_date(source.observed_at, provider.observed_at)) {
      return false;
    }
    for (std::size_t window_index = 0; window_index < source.windows.count; ++window_index) {
      const auto& source_window = source.windows[window_index];
      if (!std::isfinite(source_window.used_percent) || source_window.used_percent < 0.0 ||
          source_window.used_percent > 100.0 ||
          !valid_text(source_window.reset_at.view(), kMaxPersistedSnapshotTextBytes)) {
        return false;
      }
      RateWindow window = source_window;
      provider.windows.push_back(std::move(window));
    }
    projected.usage.providers.push_back(std::move(provider));
  }
  output = std::move(projected);
  return true;
}

std::vector<std::uint8_t> SnapshotStore::encode(const CompactSnapshot& snapshot) {
  if (snapshot.work_items.size() > kMaxPersistedWorkItems ||
      snapshot.usage.providers.size() > kMaxPersistedProviders) {
    return {};
  }
  std::vector<std::uint8_t> bytes;
  bytes.reserve(kSnapshotJournalMaxBytes);
  bytes.push_back(snapshot.protocol_version);
  put_u64(bytes, snapshot.adapter_sequence);
  bytes.push_back(static_cast<std::uint8_t>(snapshot.work_items.size()));
  for (const auto& item : snapshot.work_items) {
    if (!valid_text(item.environment_id.view(), kMaxPersistedSnapshotTextBytes, true) ||
        !valid_text(item.project_id.view(), kMaxPersistedSnapshotTextBytes, true) ||
        !valid_text(item.thread_id.view(), kMaxPersistedSnapshotTextBytes, true) ||
        !valid_text(item.project_name.view(), kMaxPersistedSnapshotTextBytes) ||
        !valid_text(item.thread_name.view(), kMaxPersistedSnapshotTextBytes) ||
        !valid_text(item.updated_at.view(), kMaxPersistedSnapshotTextBytes) ||
        (item.activity.has_value() &&
         !valid_text(item.activity->view(), kMaxPersistedSnapshotTextBytes))) {
      return {};
    }
    put_string(bytes, item.environment_id.view());
    put_string(bytes, item.project_id.view());
    put_string(bytes, item.thread_id.view());
    bytes.push_back(item.turn_id.has_value() ? 1U : 0U);
    if (item.turn_id.has_value()) {
      put_string(bytes, item.turn_id->view());
    }
    put_string(bytes, item.project_name.view());
    put_string(bytes, item.thread_name.view());
    bytes.push_back(item.provider.has_value() ? 1U : 0U);
    if (item.provider.has_value()) {
      bytes.push_back(static_cast<std::uint8_t>(*item.provider));
    }
    bytes.push_back(static_cast<std::uint8_t>(item.state));
    bytes.push_back(item.activity.has_value() ? 1U : 0U);
    if (item.activity.has_value()) {
      put_string(bytes, item.activity->view());
    }
    put_string(bytes, item.updated_at.view());
  }
  bytes.push_back(snapshot.automatic_focus_thread_id.has_value() ? 1U : 0U);
  if (snapshot.automatic_focus_thread_id.has_value()) {
    put_string(bytes, snapshot.automatic_focus_thread_id->view());
  }
  bytes.push_back(static_cast<std::uint8_t>(snapshot.usage.providers.size()));
  for (const auto& provider : snapshot.usage.providers) {
    if (!valid_text(provider.observed_at.view(), kMaxPersistedSnapshotTextBytes) ||
        provider.windows.size() > kMaxPersistedWindows) {
      return {};
    }
    bytes.push_back(static_cast<std::uint8_t>(provider.provider));
    bytes.push_back(static_cast<std::uint8_t>(provider.status));
    put_string(bytes, provider.observed_at.view());
    bytes.push_back(static_cast<std::uint8_t>(provider.windows.size()));
    for (const auto& window : provider.windows) {
      if (!std::isfinite(window.used_percent) || window.used_percent < 0.0 ||
          window.used_percent > 100.0 ||
          !valid_text(window.reset_at.view(), kMaxPersistedSnapshotTextBytes)) {
        return {};
      }
      const auto basis_points = static_cast<std::uint16_t>(window.used_percent * 100.0 + 0.5);
      bytes.push_back(static_cast<std::uint8_t>(window.period));
      put_u16(bytes, basis_points);
      put_string(bytes, window.reset_at.view());
    }
  }
  bytes.push_back(static_cast<std::uint8_t>(snapshot.connectivity));
  bytes.push_back(snapshot.last_seen_at.has_value() ? 1U : 0U);
  if (snapshot.last_seen_at.has_value()) {
    if (!valid_text(snapshot.last_seen_at->view(), kMaxPersistedSnapshotTextBytes)) {
      return {};
    }
    put_string(bytes, snapshot.last_seen_at->view());
  }
  return bytes.size() <= kSnapshotJournalMaxBytes ? bytes : std::vector<std::uint8_t>{};
}

bool SnapshotStore::decode(std::span<const std::uint8_t> bytes, CompactSnapshot& snapshot) {
  if (bytes.size() < 10U) {
    return false;
  }
  std::size_t cursor = 0;
  CompactSnapshot decoded;
  decoded.protocol_version = bytes[cursor++];
  decoded.adapter_sequence = get_u64(bytes, cursor);
  cursor += 8U;
  const auto work_count = bytes[cursor++];
  if (work_count > kMaxPersistedWorkItems) {
    return false;
  }
  for (std::size_t index = 0; index < work_count; ++index) {
    CompactWorkItem item;
    if (!read_id(bytes, cursor, item.environment_id) || !read_id(bytes, cursor, item.project_id) ||
        !read_id(bytes, cursor, item.thread_id) || cursor >= bytes.size()) {
      return false;
    }
    const bool has_turn = bytes[cursor++] != 0U;
    if (has_turn) {
      Id turn;
      if (!read_id(bytes, cursor, turn)) {
        return false;
      }
      item.turn_id = std::move(turn);
    }
    if (!read_short(bytes, cursor, item.project_name) ||
        !read_short(bytes, cursor, item.thread_name) || cursor >= bytes.size()) {
      return false;
    }
    const bool has_provider = bytes[cursor++] != 0U;
    if (has_provider) {
      if (cursor >= bytes.size() || bytes[cursor] > static_cast<std::uint8_t>(Provider::Xai)) {
        return false;
      }
      item.provider = static_cast<Provider>(bytes[cursor++]);
    }
    if (cursor + 2U > bytes.size() || bytes[cursor] > static_cast<std::uint8_t>(WorkItemState::Idle)) {
      return false;
    }
    item.state = static_cast<WorkItemState>(bytes[cursor++]);
    const bool has_activity = bytes[cursor++] != 0U;
    if (has_activity) {
      ShortString activity;
      if (!read_short(bytes, cursor, activity)) {
        return false;
      }
      item.activity = std::move(activity);
    }
    if (!read_date(bytes, cursor, item.updated_at)) {
      return false;
    }
    decoded.work_items.push_back(std::move(item));
  }
  if (cursor >= bytes.size()) {
    return false;
  }
  const bool has_focus = bytes[cursor++] != 0U;
  if (has_focus) {
    Id focus;
    if (!read_id(bytes, cursor, focus)) {
      return false;
    }
    decoded.automatic_focus_thread_id = std::move(focus);
  }
  if (cursor >= bytes.size()) {
    return false;
  }
  const auto provider_count = bytes[cursor++];
  if (provider_count > kMaxPersistedProviders) {
    return false;
  }
  for (std::size_t index = 0; index < provider_count; ++index) {
    if (cursor + 2U > bytes.size() || bytes[cursor] > static_cast<std::uint8_t>(Provider::Xai) ||
        bytes[cursor + 1U] > static_cast<std::uint8_t>(UsageStatus::Error)) {
      return false;
    }
    CompactUsageProvider provider;
    provider.provider = static_cast<Provider>(bytes[cursor++]);
    provider.status = static_cast<UsageStatus>(bytes[cursor++]);
    if (!read_date(bytes, cursor, provider.observed_at) || cursor >= bytes.size()) {
      return false;
    }
    const auto window_count = bytes[cursor++];
    if (window_count > kMaxPersistedWindows) {
      return false;
    }
    for (std::size_t window_index = 0; window_index < window_count; ++window_index) {
      if (cursor + 3U > bytes.size() || bytes[cursor] > 1U) {
        return false;
      }
      RateWindow window;
      window.period = static_cast<RateWindow::Period>(bytes[cursor++]);
      const auto basis_points = get_u16(bytes, cursor);
      cursor += 2U;
      window.used_percent = static_cast<double>(basis_points) / 100.0;
      if (!read_date(bytes, cursor, window.reset_at)) {
        return false;
      }
      provider.windows.push_back(std::move(window));
    }
    decoded.usage.providers.push_back(std::move(provider));
  }
  if (cursor + 2U > bytes.size() || bytes[cursor] > static_cast<std::uint8_t>(ConnectivityStatus::Offline)) {
    return false;
  }
  decoded.connectivity = static_cast<ConnectivityStatus>(bytes[cursor++]);
  const bool has_last_seen = bytes[cursor++] != 0U;
  if (has_last_seen) {
    DateTime last_seen;
    if (!read_date(bytes, cursor, last_seen)) {
      return false;
    }
    decoded.last_seen_at = std::move(last_seen);
  }
  if (cursor != bytes.size()) {
    return false;
  }
  snapshot = std::move(decoded);
  return true;
}

bool SnapshotStore::load() {
  if (!journal_.has_value()) {
    return last_valid_.has_value();
  }
  std::vector<std::uint8_t> bytes;
  if (!journal_->load(bytes)) {
    last_valid_.reset();
    return false;
  }
  CompactSnapshot decoded;
  if (!decode(bytes, decoded)) {
    last_valid_.reset();
    return false;
  }
  last_valid_ = std::move(decoded);
  return true;
}

bool SnapshotStore::save(const Snapshot& snapshot) {
  CompactSnapshot projected;
  return project(snapshot, projected) && save(projected);
}

bool SnapshotStore::save(const CompactSnapshot& snapshot) {
  const auto bytes = encode(snapshot);
  if (bytes.empty()) {
    return false;
  }
  if (journal_.has_value() && !journal_->commit(bytes)) {
    return false;
  }
  last_valid_ = snapshot;
  last_valid_->pending_requests.clear();
  return true;
}

}  // namespace t3::companion
