#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>

#include "TestSources.h"

using Catch::Approx;
using namespace bigtop;

TEST_CASE("A started sound plays sample-exactly from its start point and then stops") {
    Engine e(1000); RampSource src(300);
    auto c = startCmd(1, src); c.start.startFrame = 100; e.post(c);
    auto out = render(e, 400);
    // A start inside the file rises over the 10-frame declick; from then on it is sample-exact.
    CHECK(out[0] < 100.0f); CHECK(out[10] == 110.0f); CHECK(out[199] == 299.0f); CHECK(out[200] == 0.0f);
    CHECK(e.snapshot().numVoices == 0);
}
TEST_CASE("Start delay holds the sound back and shows it as pending") {
    Engine e(1000); ConstSource src(1000);
    auto c = startCmd(1, src); c.start.delayFrames = 250; e.post(c);
    auto out = render(e, 128);
    CHECK(e.snapshot().voices[0].pending);
    auto rest = render(e, 372); out.insert(out.end(), rest.begin(), rest.end());
    CHECK(out[249] == 0.0f); CHECK(out[250] == 1.0f);
}
// Fade-in starts at 0, is at sin(pi/4) halfway, and lands exactly on the gain.
TEST_CASE("Fade-in follows the fader shape") {
    Engine e(1000); ConstSource src(5000);
    auto c = startCmd(1, src); c.start.fadeInFrames = 1000; e.post(c);
    auto out = render(e, 1200);
    CHECK(out[0] == 0.0f); CHECK(out[500] == Approx(0.70711f).margin(1e-3)); CHECK(out[1000] == 1.0f);
}
TEST_CASE("A looped sound wraps from end point to start point without a gap") {
    Engine e(1000); RampSource src(1000);
    auto c = startCmd(1, src); c.start.startFrame = 10; c.start.endFrame = 20; c.start.loop = true; e.post(c);
    auto out = render(e, 35);  // the first 10 frames are the declick rise of a start inside the file
    CHECK(out[19] == 19.0f); CHECK(out[20] == 10.0f); CHECK(out[29] == 19.0f); CHECK(out[30] == 10.0f);
}
TEST_CASE("Two sounds are summed") {
    Engine e(1000); ConstSource a(100, 0.25f), b(100, 0.5f);
    e.post(startCmd(1, a)); e.post(startCmd(2, b));
    CHECK(render(e, 10)[5] == Approx(0.75f));
}
TEST_CASE("A 33rd voice is refused and running sounds are untouched") {   // never steal a running sound
    Engine e(1000); ConstSource src(100000, 0.01f);
    for (VoiceKey k = 1; k <= 33; ++k) { e.post(startCmd(k, src)); render(e, 64); }
    CHECK(e.snapshot().numVoices == 32); CHECK(e.snapshot().droppedStarts == 1);
    CHECK(render(e, 64)[0] == Approx(0.32f));
}
TEST_CASE("post reports a full queue") {                                   // capacity 64; a full queue is reported, not blocked
    Engine e(1000); ConstSource src(10); int accepted = 0;
    for (int i = 0; i < 100; ++i) accepted += e.post(startCmd(1, src)) ? 1 : 0;
    CHECK(accepted == 64);
}
TEST_CASE("process with zero frames does nothing") {
    Engine e(1000); float l = 7, r = 7; e.process(&l, &r, 0); CHECK(l == 7.0f);
}
// Invalid starts are ignored quietly: no voice, no sound, not a dropped start.
TEST_CASE("A start with no source or an empty range is ignored") {
    Engine e(1000); ConstSource src(100);
    auto none = startCmd(1, src); none.start.source = nullptr; e.post(none);
    auto empty = startCmd(2, src); empty.start.startFrame = 50; empty.start.endFrame = 50; e.post(empty);
    auto pastEnd = startCmd(3, src); pastEnd.start.startFrame = 100; pastEnd.start.endFrame = 500; e.post(pastEnd);
    auto out = render(e, 64);
    CHECK(*std::max_element(out.begin(), out.end()) == 0.0f);
    CHECK(e.snapshot().numVoices == 0); CHECK(e.snapshot().droppedStarts == 0);
}
// A start gain past +24 dB (faulty controller) is capped.
TEST_CASE("A huge start gain is capped at kMaxProgrammedGain") {
    Engine e(1000); ConstSource src(1000);
    auto loud = startCmd(1, src); loud.start.gain = 1e6f; e.post(loud);
    CHECK(render(e, 10)[5] == kMaxProgrammedGain);
}
// An end point beyond the source is clamped to the source length.
TEST_CASE("An end point past the source length is clamped") {
    Engine e(1000); RampSource src(100);
    auto c = startCmd(1, src); c.start.startFrame = 80; c.start.endFrame = 1000; e.post(c);
    auto out = render(e, 64);  // past the 10-frame declick rise, then the file's own end (no end ramp)
    CHECK(out[19] == 99.0f); CHECK(out[20] == 0.0f); CHECK(e.snapshot().numVoices == 0);
}
// The snapshot is passed through raw atomic words; every field of the first
// and last voice plus the trailing counters must survive, which catches a
// wrong word count or memcpy size.
TEST_CASE("Snapshot carries every field of every voice") {
    Engine e(1000); RampSource a(1000), b(1000); ConstSource filler(100000, 0.0f);
    auto c1 = startCmd(5, a); c1.start.startFrame = 100; c1.start.gain = 0.5f; e.post(c1);
    auto c2 = startCmd(9, b); c2.start.startFrame = 300; c2.start.gain = 0.25f; c2.start.delayFrames = 200; e.post(c2);
    for (VoiceKey k = 100; k < 130; ++k) e.post(startCmd(k, filler));   // fills slots 2..31
    e.post(startCmd(200, filler));                                         // refused: no slot left
    render(e, 64);
    const Snapshot s = e.snapshot();
    CHECK(s.numVoices == 32); CHECK(s.droppedStarts == 1);
    CHECK(s.timecodeKey == 0); CHECK(s.timecodeFrame == 0);
    CHECK(s.voices[0].key == 5); CHECK_FALSE(s.voices[0].pending); CHECK(s.voices[0].positionFrame == 164);
    CHECK(s.voices[0].gain == 0.5f); CHECK(s.voices[0].trim == 1.0f);
    CHECK(s.voices[1].key == 9); CHECK(s.voices[1].pending); CHECK(s.voices[1].positionFrame == 300);
    CHECK(s.voices[1].gain == 0.25f); CHECK(s.voices[1].trim == 1.0f);
    CHECK(s.voices[31].key == 129); CHECK(s.voices[31].positionFrame == 64);
}
