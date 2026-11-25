// EditorTests.cpp: the step editor's number fields, typed into as keys would and committed the
// way JUCE commits them (Enter, focus loss), plus the -/+ buttons, the start / end check and the
// timecode field. Fields are found by their label (AppTestAccess::field).
#include <algorithm>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "AppTestSupport.h"
#include "UiClock.h"
#include "bigtop/ShowEdits.h"

namespace bigtop::app {

namespace {

// threeSoundSteps() with a time away from both limits (Intro's start delay 2 s) and an end point
// (Song A ends at 5 s).
Show editorShow() {
    Show show = threeSoundSteps();
    show.steps[0].sound->startDelaySeconds = 2.0;
    show.steps[1].sound->endSeconds = 5.0;
    return show;
}

// The windowless app with the editor open on step @p id, its fields laid out for the panel width.
struct Editing {
    ManualClock clock;
    TestApp app;
    StepEditor& editor = AppTestAccess::editor(app.main());

    explicit Editing(StepId id, const Show& show = editorShow()) : app(show) {
        AppTestAccess::openEditor(app.main(), id);
        app.tick();  // the panel got its size after open(): the fields are rebuilt for it once
        requireHittable(editor);
    }

    // Looked up again on every use: a refused edit rebuilds the fields on the next tick.
    NumberField& field(const juce::String& label) { return AppTestAccess::field(editor, label); }

