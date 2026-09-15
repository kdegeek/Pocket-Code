#pragma once

#include "envelope.hpp"

#include <cstddef>
#include <optional>
#include <vector>

namespace t3::companion {

enum class EffectKind {
  PersistCommand,
  SendCommand,
  ShowAppliedGlow,
  ShowErrorGlow,
  OfflineRejected,
  RequestVoiceInput,
  RestoreAmbient,
};

struct Effect {
  EffectKind kind = EffectKind::RestoreAmbient;
  std::optional<DeviceCommand> command;
  std::optional<Id> command_id;
  std::optional<ShortString> error;
};

[[nodiscard]] bool has_effect(const std::vector<Effect>& effects, EffectKind kind);

struct OutboxEntry {
  DeviceCommand command;
  CommandStatus status = CommandStatus::Received;
  bool applied_glow_emitted = false;
  bool error_glow_emitted = false;
};

struct OutboxTransition {
  bool accepted = false;
  std::vector<Effect> effects;
};

class CommandOutbox {
 public:
  [[nodiscard]] OutboxTransition enqueue(const DeviceCommand& command, bool live_handshake);
  [[nodiscard]] OutboxTransition apply_status(const Id& command_id, CommandStatus status);
  [[nodiscard]] OutboxTransition reconcile(const CommandStatuses& statuses);

  [[nodiscard]] std::size_t size() const { return entries_.size(); }
  [[nodiscard]] bool contains(const Id& command_id) const;
  [[nodiscard]] const std::vector<OutboxEntry>& entries() const { return entries_; }
  [[nodiscard]] std::optional<OutboxEntry> find(const Id& command_id) const;

 private:
  [[nodiscard]] OutboxEntry* find_mutable(const Id& command_id);
  std::vector<OutboxEntry> entries_;
};

}  // namespace t3::companion
