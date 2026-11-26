// StatusTests.cpp: the status bar's CLIP light, driven by a stand-in for the
// output limiter's gain reduction.
#include <catch2/catch_test_macros.hpp>

#include <cmath>

#include "AppTestSupport.h"
#include "UiClock.h"

namespace bigtop::app {

TEST_CASE("0.3 dB of limiting leaves CLIP dark") {
    // A master peaking at 0 dBFS meets the -0.3 dBFS ceiling: inaudible, not a clip.
    TestApp app(threeSoundSteps());
    app.setGainReductionDb(0.3f);
    app.session().tick();
    REQUIRE_FALSE(app.session().status().clip);
}

TEST_CASE("1.5 dB of limiting lights CLIP") {
    TestApp app(threeSoundSteps());
    app.setGainReductionDb(1.5f);
    app.session().tick();
    REQUIRE(app.session().status().clip);
}

TEST_CASE("CLIP stays lit until tapped") {
    ManualClock clock;
    TestApp app(threeSoundSteps());
    app.setGainReductionDb(1.5f);
    app.session().tick();
    app.setGainReductionDb(0.0f);
    clock.advance(10000);  // far past the old 2 s hold
    app.session().tick();
    REQUIRE(app.session().status().clip);
    auto& clip = AppTestAccess::clipButton(app.main());
    tap(clip, clip.getLocalBounds().getCentre());
    REQUIRE_FALSE(app.session().status().clip);
}

TEST_CASE("a CLIP tap while the limiter still works lights it again on the next tick") {
    ManualClock clock;
    TestApp app(threeSoundSteps());
    app.setGainReductionDb(2.0f);
    app.session().tick();
    auto& clip = AppTestAccess::clipButton(app.main());
    tap(clip, clip.getLocalBounds().getCentre());
    REQUIRE_FALSE(app.session().status().clip);
    app.session().tick();
    REQUIRE(app.session().status().clip);
}

TEST_CASE("CLIP survives a mode change and is a 48 px touch target") {
    ManualClock clock;
    TestApp app(threeSoundSteps());
    app.setGainReductionDb(1.5f);
    app.session().tick();
    app.setGainReductionDb(0.0f);
    AppTestAccess::pickMode(app.main(), ShowMode::Show);
    app.session().tick();
    REQUIRE(app.session().status().clip);
    auto& clip = AppTestAccess::clipButton(app.main());
    app.tick();
    requireHittable(clip);
    CHECK(clip.getHeight() >= 48);
}

TEST_CASE("the level meter shows the output peaks and holds the highest for 1 s") {
    ManualClock clock;
    TestApp app(threeSoundSteps());
    app.setOutputPeaks({0.5f, 0.25f});
    app.tick();
    auto& meter = AppTestAccess::levelMeter(app.main());
    requireHittable(meter);
    CHECK(meter.shown().left == 0.5f);
    CHECK(meter.shown().right == 0.25f);
    app.setOutputPeaks({0.0f, 0.0f});
    clock.advance(500);
    app.tick();
    CHECK(meter.shown().left == 0.0f);
    CHECK(meter.held().left == 0.5f);   // the peak tick stays
    clock.advance(600);
    app.tick();
    CHECK(meter.held().left == 0.0f);   // after 1 s it falls to the current level
}

TEST_CASE("the level meter lights one 2 dB segment per step from -40 dBFS") {
    CHECK(LevelMeter::kSegments == 20);
    CHECK(LevelMeter::litSegments(0.0f) == 0);
    CHECK(LevelMeter::litSegments(0.005f) == 0);    // -46 dBFS: below the scale
    CHECK(LevelMeter::litSegments(0.1f) == 10);     // -20 dBFS: half way
    CHECK(LevelMeter::litSegments(0.45f) == 17);    // about -7 dBFS: a normal show level, not near full
    CHECK(LevelMeter::litSegments(1.0f) == 20);
    CHECK(LevelMeter::litSegments(2.0f) == 20);     // above 0 dBFS stays full
    CHECK(LevelMeter::litSegments(std::nanf("")) == 0);
}

TEST_CASE("the level meter is wide enough to read its scale") {
    TestApp app(threeSoundSteps());
    app.tick();
    CHECK(AppTestAccess::levelMeter(app.main()).getWidth() >= 240);
}

}  // namespace bigtop::app