    const Sound& sound(StepId id) {
        const auto& steps = app.session().controller().show().steps;
        const auto it = std::find_if(steps.begin(), steps.end(), [id](const Step& s) { return s.id == id; });
        REQUIRE((it != steps.end() && it->sound));
        return *it->sound;
    }
};

// Types @p text into the field's box key by key, replacing what it showed (the box selects all
// when it gains focus, so the first key replaces the text).
void type(NumberField& f, const juce::String& text) {
    auto& box = AppTestAccess::box(f);
    requireHittable(box);
    box.setText({}, false);
    for (const auto c : text) REQUIRE(box.keyPressed(juce::KeyPress(static_cast<int>(c), {}, c)));
    REQUIRE(box.getText() == text);
}

// JUCE posts Enter and focus loss as messages to the box and then calls these handlers; the tests
// call them directly, so no other queued message (the 30 Hz timer) can run in between.
void enter(NumberField& f) { AppTestAccess::box(f).onReturnKey(); }
void loseFocus(NumberField& f) { AppTestAccess::box(f).onFocusLost(); }

}  // namespace

TEST_CASE("Enter, focus loss and tap away store the value") {
    Editing e(1);
    REQUIRE(e.sound(1).startDelaySeconds == 2.0);
    type(e.field("Start delay"), "3.5");
    REQUIRE(e.sound(1).startDelaySeconds == 2.0);  // never per keystroke

    SECTION("Enter") { enter(e.field("Start delay")); }
    // A tap anywhere else takes the focus, so it ends in the same handler.
    SECTION("focus loss or a tap away") { loseFocus(e.field("Start delay")); }

    REQUIRE(e.sound(1).startDelaySeconds == 3.5);
    REQUIRE(AppTestAccess::box(e.field("Start delay")).getText() == "3.5");
}

TEST_CASE("1,5 is 1.5") {
    Editing e(1);
    REQUIRE(e.sound(1).startDelaySeconds == 2.0);
    type(e.field("Start delay"), "1,5");
    enter(e.field("Start delay"));
    REQUIRE(e.sound(1).startDelaySeconds == 1.5);
    REQUIRE(AppTestAccess::box(e.field("Start delay")).getText() == "1.5");
}

TEST_CASE("junk puts the stored value back") {
    const juce::String junk = GENERATE("abc", "", "nan", "1e999", "1.2.3", "-");
    INFO("typed: \"" << junk << "\"");
    Editing e(1);
    const auto shown = AppTestAccess::box(e.field("Start delay")).getText();
    REQUIRE(e.sound(1).startDelaySeconds == 2.0);
    type(e.field("Start delay"), junk);
    enter(e.field("Start delay"));
    REQUIRE(e.sound(1).startDelaySeconds == 2.0);
    REQUIRE(AppTestAccess::box(e.field("Start delay")).getText() == shown);
}

TEST_CASE("a finite number outside the range is clamped to it") {
    // Out of range is clamped (a typo should not lose the whole value); only non-numbers are refused.
    Editing e(1);
    REQUIRE(e.sound(1).startDelaySeconds == 2.0);
    SECTION("1e40 s is the longest time") {
        type(e.field("Start delay"), "1e40");
        enter(e.field("Start delay"));
        REQUIRE(e.sound(1).startDelaySeconds == kEditMaxSeconds);
    }
    SECTION("-3 s is no time") {
        type(e.field("Start delay"), "-3");
        enter(e.field("Start delay"));
        REQUIRE(e.sound(1).startDelaySeconds == 0.0);
    }
}

TEST_CASE("start at or after end is refused with a message") {
    Editing e(2);  // Song A: start 0, end 5 s
    REQUIRE(e.sound(2).startSeconds == 0.0);
    REQUIRE(e.sound(2).endSeconds == 5.0);
    e.app.session().flash({});

    SECTION("start at the end") {
        const auto shown = AppTestAccess::box(e.field("Start")).getText();
        type(e.field("Start"), "0:05");
        enter(e.field("Start"));
        REQUIRE(e.sound(2).startSeconds == 0.0);
        REQUIRE(e.app.session().status().message == "Start must be before End");
        REQUIRE(AppTestAccess::box(e.field("Start")).getText() == shown);
    }
    SECTION("end at the start") {
        const auto shown = AppTestAccess::box(e.field("End")).getText();
        type(e.field("End"), "0");
        enter(e.field("End"));
        REQUIRE(e.sound(2).endSeconds == 5.0);
        REQUIRE(e.app.session().status().message == "End must be after Start");
        REQUIRE(AppTestAccess::box(e.field("End")).getText() == shown);
    }
}

TEST_CASE("the - and + buttons step by 0.5") {
    Editing e(1);
    const double before = e.sound(1).levelDb;
    auto& plus = AppTestAccess::plus(e.field("Level"));
    auto& minus = AppTestAccess::minus(e.field("Level"));
    requireHittable(plus);
    requireHittable(minus);
    tap(plus, plus.getLocalBounds().getCentre());
    REQUIRE(e.sound(1).levelDb == before + 0.5);
    tap(minus, minus.getLocalBounds().getCentre());
    tap(minus, minus.getLocalBounds().getCentre());
    REQUIRE(e.sound(1).levelDb == before - 0.5);
}

TEST_CASE("an untouched field does not re-save a rounded value") {
    // 1.23 as the plan names it, and 1.234, which the box shows as "1.23": re-reading the
    // shown text would store a different value.
    const double stored = GENERATE(1.23, 1.234);
    Show show = editorShow();
    show.steps[0].sound->fadeInSeconds = stored;
    Editing e(1, show);
    REQUIRE(e.sound(1).fadeInSeconds == stored);
    // Focus in (nothing typed) and out again, and an Enter on the untouched box.
    loseFocus(e.field("Fade in"));
    enter(e.field("Fade in"));
    REQUIRE(e.sound(1).fadeInSeconds == stored);
}

TEST_CASE("timecode 00:00:10:30 refused at 25 fps, 00:00:10:12 accepted") {
    Editing e(1);
    REQUIRE(e.app.session().controller().show().settings.timecodeFps == 25);
    REQUIRE(!e.sound(1).timecodeStart);
    type(e.field("Timecode"), "00:00:10:30");
    enter(e.field("Timecode"));
    REQUIRE(!e.sound(1).timecodeStart);
    REQUIRE(AppTestAccess::box(e.field("Timecode")).getText().isEmpty());

    type(e.field("Timecode"), "00:00:10:12");
    enter(e.field("Timecode"));
    REQUIRE(e.sound(1).timecodeStart);
    const auto tc = *e.sound(1).timecodeStart;
    REQUIRE((tc.h == 0 && tc.m == 0 && tc.s == 10 && tc.f == 12));
    REQUIRE(AppTestAccess::box(e.field("Timecode")).getText() == "00:00:10:12");
}

}  // namespace bigtop::app
