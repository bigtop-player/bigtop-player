// WaveformTests.cpp: the peak summary of generated FLACs (a sine with a silent
// half, short files, broken files, cancel) and the column slices drawn from it.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <atomic>
#include <cmath>
#include <memory>
#include <vector>

#include <juce_audio_formats/juce_audio_formats.h>

#include "bigtop/io/Waveform.h"

using namespace bigtop::io;
using Catch::Matchers::WithinAbs;

namespace {

/// A unique temp directory, deleted with the object.
struct TempDir {
    juce::File dir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                         .getChildFile("bigtop_wave_" + juce::String(juce::Random::getSystemRandom().nextInt64()));
    TempDir() { dir.createDirectory(); }
    ~TempDir() { dir.deleteRecursively(); }
};

constexpr float kAmp = 0.5f;

/// Writes a 48 kHz, 24-bit stereo FLAC: a 440 Hz sine at kAmp for the first
/// @p soundFrames frames, then silence up to @p frames.
void writeSine(const juce::File& f, int frames, int soundFrames) {
    juce::AudioBuffer<float> buf(2, frames);
    buf.clear();
    for (int i = 0; i < soundFrames; ++i) {
        const auto s = kAmp * static_cast<float>(std::sin(2.0 * juce::MathConstants<double>::pi * 440.0 * i / 48000.0));
        buf.setSample(0, i, s);
        buf.setSample(1, i, s);
    }
    std::unique_ptr<juce::OutputStream> out = std::make_unique<juce::FileOutputStream>(f);
    auto w = juce::FlacAudioFormat().createWriterFor(
        out, juce::AudioFormatWriterOptions{}.withSampleRate(48000.0).withNumChannels(2).withBitsPerSample(24));
    REQUIRE(w != nullptr);
    REQUIRE(w->writeFromAudioSampleBuffer(buf, 0, buf.getNumSamples()));
}

}  // namespace

TEST_CASE("peaks of a sine with a silent second half") {
    TempDir tmp;
    const auto f = tmp.dir.getChildFile("sine.flac");
    constexpr int kFrames = 4 * 48000;
    writeSine(f, kFrames, kFrames / 2);
    const Peaks p = readPeaks(f);
    REQUIRE(p.frames == kFrames);
    REQUIRE(p.min.size() == static_cast<size_t>(kWaveformPoints));
    REQUIRE(p.max.size() == static_cast<size_t>(kWaveformPoints));
    // A point is 48 frames, less than one 440 Hz period (109 frames), so the loud half is
    // checked in groups of 4 points (192 frames), which always hold a full period.
    for (int i = 0; i + 4 <= kWaveformPoints / 2; i += 4) {
        float lo = 1.0f, hi = -1.0f;
        for (int j = i; j < i + 4; ++j) {
            lo = std::min(lo, p.min[static_cast<size_t>(j)]);
            hi = std::max(hi, p.max[static_cast<size_t>(j)]);
        }
        REQUIRE_THAT(hi, WithinAbs(kAmp, 0.01));
        REQUIRE_THAT(lo, WithinAbs(-kAmp, 0.01));
    }
    for (int i = kWaveformPoints / 2; i < kWaveformPoints; ++i) {
        REQUIRE_THAT(p.max[static_cast<size_t>(i)], WithinAbs(0.0, 1e-4));
        REQUIRE_THAT(p.min[static_cast<size_t>(i)], WithinAbs(0.0, 1e-4));
    }

    SECTION("peakRange over a sub-range picks that slice") {
        std::vector<float> lo, hi;
        peakRange(p, 0, kFrames, 2, lo, hi);  // loud half, silent half
        REQUIRE(lo.size() == 2);
        REQUIRE_THAT(hi[0], WithinAbs(kAmp, 0.01));
        REQUIRE_THAT(hi[1], WithinAbs(0.0, 1e-4));
        peakRange(p, kFrames / 2, kFrames, 10, lo, hi);  // only the silent half
        for (int c = 0; c < 10; ++c) REQUIRE_THAT(hi[static_cast<size_t>(c)], WithinAbs(0.0, 1e-4));
        peakRange(p, kFrames / 4, kFrames * 3 / 4, 4, lo, hi);  // loud, loud, silent, silent
        REQUIRE_THAT(hi[1], WithinAbs(kAmp, 0.01));
        REQUIRE_THAT(lo[1], WithinAbs(-kAmp, 0.01));
        REQUIRE_THAT(hi[2], WithinAbs(0.0, 1e-4));
        peakRange(p, kFrames / 2, kFrames * 3 / 2, 2, lo, hi);  // second slice is past the file
        REQUIRE(hi[1] == 0.0f);
        REQUIRE(lo[1] == 0.0f);
        peakRange(p, 0, kFrames, 0, lo, hi);
        REQUIRE(lo.empty());
    }
}

TEST_CASE("a file shorter than kWaveformPoints frames has one point per frame") {
    TempDir tmp;
    const auto f = tmp.dir.getChildFile("short.flac");
    writeSine(f, 1000, 1000);
    const Peaks p = readPeaks(f);
    REQUIRE(p.frames == 1000);
    REQUIRE(p.min.size() == 1000);
    REQUIRE(p.max.size() == 1000);
    REQUIRE(p.min[0] == p.max[0]);  // one frame per point: min = max = that sample
    std::vector<float> lo, hi;
    peakRange(p, 0, 1000, 3000, lo, hi);  // more columns than frames: every column still draws
    REQUIRE(lo.size() == 3000);
    REQUIRE_THAT(hi[1000], WithinAbs(p.max[333], 1e-6));
}

TEST_CASE("a missing or corrupt file, or a cancelled read, gives empty peaks") {
    TempDir tmp;
    const Peaks missing = readPeaks(tmp.dir.getChildFile("nothing.flac"));
    REQUIRE(missing.frames == 0);
    REQUIRE(missing.min.empty());
    const auto bad = tmp.dir.getChildFile("bad.flac");
    REQUIRE(bad.replaceWithText("not audio at all"));
    REQUIRE(readPeaks(bad).min.empty());

    const auto f = tmp.dir.getChildFile("sine.flac");
    writeSine(f, 48000, 48000);
    REQUIRE(!readPeaks(f).min.empty());
    std::atomic<bool> cancel{true};
    const Peaks cancelled = readPeaks(f, &cancel);
    REQUIRE(cancelled.frames == 0);
    REQUIRE(cancelled.max.empty());
}
