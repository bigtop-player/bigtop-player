// AccessibilityTests.cpp: what a screen reader gets from the step list. The tests are windowless,
// and JUCE's getAccessibilityHandler() / getChildren() answer only for components with a native
// window, so they build the handlers the way those would: the rows component's own handler, and
// one per child that JUCE's focus traverser lists (the walk getChildren() makes). No screen reader
// is started; Narrator itself is on the rehearsal checklist.
#include <memory>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "AppTestSupport.h"
#include "UiClock.h"

namespace bigtop::app {

namespace {

// The list's handler and its items' handlers, in reading order. Must not outlive a layout change.
struct ReadList {
    std::unique_ptr<juce::AccessibilityHandler> list;
    std::vector<std::unique_ptr<juce::AccessibilityHandler>> rows;
};

ReadList read(StepList& steps) {
    auto& rows = AppTestAccess::rows(steps);
    ReadList r{rows.createAccessibilityHandler(), {}};
    for (auto* child : rows.createFocusTraverser()->getAllComponents(&rows)) r.rows.push_back(child->createAccessibilityHandler());
    return r;
}

std::vector<juce::String> titles(StepList& steps) {
    std::vector<juce::String> out;
    for (const auto& h : read(steps).rows) out.push_back(h->getTitle());
    return out;
}

}  // namespace

TEST_CASE("the step list reads as a list of its rows") {
    const ManualClock clock;
    TestApp app(threeSoundSteps());
    app.waitHeads();
    auto& list = AppTestAccess::steps(app.main());
    app.session().controller().selectNext(1);
    app.tick();

    auto& rows = AppTestAccess::rows(list);
    CHECK(rows.isFocusContainer());  // what makes the focus traverser list the row items
    // The items are click-through: a point in a row hits the rows component, so taps, drags and
    // tooltips stay the rows' own.
    requireHittable(rows);
    CHECK(rows.getComponentAt(juce::Point<int>(200, AppTestAccess::rowTop(list, 1) + 24)) == &rows);
    const auto r = read(list);
    REQUIRE(r.list->getRole() == juce::AccessibilityRole::list);
    REQUIRE(r.rows.size() == 3);
    for (const auto& h : r.rows) {
        CHECK(h->getRole() == juce::AccessibilityRole::listItem);
        CHECK(h->getCurrentState().isAccessibleOffscreen());  // scrolled-away rows are read too
        CHECK(h->getActions().contains(juce::AccessibilityActionType::press));
    }
    CHECK(r.rows[0]->getTitle() == "1, Intro");
    CHECK(r.rows[1]->getTitle() == "2, Song A, next");
    CHECK(r.rows[2]->getTitle() == "3, Song B");
}

TEST_CASE("a missing file is read out") {
    Show show = threeSoundSteps();
    show.files.pop_back();           // Song B's file is gone from the show,
    show.steps[2].sound->file = 99;  // and its step names a file id that does not exist
    const ManualClock clock;
    TestApp app(show);
    app.waitHeads();  // the files the show has
    app.tick();
    const auto t = titles(AppTestAccess::steps(app.main()));
    REQUIRE(t.size() == 3);
    CHECK(t[0] == "1, Intro, next");
    CHECK(t[2] == "3, Song B, missing file");
}

TEST_CASE("Details lines are read when Details is on") {
    const ManualClock clock;
    TestApp app(threeSoundSteps());
    app.waitHeads();
    auto& list = AppTestAccess::steps(app.main());
    app.tick();
    REQUIRE(titles(list) == std::vector<juce::String>{"1, Intro, next", "2, Song A", "3, Song B"});

    AppTestAccess::setDetails(app.main(), true);
    app.tick();
    const auto& lines = AppTestAccess::detailLines(list, 1);
    REQUIRE_FALSE(lines.empty());
    juce::String expected = "2, Song A";
    for (const auto& line : lines) expected << ", " << juce::String::fromUTF8(line.text.c_str());
    const auto t = titles(list);
    REQUIRE(t.size() == 3);
    CHECK(t[1] == expected);
    // Intro's reserved everything-else line is blank (nothing before it plays): it is not read.
    const auto& intro = AppTestAccess::detailLines(list, 0);
    REQUIRE(intro.size() == 2);
    REQUIRE(intro[1].text.empty());
    CHECK(t[0] == "1, Intro, next, " + juce::String::fromUTF8(intro[0].text.c_str()));
}

TEST_CASE("the select action makes a row NEXT") {
    // The same path as a tap, in every mode a tap selects in.
    const ShowMode mode = GENERATE(ShowMode::Edit, ShowMode::Rehearsal, ShowMode::Show);
    const ManualClock clock;
    TestApp app(threeSoundSteps());
    app.waitHeads();
    auto& list = AppTestAccess::steps(app.main());
    app.tick();
    AppTestAccess::pickMode(app.main(), mode);
    app.tick();
    REQUIRE(app.session().mode() == mode);
    requireHittable(AppTestAccess::rows(list));
    REQUIRE(app.session().controller().nextIndex() == 0);

    {
        const auto r = read(list);
        REQUIRE(r.rows.size() == 3);
        REQUIRE(r.rows[2]->getActions().invoke(juce::AccessibilityActionType::press));
    }
    REQUIRE(app.session().controller().nextIndex() == 2);
    app.tick();
    const auto t = titles(list);
    REQUIRE(t.size() == 3);
    CHECK(t[0] == "1, Intro");
    CHECK(t[2] == "3, Song B, next");
}

}  // namespace bigtop::app
