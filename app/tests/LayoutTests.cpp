// LayoutTests.cpp: what the UI refresh must leave alone (the show settings' scroll position, the
// cached Details lines) and what it must redo (Details after a show edit). Moved here from the
// screenshot render, with the same assertions.
#include <catch2/catch_test_macros.hpp>

#include "AppTestSupport.h"
#include "UiClock.h"

namespace bigtop::app {

TEST_CASE("show settings keeps its scroll across refreshes") {
    const ManualClock clock;
    TestApp app(threeSoundSteps());
    AppTestAccess::openShowSettings(app.main());
    app.tick(2);
    auto& panel = AppTestAccess::settingsPanel(app.main());
    requireHittable(panel);

    // Shorter than its fields (owner bug 2026-11-22: the refresh scrolled it back to the top).
    panel.setSize(panel.getWidth(), 300);
    app.tick(2);
    auto& view = AppTestAccess::viewport(panel);
    view.setViewPosition(0, 100000);  // clamps to the bottom
    const int bottom = view.getViewPositionY();
    REQUIRE(bottom > 0);

    app.tick(4);

    REQUIRE(view.getViewPositionY() == bottom);
}

TEST_CASE("Details lines are not reworded on idle ticks but are after a rename") {
    const ManualClock clock;
    TestApp app(threeSoundSteps());
    AppTestAccess::setDetails(app.main(), true);
    app.tick(2);
    const auto& steps = AppTestAccess::steps(app.main());
    requireHittable(steps);
    const int before = AppTestAccess::describeCount(steps);
    REQUIRE(before > 0);  // Details are on: they were worded once

    app.tick(4);
    REQUIRE(AppTestAccess::describeCount(steps) == before);

    REQUIRE(app.session().edit(Action::EditValue, [](Show& s) {
        s.steps[2].name = "Song B (renamed)";
        return true;
    }));
    app.tick(2);
    REQUIRE(AppTestAccess::describeCount(steps) > before);
}

namespace {

// The name label of the running row of step @p id, after it has started.
juce::Label& runningName(TestApp& app, StepId id) {
    auto* row = AppTestAccess::running(app.main()).findChildWithID(juce::String(id));
    REQUIRE(row != nullptr);
    auto* name = dynamic_cast<juce::Label*>(row->findChildWithID("name"));
    REQUIRE(name != nullptr);
    return *name;
}

}  // namespace

TEST_CASE("an ordinary file name fits a running row on one line") {
    ManualClock clock;
    Show show = threeSoundSteps();
    show.steps[0].name = "01-01 - part i - altar";
    TestApp app(show, 1280, 720);
    app.waitHeads();
    AppTestAccess::go(app.main());
    app.audio(2);
    auto& name = runningName(app, show.steps[0].id);
    requireHittable(name);
    const auto text = name.getBorderSize().subtractedFrom(name.getLocalBounds());
    CHECK(juce::GlyphArrangement::getStringWidth(name.getFont(), name.getText()) <= static_cast<float>(text.getWidth()));
}

TEST_CASE("a long name wraps to two lines and stays off the position bar") {
    ManualClock clock;
    Show show = threeSoundSteps();
    show.steps[0].name = "A very long step name that cannot fit one line of a running row";
    TestApp app(show, 1280, 720);
    app.waitHeads();
    AppTestAccess::go(app.main());
    app.audio(2);
    auto& name = runningName(app, show.steps[0].id);
    // LookAndFeel draws as many lines as fit the label's height, the last one cut with an ellipsis.
    CHECK(name.getHeight() >= 2 * juce::roundToInt(name.getFont().getHeight()));
    auto* bar = name.getParentComponent()->findChildWithID("bar");
    REQUIRE(bar != nullptr);
    CHECK_FALSE(name.getBounds().intersects(bar->getBounds()));
    CHECK(bar->getWidth() >= kRowBarFloor);
}

}  // namespace bigtop::app
