// EngineClickTests.cpp: click detection on the engine output.
// A DC source (ConstSource 0.5) goes through Engine::process at 48 kHz; on DC
// the output only moves because a gain moves, so a sample-to-sample step
// steeper than the legal fade of that case is a click.
//
// The legal curve is the Fader's equal-power ramp (Gain.h). Rising it is
// from + d*sin(a), falling to + d*cos(a), with a = (pi/2) * n / N over N
// frames. The slope per frame is d * (pi/2) / N * cos(a) (resp. sin(a)), at
// most (pi/2) * d / N: pi/2 times a straight ramp of the same length, at the
// start of a rise and the end of a fall. The discrete step d*sin(pi/(2N)) is
// never larger. allowedStep() is that bound on the source level, times 1.1.
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

#include "TestSources.h"

using namespace bigtop;

namespace {
constexpr double kRate = 48000.0;
constexpr float kValue = 0.5f;                       // the DC level every case plays
constexpr int64_t kLength = static_cast<int64_t>(60 * kRate);  // 60 s source
constexpr int kDeclickFrames = 480;                  // kDeclickSeconds at kRate
constexpr int kPauseFrames = 4800;                   // kPauseFadeSeconds at kRate
constexpr float kLow = 0.25f, kHigh = 0.75f;         // the two levels of the seek-between-levels case

/// Largest |x[n] - x[n-1]| over the vector (0 for fewer than two samples).
float maxStep(const std::vector<float>& left) {
    float m = 0.0f;
    for (size_t i = 1; i < left.size(); ++i) m = std::max(m, std::abs(left[i] - left[i - 1]));
    return m;
}

/// Steepest legal step for a gain change fromGain -> toGain over `seconds`
/// (never shorter than kDeclickSeconds) of a kValue DC source, with a 1.1 margin.
float allowedStep(float fromGain, float toGain, double seconds, double rate) {
    const double s = std::max(seconds, kDeclickSeconds);
    return static_cast<float>(1.1 * (3.14159265358979 / 2.0) * std::abs(toGain - fromGain) * kValue / (s * rate));
}

// Renders 64 frames, posts the command, renders `frames` more and returns both
// joined, so the step from the last sample before the event is checked too.
std::vector<float> around(Engine& e, const Command& c, int frames) {
    std::vector<float> out = render(e, 64);
    e.post(c);
    const std::vector<float> after = render(e, frames);
    out.insert(out.end(), after.begin(), after.end());
    return out;
}

// An engine at kRate playing ConstSource src as key 1, 0.1 s in.
void startPlaying(Engine& e, ConstSource& src) {
    e.post(startCmd(1, src));
    render(e, 4800);
}

StepCommand change(ChangeKind kind, float gain, int64_t frames) {
    StepCommand c;
    c.numChanges = 1;
    c.changes[0] = {1, kind, gain, frames};
    return c;
}
}  // namespace

TEST_CASE("maxStep sees a hard cut") {
    // What endVoice() without a declick would give: DC, then silence at once.
    std::vector<float> cut(64, kValue);
    cut.resize(128, 0.0f);
    CHECK(maxStep(cut) > allowedStep(1.0f, 0.0f, kDeclickSeconds, kRate));
}

