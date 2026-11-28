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
    CHECK(meter.shown().left < 0.5f);   // falling, not jumping (see the decay test)
    CHECK(meter.held().left == 0.5f);   // the peak tick stays
    clock.advance(600);
    app.tick();
    CHECK(meter.held().left == meter.shown().left);  // after 1 s it falls to the current level
}

TEST_CASE("The level meter's bars fall at about 20 dB/s") {
    ManualClock clock;
    TestApp app(threeSoundSteps());
    app.setOutputPeaks({0.5f, 0.5f});  // about -6 dBFS
    app.tick();
    app.setOutputPeaks({0.001f, 0.001f});  // -60 dBFS
    for (int i = 0; i < 15; ++i) {  // 0.5 s at the UI rate
        clock.advance(33);
        app.tick();
    }
    const float db = 20.0f * std::log10(AppTestAccess::levelMeter(app.main()).shown().left);
    CHECK(db < -14.0f);
    CHECK(db > -18.0f);
}

TEST_CASE("CLIP goes dark when another show opens") {
    const ManualClock clock;
    TestApp app(threeSoundSteps());
    app.setGainReductionDb(1.5f);
    app.session().tick();
    REQUIRE(app.session().status().clip);
    app.setGainReductionDb(0.0f);
    std::string error;
    TempDir other;
    REQUIRE(app.session().createNew(other.dir.getChildFile("Other show"), error));
    CHECK_FALSE(app.session().status().clip);
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

namespace bigtop::app {

TEST_CASE("An alert shows over the step list and leaves after 8 s") {
    ManualClock clock;
    TestApp app(threeSoundSteps());
    app.session().alert(AlertKind::MissingFile, "Step 2 'Song B': file missing");
    app.tick();
    auto& banner = AppTestAccess::alertBanner(app.main());
    REQUIRE(banner.isVisible());
    CHECK(banner.text() == "Step 2 'Song B': file missing");
    auto& main = app.main();
    const auto at = [&](juce::Component& c) { return main.getLocalArea(c.getParentComponent(), c.getBounds()); };
    CHECK(at(AppTestAccess::steps(main)).contains(at(banner)));
    CHECK_FALSE(at(banner).intersects(at(AppTestAccess::goButton(main))));
    CHECK_FALSE(at(banner).intersects(at(AppTestAccess::modeBar(main))));
    clock.advance(ShowSession::kAlertMs + 1);
    app.tick();
    CHECK_FALSE(banner.isVisible());
}

TEST_CASE("A second alert replaces the text and counts the first") {
    ManualClock clock;
    TestApp app(threeSoundSteps());
    app.session().alert(AlertKind::MissingFile, "first");
    app.session().alert(AlertKind::ImportFailed, "second");
    app.tick();
    auto& banner = AppTestAccess::alertBanner(app.main());
    CHECK(banner.text() == "second");
    CHECK(banner.hint() == juce::String::fromUTF8("+1 more \xe2\x80\x93 Show check"));
}

TEST_CASE("Space still fires GO while an alert shows") {
    ManualClock clock;
    TestApp app(threeSoundSteps());
    app.session().alert(AlertKind::MissingFile, "x");
    app.tick();
    REQUIRE(app.main().keyPressed(juce::KeyPress(juce::KeyPress::spaceKey)));
    CHECK(app.session().controller().nextIndex() == 1);
    CHECK_FALSE(AppTestAccess::alertBanner(app.main()).getWantsKeyboardFocus());
}

TEST_CASE("Tapping the alert dismisses it") {
    ManualClock clock;
    TestApp app(threeSoundSteps());
    app.session().alert(AlertKind::MissingFile, "x");
    app.tick();
    auto& banner = AppTestAccess::alertBanner(app.main());
    tap(banner, banner.getLocalBounds().getCentre());
    app.tick();
    CHECK_FALSE(banner.isVisible());
    CHECK_FALSE(app.session().currentAlert().has_value());
}

}  // namespace bigtop::app

namespace bigtop::app {

TEST_CASE("Show check lists a missing file") {
    const ManualClock clock;
    Show show = threeSoundSteps();
    show.files.pop_back();
    show.steps[2].sound->file = 99;  // Song B names a file the show does not have
    TestApp app(show);
    app.waitHeads();
    const auto lines = app.session().checkReport();
    CHECK(std::find(lines.begin(), lines.end(), juce::String("Step 3 'Song B': file missing")) != lines.end());
    CHECK(std::none_of(lines.begin(), lines.end(), [](const juce::String& l) { return l.startsWith("Step 1"); }));
}

TEST_CASE("Show check says All clear when nothing is wrong") {
    ShowCheckPanel panel;
    panel.setLines({});
    CHECK(panel.shownText() == "All clear");
    panel.setLines({"Step 3 'Song B': file missing"});
    CHECK(panel.shownText() == "Step 3 'Song B': file missing");
}

TEST_CASE("Show check sits next to Close in Show settings, not in the status bar") {
    const ManualClock clock;
    TestApp app(threeSoundSteps());
    CHECK(AppTestAccess::statusBarButton(app.main(), "Show check") == nullptr);
    AppTestAccess::openShowSettings(app.main());
    app.tick();
    auto& panel = AppTestAccess::settingsPanel(app.main());
    auto& b = AppTestAccess::showCheckButton(panel);
    auto& close = AppTestAccess::closeButton(panel);
    requireHittable(b);
    CHECK(b.getY() == close.getY());
    CHECK(b.getRight() <= close.getX());
    tap(b, b.getLocalBounds().getCentre());
    app.tick();
    CHECK(AppTestAccess::showCheckPanel(app.main()).isVisible());
}

TEST_CASE("The alert hint opens Show check") {
    const ManualClock clock;
    TestApp app(threeSoundSteps());
    app.session().alert(AlertKind::MissingFile, "first");
    app.session().alert(AlertKind::MissingFile, "second");
    app.tick();
    auto& banner = AppTestAccess::alertBanner(app.main());
    tap(banner, {banner.getWidth() - 10, banner.getHeight() / 2});
    app.tick();
    CHECK(AppTestAccess::showCheckPanel(app.main()).isVisible());
}

}  // namespace bigtop::app

namespace bigtop::app {

TEST_CASE("The status bar counts imports of a drop") {
    CHECK(importStatusText(0, 0).isEmpty());
    CHECK(importStatusText(2, 10) == "Importing 3 of 10");  // the one being converted is the third
    CHECK(importStatusText(10, 10).isEmpty());
}

}  // namespace bigtop::app
