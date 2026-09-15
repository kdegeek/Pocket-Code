idf_component_register(
  SRCS
    "ambient_view.cpp"
    "prompt_cards.cpp"
    "transcript_card.cpp"
    "ring_renderer.cpp"
    "overlays.cpp"
    "pixel_shift.cpp"
  INCLUDE_DIRS "include"
  REQUIRES companion_protocol companion_state
)
