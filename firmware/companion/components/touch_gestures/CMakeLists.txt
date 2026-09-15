idf_component_register(
  SRCS "touch_reader.cpp" "gesture_recognizer.cpp" "card_physics.cpp"
  INCLUDE_DIRS "include"
  REQUIRES companion_protocol companion_state
)
