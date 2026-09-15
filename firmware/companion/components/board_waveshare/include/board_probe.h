#pragma once

namespace t3::board {

// Runs the temporary, non-destructive Task 2 physical qualification image.
// The caller must keep the board powered and connected while this function
// reports live display, touch, button, and microphone observations.
void run_probe();

// Runs one bounded audio tone/capture experiment.  The caller must keep the
// board connected while the diagnostic emits its before/during/after evidence.
// This path is intentionally separate from the full display/touch/button
// probe and uses only the maintained ES8311/ES7210 codec devices.
void run_tone_diagnostic();

// Runs one audio-only 1.75C 24 kHz four-slot TDM capture.  The caller must
// keep the board connected while the bounded idle window proves readiness.
void run_tdm_diagnostic();

}  // namespace t3::board
