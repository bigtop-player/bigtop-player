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

}  // namespace bigtop::app
