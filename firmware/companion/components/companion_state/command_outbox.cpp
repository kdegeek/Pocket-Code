#include "command_outbox.hpp"

namespace t3::companion {
namespace {

Effect command_effect(EffectKind kind, const DeviceCommand& command) {
  Effect effect;
  effect.kind = kind;
  DeviceCommand effect_command = command;
  // The durable pending-command journal stores only command ID/status. Keep
  // the declarative persistence effect honest even when a caller inspects or
  // serializes it: unconfirmed/final transcript text never enters that path.
  if (kind == EffectKind::PersistCommand) {
    effect_command.transcript = {};
  }
  effect.command = effect_command;
  effect.command_id = command.command_id;
  return effect;
}

bool terminal(CommandStatus status) {
  return status == CommandStatus::Applied || status == CommandStatus::Rejected;
}

int status_rank(CommandStatus status) {
  switch (status) {
    case CommandStatus::Received:
      return 0;
    case CommandStatus::Committed:
      return 1;
    case CommandStatus::Applied:
    case CommandStatus::Rejected:
      return 2;
  }
  return 0;
}

}  // namespace

bool has_effect(const std::vector<Effect>& effects, EffectKind kind) {
  for (const auto& effect : effects) {
    if (effect.kind == kind) {
      return true;
    }
  }
  return false;
}

OutboxTransition CommandOutbox::enqueue(const DeviceCommand& command, bool live_handshake) {
  OutboxTransition transition;
  if (!live_handshake) {
    Effect effect;
    effect.kind = EffectKind::OfflineRejected;
    effect.command_id = command.command_id;
    transition.effects.push_back(effect);
    return transition;
  }
  if (find(command.command_id).has_value()) {
    transition.accepted = true;
    return transition;
  }
  entries_.push_back(OutboxEntry{command});
  transition.accepted = true;
  transition.effects.push_back(command_effect(EffectKind::PersistCommand, command));
  transition.effects.push_back(command_effect(EffectKind::SendCommand, command));
  return transition;
}

OutboxTransition CommandOutbox::apply_status(const Id& command_id, CommandStatus status) {
  OutboxTransition transition;
  OutboxEntry* entry = find_mutable(command_id);
  if (entry == nullptr) {
    return transition;
  }
  transition.accepted = true;
  if (terminal(entry->status) || status_rank(status) < status_rank(entry->status)) {
    return transition;
  }
  if (entry->status == status) {
    return transition;
  }
  entry->status = status;
  if (status == CommandStatus::Applied && !entry->applied_glow_emitted) {
    entry->applied_glow_emitted = true;
    transition.effects.push_back(command_effect(EffectKind::ShowAppliedGlow, entry->command));
  } else if (status == CommandStatus::Rejected && !entry->error_glow_emitted) {
    entry->error_glow_emitted = true;
    transition.effects.push_back(command_effect(EffectKind::ShowErrorGlow, entry->command));
  }
  return transition;
}

OutboxTransition CommandOutbox::reconcile(const CommandStatuses& statuses) {
  OutboxTransition result;
  for (std::size_t index = 0; index < statuses.records.count; ++index) {
    const auto transition = apply_status(statuses.records[index].command_id,
                                         statuses.records[index].status);
    result.accepted = result.accepted || transition.accepted;
    result.effects.insert(result.effects.end(), transition.effects.begin(), transition.effects.end());
  }
  return result;
}

bool CommandOutbox::contains(const Id& command_id) const {
  return find(command_id).has_value();
}

std::optional<OutboxEntry> CommandOutbox::find(const Id& command_id) const {
  for (const auto& entry : entries_) {
    if (entry.command.command_id == command_id) {
      return entry;
    }
  }
  return std::nullopt;
}

OutboxEntry* CommandOutbox::find_mutable(const Id& command_id) {
  for (auto& entry : entries_) {
    if (entry.command.command_id == command_id) {
      return &entry;
    }
  }
  return nullptr;
}

}  // namespace t3::companion
