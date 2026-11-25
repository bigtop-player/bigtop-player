// EngineStepTests.cpp: what one GO does to sounds that are already running
// (changes, the everything-else rule, restart, fade out all), plus large
// blocks and non-finite gains.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

#include "TestSources.h"

using Catch::Approx;
using namespace bigtop;

TEST_CASE("Level change moves a running sound to an absolute gain") {
    Engine e(1000); ConstSource a(100000); e.post(startCmd(1, a)); render(e, 64);
    StepCommand c; c.numChanges = 1; c.changes[0] = {1, ChangeKind::Level, 0.25f, 1000}; e.post(c);
    auto out = render(e, 1100);
    CHECK(out[0] == 1.0f); CHECK(out[1000] == 0.25f); CHECK(out[1099] == 0.25f);
}
TEST_CASE("Fade-out ends the voice") {
    Engine e(1000); ConstSource a(100000); e.post(startCmd(1, a)); render(e, 64);
    StepCommand c; c.numChanges = 1; c.changes[0] = {1, ChangeKind::FadeOut, 0, 500}; e.post(c);
    auto out = render(e, 600);
    CHECK(out[250] == Approx(0.70711f).margin(1e-3)); CHECK(out[500] == 0.0f);
    CHECK(e.snapshot().numVoices == 0);
}
// A true cut would click, so a FadeOut of 0 frames takes the declick time (10 frames at 1 kHz).
TEST_CASE("A fade-out time of zero stops within the declick time") {
    Engine e(1000); ConstSource a(100000); e.post(startCmd(1, a)); render(e, 64);
    StepCommand c; c.numChanges = 1; c.changes[0] = {1, ChangeKind::FadeOut, 0, 0}; e.post(c);
    CHECK(render(e, 20)[10] == 0.0f);
}
TEST_CASE("Everything-else fades unlisted step sounds, keeps listed ones and pads") {
    Engine e(1000); ConstSource a(100000, 0.1f), b(100000, 0.2f), pad(100000, 0.4f), n(100000, 0.01f);
    e.post(startCmd(1, a)); e.post(startCmd(2, b)); e.post(startCmd(kPadKeyBase + 0, pad)); render(e, 64);
    auto c = startCmd(3, n); c.numChanges = 1; c.changes[0] = {2, ChangeKind::Keep, 0, 0};
    c.othersKind = ChangeKind::FadeOut; c.othersFrames = 100; e.post(c);
    auto out = render(e, 200);
    CHECK(out[150] == Approx(0.2f + 0.4f + 0.01f));          // voice 1 gone, 2 kept, pad kept, 3 playing
}
TEST_CASE("A change for a sound that is not playing does nothing") {
    Engine e(1000); ConstSource a(100000); e.post(startCmd(1, a)); render(e, 64);
    StepCommand c; c.numChanges = 1; c.changes[0] = {99, ChangeKind::FadeOut, 0, 0}; e.post(c);
    CHECK(render(e, 10)[5] == 1.0f); CHECK(e.snapshot().numVoices == 1);
}
TEST_CASE("Fade-out on a pending voice cancels it; fade-out-all cancels pending starts") {
    Engine e(1000); ConstSource a(100000);
    auto s = startCmd(1, a); s.start.delayFrames = 500; e.post(s); render(e, 64);
    e.post(FadeOutAllCommand{100});
    auto out = render(e, 1000);
    CHECK(*std::max_element(out.begin(), out.end()) == 0.0f);
}
TEST_CASE("Starting a step that is already playing restarts it with a short crossfade") {
    Engine e(1000); RampSource r(100000); e.post(startCmd(1, r)); render(e, 640);
    e.post(startCmd(1, r)); auto out = render(e, 64);
    CHECK(e.snapshot().numVoices == 1); CHECK(e.snapshot().voices[0].positionFrame == 64);
    CHECK(out[20] == Approx(20.0f).margin(0.5f));           // old voice gone after 10 frames at 1000 Hz
}
TEST_CASE("Second fade-out-all stops at once; pads keep playing") {
    Engine e(1000); ConstSource a(100000, 0.5f), pad(100000, 0.25f);
    e.post(startCmd(1, a)); e.post(startCmd(kPadKeyBase, pad)); render(e, 64);
    e.post(FadeOutAllCommand{2000}); render(e, 128);
    e.post(FadeOutAllCommand{2000}); auto out = render(e, 64);
    CHECK(out[30] == Approx(0.25f));
}

