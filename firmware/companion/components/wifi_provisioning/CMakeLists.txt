idf_component_register(
  SRCS
    "network_store.cpp"
    "softap_portal.cpp"
    "roaming.cpp"
    "gateway_discovery.cpp"
  INCLUDE_DIRS "include"
  REQUIRES persistence companion_protocol
)
