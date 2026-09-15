idf_component_register(
  SRCS
    "reconnect.cpp"
    "http_client.cpp"
    "wifi_ws_client.cpp"
    "transport.cpp"
  INCLUDE_DIRS "include"
  REQUIRES companion_protocol companion_state persistence
)
