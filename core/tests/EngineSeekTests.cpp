// EngineSeekTests.cpp: live operator controls on a playing sound (seek, trim)
// and which sound drives the timecode output.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <limits>

#include "TestSources.h"

using Catch::Approx;
using namespace bigtop;

TEST_CASE("Seek jumps the position with a 10 ms crossfade and no gap") {
    Engine e(1000); ConstSource a(100000); e.post(startCmd(1, a)); render(e, 64);
    e.post(SeekCommand{1, 50000}); auto out = render(e, 64);
    for (float v : out) { CHECK(v >= 0.999f); CHECK(v <= 1.4143f); }  // identical signals: equal-power join peaks at sqrt(2), never dips
    for (int i = 20; i < 64; ++i) CHECK(out[i] == 1.0f);              // join is over after 10 frames
    CHECK(e.snapshot().numVoices == 1); CHECK(e.snapshot().voices[0].positionFrame == 50064);
}
TEST_CASE("Seek lands on the requested frame") {
    Engine e(1000); RampSource r(100000); e.post(startCmd(1, r)); render(e, 64);
    e.post(SeekCommand{1, 5000}); auto out = render(e, 64);
    CHECK(out[40] == Approx(5040.0f));
}
TEST_CASE("Seek keeps a running fade and the trim") {
    Engine e(1000); ConstSource a(100000); e.post(startCmd(1, a)); render(e, 64);
    e.post(TrimCommand{1, 0.5f}); render(e, 64);
    StepCommand c; c.numChanges = 1; c.changes[0] = {1, ChangeKind::Level, 0.5f, 0}; e.post(c); render(e, 64);
    e.post(SeekCommand{1, 9000}); auto out = render(e, 64);
    CHECK(out[40] == Approx(0.25f));
}
TEST_CASE("Seek on an unknown key does nothing") {
    Engine e(1000); e.post(SeekCommand{5, 10}); CHECK(render(e, 10)[0] == 0.0f);
}
TEST_CASE("Newest timecode sound drives; no fallback when it ends") {
    Engine e(1000); ConstSource longA(100000), shortB(200);
    auto a = startCmd(1, longA); a.start.drivesTimecode = true; e.post(a); render(e, 64);
    CHECK(e.snapshot().timecodeKey == 1); CHECK(e.snapshot().timecodeFrame == 64);
    auto b = startCmd(2, shortB); b.start.drivesTimecode = true; e.post(b); render(e, 64);
    CHECK(e.snapshot().timecodeKey == 2);
    render(e, 256); CHECK(e.snapshot().timecodeKey == 0);
}
TEST_CASE("Timecode position follows a seek") {
    Engine e(1000); ConstSource a(100000);
    auto c = startCmd(1, a); c.start.drivesTimecode = true; e.post(c); render(e, 64);
    e.post(SeekCommand{1, 30000}); render(e, 64);
    CHECK(e.snapshot().timecodeFrame == 30064);
}

// Fail-safe like restart: with no free slot the seek is dropped and the sound plays on.
TEST_CASE("Seek with all voices busy is dropped and counted") {
    Engine e(1000); RampSource r(100000);
    for (VoiceKey k = 1; k <= static_cast<VoiceKey>(kMaxVoices); ++k) e.post(startCmd(k, r));
    render(e, 64);
    e.post(SeekCommand{1, 5000}); render(e, 64);
    const Snapshot s = e.snapshot();
    CHECK(s.droppedStarts == 1); CHECK(s.voices[0].key == 1); CHECK(s.voices[0].positionFrame == 128);
}
TEST_CASE("Seek target is clamped into the start/end range") {
    Engine e(1000); RampSource r(100000);
    auto c = startCmd(1, r); c.start.startFrame = 100; c.start.endFrame = 1000; c.start.loop = true; e.post(c); render(e, 64);
    e.post(SeekCommand{1, -50}); auto out = render(e, 64);
    CHECK(out[40] == Approx(140.0f));
    e.post(SeekCommand{1, 5000}); render(e, 1);
    CHECK(e.snapshot().voices[0].positionFrame == 100);  // landed on 999, played it, wrapped to the loop start
}
TEST_CASE("Trim is smoothed over the declick time and reported") {
    Engine e(1000); ConstSource a(100000); e.post(startCmd(1, a)); render(e, 64);
    e.post(TrimCommand{1, 0.5f}); auto out = render(e, 64);
    CHECK(out[0] == 1.0f); CHECK(out[5] < 1.0f); CHECK(out[5] > 0.5f); CHECK(out[10] == 0.5f);
    CHECK(e.snapshot().voices[0].trim == 0.5f);
}
TEST_CASE("A negative or non-finite trim is ignored") {
    Engine e(1000); ConstSource a(100000); e.post(startCmd(1, a)); render(e, 64);
    e.post(TrimCommand{1, -0.5f}); e.post(TrimCommand{1, std::numeric_limits<float>::quiet_NaN()});
    e.post(TrimCommand{1, std::numeric_limits<float>::infinity()});
    CHECK(render(e, 64)[30] == 1.0f); CHECK(e.snapshot().voices[0].trim == 1.0f);
}
TEST_CASE("A restarted timecode step keeps the role; a restart without it drops the role") {
    Engine e(1000); ConstSource a(100000);
    auto c = startCmd(1, a); c.start.drivesTimecode = true; e.post(c); render(e, 640);
    e.post(c); render(e, 64);
    CHECK(e.snapshot().timecodeKey == 1); CHECK(e.snapshot().timecodeFrame == 64);
    e.post(startCmd(1, a)); render(e, 64);
    CHECK(e.snapshot().timecodeKey == 0);
}
// A pending timecode sound takes the role only when its delay is over; until
// then the sound already playing keeps driving (lighting cues must not fire early).
TEST_CASE("A pending timecode sound drives only once it starts to sound") {
    Engine e(1000); ConstSource a(100000), b(100000);
    auto c = startCmd(1, a); c.start.drivesTimecode = true; e.post(c); render(e, 64);
    auto d = startCmd(2, b); d.start.drivesTimecode = true; d.start.startFrame = 700; d.start.delayFrames = 500; e.post(d);
    render(e, 128);
    CHECK(e.snapshot().timecodeKey == 1); CHECK(e.snapshot().timecodeFrame == 192);
    render(e, 472);                                      // 600 frames after the post: B has played 100
    CHECK(e.snapshot().timecodeKey == 2); CHECK(e.snapshot().timecodeFrame == 800);
}
// Cancelling a pending timecode sound leaves the current driver in place.
TEST_CASE("Cancelling a pending timecode sound keeps the driver") {
    Engine e(1000); ConstSource a(100000), b(100000);
    auto c = startCmd(1, a); c.start.drivesTimecode = true; e.post(c); render(e, 64);
    auto d = startCmd(2, b); d.start.drivesTimecode = true; d.start.delayFrames = 500; e.post(d); render(e, 64);
    StepCommand f; f.numChanges = 1; f.changes[0] = {2, ChangeKind::FadeOut, 0, 100}; e.post(f);
    render(e, 640);
    CHECK(e.snapshot().timecodeKey == 1); CHECK(e.snapshot().timecodeFrame == 768);
}
TEST_CASE("Fade-out-all hurry stop of the timecode sound clears the role") {
    Engine e(1000); ConstSource a(100000), b(100000);
    auto c = startCmd(1, a); c.start.drivesTimecode = true; e.post(c);
    auto d = startCmd(2, b); d.start.drivesTimecode = true; e.post(d); render(e, 64);
    e.post(FadeOutAllCommand{2000}); render(e, 64);
    CHECK(e.snapshot().timecodeKey == 2);  // still fading: still drives
    e.post(FadeOutAllCommand{2000}); render(e, 64);
    CHECK(e.snapshot().timecodeKey == 0);
}

