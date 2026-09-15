idf_component_register(
  SRCS
    "credential_store.cpp"
    "device_identity.cpp"
    "pending_commands.cpp"
    "snapshot_store.cpp"
  INCLUDE_DIRS "include"
  REQUIRES companion_protocol
)
