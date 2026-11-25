/// @file DemoAudio.h
/// @brief Invented audio for the screenshot demo show and the app tests: plain sine tones, so
///        nothing real is ever shipped or read.
#pragma once

#include <juce_core/juce_core.h>

namespace bigtop::app {

/// @brief Writes a 48 kHz stereo 24-bit FLAC holding one sine tone.
/// @param file The file to write; an existing file is overwritten from the start.
/// @param seconds Length in seconds (whole frames, rounded down).
/// @param level Peak amplitude, 0..1 (0.1 is about -20 dBFS).
/// @param hz Tone frequency in Hz.
/// @param swellSeconds 0 = a steady tone; above 0, the level swells from 15 % up to @p level and
///        back down every this many seconds (the demo show's waveforms get a visible shape).
/// @return True when the whole file was written.
/// @note Any thread; blocking (writes the file).
bool writeSineFlac(const juce::File& file, double seconds, float level, double hz = 440.0, double swellSeconds = 0.0);

}  // namespace bigtop::app
