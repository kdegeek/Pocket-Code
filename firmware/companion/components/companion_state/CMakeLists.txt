idf_component_register(
  SRCS
    "snapshot_model.cpp"
    "focus_queue.cpp"
    "command_outbox.cpp"
    "interaction_fsm.cpp"
  INCLUDE_DIRS "include"
  REQUIRES companion_protocol
)