// A seek is a crossfade: the old voice falls with cos while the copy rises
// with sin, both over kDeclickSeconds (N frames). On DC v the sum is
// v * (sin a + cos a), slope v * (pi/2N) * (cos a - sin a): at most v * (pi/2) / N,
// because the fall is flat exactly where the rise is steepest. So one legal
// slope bounds it, not the sum of two.
TEST_CASE("seek in the head does not click") {
    Engine e(kRate); ConstSource a(kLength, kValue); startPlaying(e, a);
    const auto out = around(e, SeekCommand{1, 48000}, 2 * kDeclickFrames);
    CHECK(maxStep(out) <= allowedStep(1.0f, 0.0f, kDeclickSeconds, kRate));
}
TEST_CASE("seek in the tail does not click") {
    // The tail is a ConstSource too: the engine path, not the streaming one.
    Engine e(kRate); ConstSource a(kLength, kValue); startPlaying(e, a);
    const auto out = around(e, SeekCommand{1, kLength - 9600}, 2 * kDeclickFrames);
    CHECK(maxStep(out) <= allowedStep(1.0f, 0.0f, kDeclickSeconds, kRate));
}
// On DC a too-short crossfade can still sum nearly flat, so here old and new
// positions carry different levels: frames before the middle are kLow, after
// it kHigh. The output is kLow * cos a + kHigh * sin a, slope
// (pi/2N) * (kHigh cos a - kLow sin a), at most (pi/2N) * sqrt(kLow^2 + kHigh^2)
// (the amplitude of that sum of sin and cos); any shorter crossfade, down to
// a jump, is steeper.
TEST_CASE("seek between two levels does not click") {
    struct TwoLevelSource : SoundSource {
        int64_t lengthFrames() const noexcept override { return kLength; }
        void read(int64_t start, float* left, float* right, int frames) noexcept override {
            for (int i = 0; i < frames; ++i) left[i] = right[i] = start + i < kLength / 2 ? kLow : kHigh;
        }
    } src;
    Engine e(kRate); e.post(startCmd(1, src)); render(e, 4800);
    const auto out = around(e, SeekCommand{1, kLength / 2 + 48000}, 2 * kDeclickFrames);
    CHECK(out.back() == kHigh);                         // it really moved to the other level
    const double limit = 1.1 * (3.14159265358979 / 2.0) * std::sqrt(kLow * kLow + kHigh * kHigh) / kDeclickFrames;
    CHECK(maxStep(out) <= static_cast<float>(limit));
}

TEST_CASE("pause and resume do not click") {
    Engine e(kRate); ConstSource a(kLength, kValue); startPlaying(e, a);
    const float limit = allowedStep(1.0f, 0.0f, kPauseFadeSeconds, kRate);
    const auto paused = around(e, PauseCommand{true}, kPauseFrames + 64);
    CHECK(paused.back() == 0.0f);                   // the pause really faded out
    CHECK(maxStep(paused) <= limit);
    const auto resumed = around(e, PauseCommand{false}, kPauseFrames + 64);
    CHECK(resumed.back() == kValue);                // and came back
    CHECK(maxStep(resumed) <= limit);
}

TEST_CASE("per-sound fade out does not click") {
    Engine e(kRate); ConstSource a(kLength, kValue); startPlaying(e, a);
    const auto out = around(e, change(ChangeKind::FadeOut, 0.0f, 48000), 48000 + 64);
    CHECK(out.back() == 0.0f);
    CHECK(maxStep(out) <= allowedStep(1.0f, 0.0f, 1.0, kRate));
}
TEST_CASE("second Fade tap stops within 10 ms without a click") {
    Engine e(kRate); ConstSource a(kLength, kValue); startPlaying(e, a);
    e.post(change(ChangeKind::FadeOut, 0.0f, 5 * 48000));  // first tap: a 5 s fade
    render(e, 48000);
    const float level = e.snapshot().voices[0].gain;       // where the fade is now
    REQUIRE(level > 0.1f);
    const auto out = around(e, change(ChangeKind::FadeOut, 0.0f, kDeclickFrames), kDeclickFrames + 64);
    CHECK(out[64 + kDeclickFrames] == 0.0f);               // silent 10 ms after the tap
    CHECK(maxStep(out) <= allowedStep(level, 0.0f, kDeclickSeconds, kRate));
}

TEST_CASE("Keep does not change the output") {
    // The same run twice, one with a Keep: trim +6 dB, then fold it in.
    auto run = [](bool keep) {
        Engine e(kRate); ConstSource a(kLength, kValue); startPlaying(e, a);
        e.post(TrimCommand{1, 2.0f});
        render(e, 960);                                    // trim ramp done
        if (keep) e.post(KeepTrimCommand{1});
        return render(e, 4800);
    };
    const auto kept = run(true), plain = run(false);
    REQUIRE(kept.size() == plain.size());
    float diff = 0.0f;
    for (size_t i = 0; i < kept.size(); ++i) diff = std::max(diff, std::abs(kept[i] - plain[i]));
    CHECK(diff < 1e-6f);
}