// A burst of seeks in one block repositions the fresh copy instead of filling
// every voice slot, so a GO in the same block still starts.
TEST_CASE("A seek burst in one block uses one copy and leaves slots free") {
    Engine e(1000); ConstSource a(100000), b(100000); e.post(startCmd(1, a)); render(e, 64);
    for (int i = 0; i < 40; ++i) e.post(SeekCommand{1, 1000 + i * 100});
    e.post(startCmd(2, b));
    render(e, 64);
    const Snapshot s = e.snapshot();
    CHECK(s.numVoices == 2);  // the seek copy of key 1 plus the new start, nothing else
    CHECK(s.droppedStarts == 0);
    CHECK(s.voices[0].key == 1); CHECK(s.voices[0].positionFrame == 4900 + 64);
    CHECK(s.voices[1].key == 2);
}
TEST_CASE("Two seeks in one block land on the second target") {
    Engine e(1000); RampSource r(100000); e.post(startCmd(1, r)); render(e, 64);
    e.post(SeekCommand{1, 5000}); e.post(SeekCommand{1, 7000}); auto out = render(e, 64);
    CHECK(out[40] == Approx(7040.0f));
}
// A faulty controller sending a huge trim must not blow up the PA.
TEST_CASE("Trim is capped at +12 dB") {
    Engine e(1000); ConstSource a(100000, 0.5f); e.post(startCmd(1, a)); render(e, 64);
    e.post(TrimCommand{1, 1e30f}); auto out = render(e, 64);
    CHECK(out[30] == Approx(kMaxTrimGain * 0.5f)); CHECK(e.snapshot().voices[0].trim == kMaxTrimGain);
}
// The seek copy carries a running FadeOut: same curve, ends on the same block.
TEST_CASE("Seek mid fade-out keeps the fade curve and ends with it") {
    Engine e(1000), ref(1000); ConstSource a(100000);
    StepCommand f; f.numChanges = 1; f.changes[0] = {1, ChangeKind::FadeOut, 0, 1000};
    for (Engine* x : {&e, &ref}) { x->post(startCmd(1, a)); render(*x, 64); x->post(f); render(*x, 300); }
    e.post(SeekCommand{1, 20000});
    auto out = render(e, 64), want = render(ref, 64);
    CHECK(out[30] == want[30]); CHECK(out[63] == want[63]);
    for (int block = 0; block < 20; ++block) {
        CHECK(e.snapshot().numVoices == ref.snapshot().numVoices);
        render(e, 64); render(ref, 64);
    }
    CHECK(ref.snapshot().numVoices == 0); CHECK(e.snapshot().numVoices == 0);
}

TEST_CASE("Snapshot carries the driver's timecode start") {
    Engine e(1000); ConstSource a(100000);
    auto c = startCmd(1, a); c.start.drivesTimecode = true; c.start.timecodeStart = Timecode{1,2,3,4}; e.post(c); render(e, 64);
    CHECK(e.snapshot().timecodeStart == Timecode{1,2,3,4});
    CHECK(snapshotTimecode(e.snapshot(), 1000, 25) == Timecode{1,2,3,5});   // 64 frames = 0.064 s = 1 frame at 25 fps
    CHECK_FALSE(snapshotTimecode(e.snapshot(), 1000, 23));
}
TEST_CASE("framesProcessed counts every rendered frame") {
    Engine e(1000); render(e, 100, 30); CHECK(e.snapshot().framesProcessed == 100);
}