// The explicit FadeOut on a pending voice (not only fade-out-all) cancels it.
TEST_CASE("Fade-out change on a pending voice cancels it") {
    Engine e(1000); ConstSource a(100000);
    auto s = startCmd(1, a); s.start.delayFrames = 500; e.post(s); render(e, 64);
    StepCommand c; c.numChanges = 1; c.changes[0] = {1, ChangeKind::FadeOut, 0, 100}; e.post(c);
    auto out = render(e, 1000);
    CHECK(*std::max_element(out.begin(), out.end()) == 0.0f); CHECK(e.snapshot().numVoices == 0);
}
// Another GO during a start delay does not cancel the pending start: the
// others rule skips pending voices, which start as scheduled at full gain.
TEST_CASE("The others rule leaves a pending start alone") {
    Engine e(1000); ConstSource a(100000, 0.5f), b(100000, 0.25f);
    auto s = startCmd(1, a); s.start.delayFrames = 500; e.post(s); render(e, 64);
    auto n = startCmd(2, b); n.othersKind = ChangeKind::FadeOut; n.othersFrames = 100; e.post(n);
    auto out = render(e, 1000);
    CHECK(out[435] == 0.25f);                  // A still waiting (436 frames of delay left), B playing
    CHECK(out[436] == Approx(0.75f));          // A starts on its scheduled frame, full gain
    CHECK(out[999] == Approx(0.75f));
    CHECK(e.snapshot().numVoices == 2);
}
TEST_CASE("Level change on a pending voice sets the gain it starts with") {
    Engine e(1000); ConstSource a(100000);
    auto s = startCmd(1, a); s.start.delayFrames = 100; e.post(s); render(e, 64);
    StepCommand c; c.numChanges = 1; c.changes[0] = {1, ChangeKind::Level, 0.5f, 1000}; e.post(c);
    auto out = render(e, 100);
    CHECK(out[35] == 0.0f); CHECK(out[36] == 0.5f);
}
// Blocks above the engine's 512-frame internal split.
TEST_CASE("A 1500-frame block is sample-exact across the 512-frame pass boundary") {
    Engine e(1000); RampSource r(1000); ConstSource k(100000, 1000.0f);
    auto loop = startCmd(1, r); loop.start.startFrame = 100; loop.start.endFrame = 600; loop.start.loop = true;
    e.post(loop);
    auto late = startCmd(2, k); late.start.delayFrames = 1030; e.post(late);   // second pass is 512..1023
    auto out = render(e, 1500, 1500);
    CHECK(out[499] == 599.0f); CHECK(out[500] == 100.0f);
    CHECK(out[511] == 111.0f); CHECK(out[512] == 112.0f); CHECK(out[999] == 599.0f); CHECK(out[1000] == 100.0f);
    CHECK(out[1029] == 129.0f); CHECK(out[1030] == 1130.0f);
}
// A NaN/inf gain must never reach the mix.
TEST_CASE("A non-finite start gain is treated as silence") {
    Engine e(1000); ConstSource a(1000), b(1000, 0.5f);
    auto c = startCmd(1, a); c.start.gain = std::numeric_limits<float>::quiet_NaN(); e.post(c);
    auto d = startCmd(2, a); d.start.gain = std::numeric_limits<float>::infinity(); e.post(d);
    e.post(startCmd(3, b));
    CHECK(render(e, 10)[5] == 0.5f);
}
TEST_CASE("A non-finite Level change is ignored") {
    Engine e(1000); ConstSource a(100000); e.post(startCmd(1, a)); render(e, 64);
    StepCommand c; c.numChanges = 2;
    c.changes[0] = {1, ChangeKind::Level, std::numeric_limits<float>::quiet_NaN(), 0};
    c.changes[1] = {1, ChangeKind::Level, -std::numeric_limits<float>::infinity(), 0};
    e.post(c);
    auto out = render(e, 10);
    CHECK(out[0] == 1.0f); CHECK(out[9] == 1.0f);
}
// numChanges outside 0..kMaxChanges is clamped, never read past the array.
TEST_CASE("An out-of-range change count is clamped") {
    Engine e(1000); ConstSource a(100000); e.post(startCmd(1, a)); render(e, 64);
    StepCommand c; c.numChanges = 1000; c.changes[kMaxChanges - 1] = {1, ChangeKind::FadeOut, 0, 0}; e.post(c);
    CHECK(render(e, 20)[10] == 0.0f);                                       // faded out over the declick time
    e.post(startCmd(2, a)); render(e, 10);
    StepCommand neg; neg.numChanges = -5; e.post(neg);
    auto out = render(e, 10);
    CHECK(out[0] == 1.0f); CHECK(out[9] == 1.0f); CHECK(e.snapshot().numVoices == 1);
}
// Starting a pad is not a GO on the show, so it must not turn a
// second fade-out-all press back into a slow fresh fade.
TEST_CASE("Starting a pad keeps the fade-out-all hurry state") {
    Engine e(1000); ConstSource a(100000, 0.5f), pad(100000, 0.25f), pad2(100000, 0.125f);
    e.post(startCmd(1, a)); e.post(startCmd(kPadKeyBase, pad)); render(e, 64);
    e.post(FadeOutAllCommand{2000}); render(e, 128);
    e.post(startCmd(kPadKeyBase + 1, pad2));
    e.post(FadeOutAllCommand{2000}); auto out = render(e, 64);
    CHECK(out[30] == Approx(0.25f + 0.125f));
}
// Stopping a pad is not a GO either; the second fade-out-all
// must still stop the step sounds at once.
TEST_CASE("Stopping a pad keeps the fade-out-all hurry state") {
    Engine e(1000); ConstSource a(100000, 0.5f), pad(100000, 0.25f);
    e.post(startCmd(1, a)); e.post(startCmd(kPadKeyBase, pad)); render(e, 64);
    e.post(FadeOutAllCommand{2000}); render(e, 128);
    StepCommand stop; stop.numChanges = 1; stop.changes[0] = {kPadKeyBase, ChangeKind::FadeOut, 0.0f, 50};
    e.post(stop);
    e.post(FadeOutAllCommand{2000}); render(e, 64);
    CHECK(render(e, 64)[0] == 0.0f);
}
// A Level gain past +24 dB (faulty controller) is capped.
TEST_CASE("A huge Level change gain is capped at kMaxProgrammedGain") {
    Engine e(1000); ConstSource a(100000); e.post(startCmd(1, a)); render(e, 64);
    StepCommand c; c.numChanges = 1; c.changes[0] = {1, ChangeKind::Level, 1e6f, 0}; e.post(c);
    CHECK(render(e, 20)[15] == kMaxProgrammedGain);  // after the 10 ms declick
}
// The step list's NEXT row counts only voices that are not already on their way out.
TEST_CASE("fadingOut is set by a FadeOut change and fade out all, not by a Level change to silence") {
    Engine e(1000); ConstSource a(100000), b(100000), c(100000);
    e.post(startCmd(1, a)); e.post(startCmd(2, b)); e.post(startCmd(3, c)); render(e, 64);
    const auto state = [&](VoiceKey key) {
        const Snapshot s = e.snapshot();
        const auto* v = std::find_if(s.voices, s.voices + s.numVoices, [&](const VoiceState& x) { return x.key == key; });
        REQUIRE(v != s.voices + s.numVoices);
        return *v;
    };
    CHECK_FALSE(state(1).fadingOut); CHECK_FALSE(state(2).fadingOut); CHECK_FALSE(state(3).fadingOut);
    StepCommand s; s.numChanges = 2;
    s.changes[0] = {1, ChangeKind::FadeOut, 0, 1000}; s.changes[1] = {2, ChangeKind::Level, 0.0f, 100};
    e.post(s); render(e, 256);
    CHECK(state(1).fadingOut);
    CHECK(state(2).gain == 0.0f); CHECK_FALSE(state(2).fadingOut);  // silent, but kept alive
    CHECK_FALSE(state(3).fadingOut);
    e.post(FadeOutAllCommand{1000}); render(e, 64);
    CHECK(state(2).fadingOut); CHECK(state(3).fadingOut);
}
TEST_CASE("a pending voice is in the snapshot with fadingOut false") {
    Engine e(1000); ConstSource a(100000);
    auto s = startCmd(1, a); s.start.delayFrames = 500; e.post(s); render(e, 64);
    const Snapshot snap = e.snapshot();
    REQUIRE(snap.numVoices == 1);
    CHECK(snap.voices[0].pending); CHECK_FALSE(snap.voices[0].fadingOut);
}