TEST_CASE("a 0 s Level change ramps over the declick") {
    const float target = GENERATE(0.5f, kMaxProgrammedGain);  // -6 dB and +24 dB
    Engine e(kRate); ConstSource a(kLength, kValue); startPlaying(e, a);
    const auto out = around(e, change(ChangeKind::Level, target, 0), kDeclickFrames + 64);
    CHECK(out.back() == kValue * target);                     // the level was reached
    CHECK(maxStep(out) <= allowedStep(1.0f, target, kDeclickSeconds, kRate));
}
TEST_CASE("a 0 s fade out ramps over the declick") {
    Engine e(kRate); ConstSource a(kLength, kValue); startPlaying(e, a);
    const auto out = around(e, change(ChangeKind::FadeOut, 0.0f, 0), kDeclickFrames + 64);
    CHECK(out.back() == 0.0f);
    CHECK(maxStep(out) <= allowedStep(1.0f, 0.0f, kDeclickSeconds, kRate));
}
TEST_CASE("Esc twice does not click") {
    Engine e(kRate); ConstSource a(kLength, kValue); startPlaying(e, a);
    const auto first = around(e, FadeOutAllCommand{3 * 48000}, 48000);  // first Esc: a 3 s fade
    CHECK(maxStep(first) <= allowedStep(1.0f, 0.0f, 3.0, kRate));
    const float level = e.snapshot().voices[0].gain;
    REQUIRE(level > 0.1f);
    // Second Esc: a declick from the current level, while the 3 s fade still
    // moves underneath; its slope is ~1/300 of the declick's, inside the margin.
    const auto second = around(e, FadeOutAllCommand{3 * 48000}, kDeclickFrames + 64);
    CHECK(second.back() == 0.0f);
    CHECK(maxStep(second) <= allowedStep(level, 0.0f, kDeclickSeconds, kRate));
}

// GO on the playing step: the old copy fades out over 1 s instead of being cut.
// The new copy plays silence here, so only the old copy's fade is measured (a
// new copy without fade-in cuts in by design, as any step with fade-in 0 does).
TEST_CASE("a restart fades the old copy out without a click") {
    Engine e(kRate); ConstSource a(kLength, kValue), silent(kLength, 0.0f); startPlaying(e, a);
    auto c = startCmd(1, silent); c.restartFrames = static_cast<int64_t>(kRate);
    const auto out = around(e, c, static_cast<int>(kRate) + 64);
    CHECK(out.back() == 0.0f);
    CHECK(maxStep(out) <= allowedStep(1.0f, 0.0f, 1.0, kRate));
}

// A start point inside the music with no fade-in, and an end point before the
// file's end, must not cut in or out: both get the declick (pads and steps alike).
TEST_CASE("a trimmed start without fade-in declicks in") {
    Engine e(kRate); ConstSource a(kLength, kValue);
    auto c = startCmd(1, a); c.start.startFrame = static_cast<int64_t>(kRate); c.start.endFrame = 2 * static_cast<int64_t>(kRate);
    const auto out = around(e, c, 2 * kDeclickFrames);
    CHECK(maxStep(out) <= allowedStep(1.0f, 0.0f, kDeclickSeconds, kRate));
}
TEST_CASE("a trimmed end declicks out") {
    Engine e(kRate); ConstSource a(kLength, kValue);
    auto c = startCmd(1, a); c.start.endFrame = static_cast<int64_t>(kRate) / 2;  // 0.5 s into a longer file
    e.post(c);
    const auto out = render(e, static_cast<int>(kRate) / 2 + 64);
    CHECK(out.back() == 0.0f);
    std::vector<float> tail(out.begin() + 64, out.end());  // past the start (a file start at 0 cuts in by design)
    CHECK(maxStep(tail) <= allowedStep(1.0f, 0.0f, kDeclickSeconds, kRate));
}
