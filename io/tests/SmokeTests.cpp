// SmokeTests.cpp: proves the io build is wired up: JUCE's audio formats are
// compiled in (MP3 needs JUCE_USE_MP3AUDIOFORMAT=1) and io's header is usable.
#include <catch2/catch_test_macros.hpp>

#include <juce_audio_formats/juce_audio_formats.h>

#include "bigtop/io/AudioOutput.h"

TEST_CASE("AudioFormatManager with WAV and MP3 registered knows both extensions") {
    juce::AudioFormatManager formats;
    formats.registerFormat(new juce::WavAudioFormat(), true);
    formats.registerFormat(new juce::MP3AudioFormat(), false);
    CHECK(formats.findFormatForFileExtension(".wav") != nullptr);
    CHECK(formats.findFormatForFileExtension(".mp3") != nullptr);
    CHECK(formats.findFormatForFileExtension(".flac") == nullptr);  // not registered here
}

TEST_CASE("The engine sample rate is 48 kHz") {
    STATIC_CHECK(bigtop::io::kEngineSampleRate == 48000.0);
}
