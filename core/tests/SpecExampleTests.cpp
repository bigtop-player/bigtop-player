// SpecExampleTests.cpp: the executable form of the design's worked example
// (spec section 5): music -> atmo, atmo down, music B layers on top, atmo out.
//
// How to read the timeline: the engine runs at 1 kHz, so one frame is one
// millisecond. Every source is a constant 1.0, so the mixed output at a frame
// equals the sum of the gains of the sounds playing then. The controller's
// clock (the argument of go()) is kept equal to the audio time rendered so
// far: each go() happens right after the previous render ended. Each check
// samples a frame late in its render, after the step's fades have finished.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "TestSources.h"

using Catch::Approx;
using namespace bigtop;

namespace {
// Engine at 1 kHz with the three 60 s sources of specExampleShow() loaded.
struct Fixture {
    Engine e{1000};
    MapProvider provider;
    ShowController c{e, provider};
    Fixture() { addSpecExampleSources(provider); }
};
}  // namespace

TEST_CASE_METHOD(Fixture, "Spec example: music to atmo, atmo down, music B layers, atmo out", "[spec]") {
    c.setShow(specExampleShow());

    c.go(0);  // Music A alone
    auto t0 = render(e, 1000);
    CHECK(t0[999] == Approx(1.0f));

    c.go(1);  // Atmo: A fades out over 5 s, Atmo fades in over 3 s
    auto t1 = render(e, 6000);
    CHECK(t1[5500] == Approx(1.0f));  // A gone, Atmo at 0 dB

    c.go(7);  // Atmo to -12 dB in 2 s
    auto t2 = render(e, 3000);
    CHECK(t2[2500] == Approx(float(dbToGain(-12))).margin(1e-3));

    c.go(10);  // Music B fades in over 5 s, Atmo to -15 dB in 3 s
    auto t3 = render(e, 6000);
    CHECK(t3[5500] == Approx(float(dbToGain(-15)) + 1.0f).margin(1e-3));  // Atmo -15 dB + Music B full

    c.go(16);  // Atmo fades out over 8 s, Music B kept
    auto t4 = render(e, 9000);
    CHECK(t4[8500] == Approx(1.0f));  // Atmo out, Music B kept

    CHECK(c.go(25) == GoResult::EndOfList);
}

// Selecting a later step never reconstructs earlier ones: Atmo was never
// started, so the step's change for it does nothing.
TEST_CASE_METHOD(Fixture, "Jumping into the list does not reconstruct running sounds", "[spec]") {
    c.setShow(specExampleShow());
    c.selectNext(3);  // step id 6: start Music B, Atmo to -15 dB
    c.go(0);
    auto out = render(e, 6000);
    CHECK(out[5500] == Approx(1.0f));  // only Music B
}
