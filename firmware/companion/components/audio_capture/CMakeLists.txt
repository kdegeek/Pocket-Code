idf_component_register(
  SRCS
    "es7210_capture.cpp"
    "pcm_downmix.cpp"
    "encoder.cpp"
    "backpressure.cpp"
  INCLUDE_DIRS "include"
  REQUIRES companion_protocol board_waveshare
)
