#pragma once

namespace t3::board {

// Temporary, audio-only Task 2 qualification image.  The caller must not
// combine this with display, touch, button, network, persistence, or OTA
// startup paths; it uses only the maintained Waveshare audio constructors.
void run_audio_diagnostic();

// One automatic low-level tone correlation using the verified 1.75C MCLK16
// standard-stereo path.  No user interaction is required.
void run_stereo_tone_diagnostic();

}  // namespace t3::board
