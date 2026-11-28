// ModeTests.cpp: the mode strip's 2 s hold out of Show mode (finished, cancelled by a release, a
// drag off, a second finger or a lost release, untouched by the Details checkbox) and one pass
// over the permission table of Mode.h: every Action triggered through its control in every mode.
#include <algorithm>
#include <cstdlib>
#include <functional>
#include <optional>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "AppTestSupport.h"
#include "DemoAudio.h"
#include "Pointer.h"
#include "UiClock.h"

namespace bigtop::app {

namespace {

using Catch::Matchers::WithinAbs;

constexpr int kBlock = 512;                 // frames per TestApp::audio() block
constexpr juce::uint32 kPastGuardMs = 400;  // more than the default 0.3 s GO guard
constexpr int kNameX = 200;                 // a point in a step row's name area, right of the handle

juce::Point<int> centre(const juce::Component& c) { return c.getLocalBounds().getCentre(); }

// Moves the manual clock on by @p ms in 100 ms steps, running the strip's hold timer after each
// (the real timer runs at 30 Hz; what it does depends only on the clock).
void holdFor(ManualClock& clock, ModeBar& bar, int ms) {
    for (int t = 0; t < ms; t += 100) {
        clock.advance(100);
        AppTestAccess::holdTimer(bar);
    }
}

// The app in Show mode with a hold on EDIT under way (finger 0, still down).
struct Holding {
    ManualClock clock;
    TestApp app{threeSoundSteps()};
    ModeBar& bar = AppTestAccess::modeBar(app.main());
    juce::Component& edit = AppTestAccess::modeButton(bar, ShowMode::Edit);
    juce::Point<int> at = centre(edit);
    std::optional<PointerHeld> held;

    Holding() {
        AppTestAccess::pickMode(app.main(), ShowMode::Show);
        requireHittable(edit);
        REQUIRE(app.session().mode() == ShowMode::Show);
        press(edit, at);
        held.emplace(edit);
    }
};

// The step voice of @p key in the latest snapshot, if it plays.
std::optional<VoiceState> voice(TestApp& app, VoiceKey key) {
    const auto snap = app.session().snapshot();
    for (int i = 0; i < snap.numVoices; ++i)
        if (snap.voices[i].key == key) return snap.voices[i];
    return std::nullopt;
}

int indexOf(TestApp& app, StepId id) {
    const auto& steps = app.session().controller().show().steps;
    const auto it = std::find_if(steps.begin(), steps.end(), [id](const Step& s) { return s.id == id; });
    REQUIRE(it != steps.end());
    return static_cast<int>(it - steps.begin());
}

std::vector<StepId> order(TestApp& app) {
    std::vector<StepId> ids;
    for (const auto& s : app.session().controller().show().steps) ids.push_back(s.id);
    return ids;
}

size_t stepCount(TestApp& app) { return app.session().controller().show().steps.size(); }

// Drags a fader from its thumb by the pixels that move it to @p db (see RunningTests.cpp).
void dragFader(juce::Slider& fader, double db) {
    requireHittable(fader);
    const auto y = fader.getHeight() / 2;
    const juce::Point<int> from(juce::roundToInt(fader.getPositionOfValue(fader.getValue())), y);
    const double proportion = fader.valueToProportionOfLength(db) - fader.valueToProportionOfLength(fader.getValue());
    const juce::Point<int> to(from.x + juce::roundToInt(proportion * fader.getMouseDragSensitivity()), y);
    press(fader, from);
    drag(fader, from, to);
    release(fader, from, to);
    REQUIRE_THAT(fader.getValue(), WithinAbs(db, 0.01));
}

// Clears the status bar notice, so a refusal afterwards is this action's.
void clearNotice(TestApp& app) {
    app.session().flash({});
    REQUIRE(app.session().status().message.isEmpty());
}

// Whether the notice names the mode that refused (ShowSession::allowed's wording).
bool refusedBy(TestApp& app, ShowMode mode) {
    return app.session().status().message.startsWith(mode == ShowMode::Show ? "Show mode" : "Rehearsal mode");
}

}  // namespace

TEST_CASE("holding EDIT 2 s leaves Show") {
    Holding h;
    holdFor(h.clock, h.bar, 1900);
    REQUIRE(h.app.session().mode() == ShowMode::Show);
    holdFor(h.clock, h.bar, 100);  // 2.0 s: done without waiting for the release
    REQUIRE(h.app.session().mode() == ShowMode::Edit);
    h.held.reset();
    release(h.edit, h.at, h.at);
    REQUIRE(h.app.session().mode() == ShowMode::Edit);
}

TEST_CASE("release at 1.9 s, drag off, second finger keep Show") {
    Holding h;
    // In every section the finger stays reported down, so only the event under test can cancel.
    SECTION("release at 1.9 s") {
        holdFor(h.clock, h.bar, 1900);
        release(h.edit, h.at, h.at);
    }
    SECTION("drag off the button") {
        holdFor(h.clock, h.bar, 1000);
        drag(h.edit, h.at, {h.at.x, -40});
    }
    SECTION("a second finger on the button") {
        holdFor(h.clock, h.bar, 1000);
        press(h.edit, h.at, 1);
    }
    SECTION("the finger is no longer down but no release came") {
        holdFor(h.clock, h.bar, 1000);
        h.held.reset();
    }
    holdFor(h.clock, h.bar, 3000);
    REQUIRE(h.app.session().mode() == ShowMode::Show);
}

TEST_CASE("toggling Details during a hold neither cancels nor finishes it") {
    Holding h;
    holdFor(h.clock, h.bar, 1000);
    auto& details = AppTestAccess::detailsBox(h.bar);
    requireHittable(details);
    REQUIRE(!details.getToggleState());
    tap(details, centre(details), 1);  // another finger, while finger 0 holds EDIT
    REQUIRE(details.getToggleState());
    holdFor(h.clock, h.bar, 900);
    REQUIRE(h.app.session().mode() == ShowMode::Show);
    holdFor(h.clock, h.bar, 100);
    REQUIRE(h.app.session().mode() == ShowMode::Edit);
}

TEST_CASE("every Action is enabled, hidden or refused as Mode.h says") {
    const ShowMode mode = GENERATE(ShowMode::Edit, ShowMode::Rehearsal, ShowMode::Show);
    INFO("mode " << modeName(mode));
    ManualClock clock;
    TestApp app(threeSoundSteps());
    app.waitHeads();
    AppTestAccess::pickMode(app.main(), mode);
    app.tick();
    REQUIRE(app.session().mode() == mode);

    auto& main = app.main();
    auto& controller = app.session().controller();
    auto& list = AppTestAccess::steps(main);
    auto& rows = AppTestAccess::rows(list);
    auto& editor = AppTestAccess::editor(main);
    const TempDir temp;
    const auto flac = temp.dir.getChildFile("Drum roll.flac");
    REQUIRE(writeSineFlac(flac, 2.0, 0.1f));
    const juce::StringArray files{flac.getFullPathName()};

    // One row per Action: how to trigger it on screen and what to see. `ok` is allowed(mode, action).
    // The rows run in this order on one app; each leaves the screen as the next expects it (no
    // side panel open, nothing paused). Every Action has a control, so none is left out.
    struct Row {
        Action action;
        const char* what;
        std::function<void(bool ok)> check;
    };
    const std::vector<Row> table{
        {Action::SwitchShow, "Shows... in the status bar",
         [&](bool ok) {
             auto& shows = button(main, juce::String::fromUTF8("Shows\xe2\x80\xa6"));
             // A mode that refuses it hides it: nothing left to tap that does nothing.
             REQUIRE(shows.isVisible() == ok);
             if (!ok) return;
             requireHittable(shows);
             REQUIRE(shows.isEnabled());
             auto& go = AppTestAccess::goButton(main);
             REQUIRE(go.isVisible());
             tap(shows, centre(shows));
             REQUIRE(go.isVisible() != ok);  // the start panel covers the show
             if (ok) {
                 auto& back = button(main, "Back to Test show");
                 requireHittable(back);
                 tap(back, centre(back));
                 REQUIRE(go.isVisible());
                 app.waitHeads();  // leaving may free the sources; they are read again
             }
         }},
        {Action::ShowSettings, "Show settings in the status bar",
         [&](bool ok) {
             auto& b = button(main, "Show settings");
             REQUIRE(b.isVisible() == ok);
             if (!ok) return;
             requireHittable(b);
             REQUIRE(b.isEnabled());
             auto& panel = AppTestAccess::settingsPanel(main);
             REQUIRE(!panel.isVisible());
             tap(b, centre(b));
             REQUIRE(panel.isVisible() == ok);
             if (ok) {
                 auto& close = button(panel, "Close");
                 requireHittable(close);
                 tap(close, centre(close));
                 REQUIRE(!panel.isVisible());
             }
         }},
        {Action::AudioSettings, "Settings in the status bar",
         [&](bool ok) {
             // Enabled state only: a tap opens the real device settings window, which tests never open.
             auto& b = button(main, "Settings");
             REQUIRE(b.isVisible() == ok);
             if (!ok) return;
             requireHittable(b);
             REQUIRE(b.isEnabled());
         }},
        {Action::Pause, "the Pause button",
         [&](bool ok) {
             auto& pause = AppTestAccess::pauseButton(main);
             REQUIRE(pause.isVisible() == ok);
             if (!ok) return;
             requireHittable(pause);
             REQUIRE(!controller.paused());
             tap(pause, centre(pause));
             REQUIRE(controller.paused());
             tap(pause, centre(pause));
             REQUIRE(!controller.paused());
             app.audio(2);
         }},
        {Action::EditValue, "a step row's Edit button",
         [&](bool ok) {
             requireHittable(rows);
             const int r = indexOf(app, 3);
             const juce::Point<int> at(rows.getWidth() - 12 - StepList::kEditWidth / 2,
                                       AppTestAccess::rowTop(list, r) + StepList::kRowHeight / 2);
             REQUIRE(!editor.isVisible());
             press(rows, at, 0, &list);
             release(rows, at, at, 0, &list);
             REQUIRE(editor.isVisible() == ok);
             if (!ok) controller.selectNext(0);  // no Edit button there: the tap selected the row
         }},
        {Action::Delete, "the editor's Delete step (Show mode: the Delete key, as there is no editor)",
         [&](bool ok) {
             const auto count = stepCount(app);
             if (editor.isVisible()) {
                 auto& del = button(editor, "Delete step");
                 requireHittable(del);
                 REQUIRE(del.isEnabled() == ok);
                 tap(del, centre(del));
                 app.tick();  // the editor sees its step gone and closes
                 REQUIRE(stepCount(app) == (ok ? count - 1 : count));
                 if (!ok) {
                     auto& close = button(editor, "Close");
                     tap(close, centre(close));
                 }
                 REQUIRE(!editor.isVisible());
             } else {
                 REQUIRE(!ok);
                 clearNotice(app);
                 REQUIRE(main.keyPressed(juce::KeyPress(juce::KeyPress::deleteKey)));
                 REQUIRE(refusedBy(app, mode));
                 REQUIRE(stepCount(app) == count);
             }
         }},
        {Action::EditStructure, "+ Fade",
         [&](bool ok) {
             auto& add = button(main, "+ Fade");
             REQUIRE(add.isVisible() == ok);
             if (!ok) return;
             requireHittable(add);
             const auto count = stepCount(app);
             tap(add, centre(add));
             REQUIRE(stepCount(app) == count + 1);
             REQUIRE(editor.isVisible());  // the new fade step is opened
             auto& close = button(editor, "Close");
             tap(close, centre(close));
             REQUIRE(!editor.isVisible());
         }},
        {Action::Reorder, "a step row's grab handle",
         [&](bool ok) {
             requireHittable(rows);
             const auto before = order(app);
             const auto handle = AppTestAccess::handlePoint(list, 0);
             const juce::Point<int> gap2(kNameX, AppTestAccess::rowTop(list, 2) + 5);
             press(rows, handle, 0, &list);
             drag(rows, handle, gap2, 0, &list);
             release(rows, handle, gap2, 0, &list);
             auto expected = before;
             if (ok) std::rotate(expected.begin(), expected.begin() + 1, expected.begin() + 2);  // row 0 to index 1
             REQUIRE(order(app) == expected);
         }},
        {Action::DropFiles, "a file dropped on the step list",
         [&](bool ok) {
             requireHittable(list);
             const auto count = stepCount(app);
             const juce::Point<int> at(kNameX, AppTestAccess::rowTop(list, 0) + 5);
             clearNotice(app);
             list.fileDragEnter(files, at.x, at.y);
             REQUIRE(AppTestAccess::insertionLine(list) == (ok ? 0 : -1));
             list.filesDropped(files, at.x, at.y);
             if (ok) {
                 app.waitUntil([&] { return stepCount(app) == count + 1; });
                 REQUIRE(controller.show().steps.front().name == "Drum roll");
                 app.waitUntil([&] { return !app.session().importsRunning(); });
             } else {
                 REQUIRE(refusedBy(app, mode));
                 REQUIRE(stepCount(app) == count);
             }
         }},
        {Action::AssignPad, "a file dropped on a pad",
         [&](bool ok) {
             auto& pads = AppTestAccess::pads(main);
             requireHittable(pads);
             const auto assigned = [&] {
                 const auto& all = controller.show().pads;
                 return std::any_of(all.begin(), all.end(), [](const Pad& p) { return p.file.has_value(); });
             };
             REQUIRE(!assigned());
             clearNotice(app);
             const auto at = centre(pads);
             pads.filesDropped(files, at.x, at.y);
             if (ok) {
                 app.waitUntil(assigned);
                 app.waitUntil([&] { return !app.session().importsRunning(); });
             } else {
                 REQUIRE(refusedBy(app, mode));
                 REQUIRE(!assigned());
             }
         }},
        {Action::SelectNext, "a tap on a step row",
         [&](bool) {
             controller.selectNext(indexOf(app, 2));
             const int r = indexOf(app, 1);
             REQUIRE(controller.nextIndex() != r);
             const juce::Point<int> at(kNameX, AppTestAccess::rowTop(list, r) + StepList::kRowHeight / 2);
             requireHittable(rows);
             press(rows, at, 0, &list);
             release(rows, at, at, 0, &list);
             REQUIRE(controller.nextIndex() == r);
         }},
        {Action::Go, "the GO button",
         [&](bool) {
             auto& go = AppTestAccess::goButton(main);
             requireHittable(go);
             REQUIRE(!voice(app, 1));
             clock.advance(kPastGuardMs);
             tap(go, centre(go));
             app.audio(5);
             REQUIRE(voice(app, 1));
         }},
        {Action::Fader, "a running row's fader",
         [&](bool) {
             auto& fader = AppTestAccess::faderOf(AppTestAccess::row(main, 1));
             REQUIRE(fader.getValue() == 0.0);
             dragFader(fader, -10.0);
             app.tick();
             app.audio(2);  // the engine applied the trim
         }},
        {Action::KeepFader, "a running row's Keep",
         [&](bool ok) {
             auto& row = AppTestAccess::row(main, 1);
             auto& keep = AppTestAccess::keepOf(row);
             REQUIRE(keep.isVisible() == ok);
             if (!ok) return;
             requireHittable(keep);
             REQUIRE(keep.isEnabled());
             const double before = controller.show().steps.at(static_cast<size_t>(indexOf(app, 1))).sound->levelDb;
             tap(keep, centre(keep));
             const double after = controller.show().steps.at(static_cast<size_t>(indexOf(app, 1))).sound->levelDb;
             REQUIRE_THAT(after, WithinAbs(before - 10.0, 0.05));
             REQUIRE(AppTestAccess::faderOf(row).getValue() == 0.0);
         }},
        {Action::Seek, "a running row's position bar",
         [&](bool) {
             auto& bar = AppTestAccess::barOf(AppTestAccess::row(main, 1));
             requireHittable(bar);
             const int w = bar.getWidth(), y = bar.getHeight() / 2;
             const int64_t length = app.session().sources().fileLengthFrames(1);
             REQUIRE(length > 0);
             const int x = w * 4 / 10;  // inside the 5 s head
             const int64_t target = seekFrameForX(x, w, 0, length);
             REQUIRE(voice(app, 1)->positionFrame < target - kBlock);
             const juce::Point<int> from(juce::roundToInt(static_cast<double>(voice(app, 1)->positionFrame) * w / length), y);
             press(bar, from);
             drag(bar, from, {x, y});
             release(bar, from, {x, y});
             app.audio(1);
             REQUIRE(std::abs(voice(app, 1)->positionFrame - target) <= kBlock);
         }},
        {Action::FadeOutAll, "Fade out",
         [&](bool) {
             auto& fade = button(main, "Fade out (Esc)");
             requireHittable(fade);
             const float before = voice(app, 1)->gain;
             tap(fade, centre(fade));
             app.audio(20);  // about 0.2 s of the 2 s fade
             REQUIRE(voice(app, 1));
             REQUIRE(voice(app, 1)->gain < before);
         }},
        {Action::FadeSound, "a running row's Fade",
         [&](bool) {
             controller.selectNext(indexOf(app, 2));
             clock.advance(kPastGuardMs);
             AppTestAccess::go(main);  // Song A, a sound that is not fading yet
             app.audio(5);
             REQUIRE(voice(app, 2));
             const float before = voice(app, 2)->gain;
             auto& fade = AppTestAccess::fadeOf(AppTestAccess::row(main, 2));
             requireHittable(fade);
             tap(fade, centre(fade));
             app.audio(20);
             REQUIRE(voice(app, 2));
             REQUIRE(voice(app, 2)->gain < before);
         }},
    };

    // The table covers every Action exactly once (SwitchShow is the enum's last value, see Mode.h).
    std::vector<int> covered;
    for (const auto& row : table) covered.push_back(static_cast<int>(row.action));
    std::sort(covered.begin(), covered.end());
    std::vector<int> all(static_cast<size_t>(Action::SwitchShow) + 1);
    for (size_t i = 0; i < all.size(); ++i) all[i] = static_cast<int>(i);
    REQUIRE(covered == all);

    for (const auto& row : table) {
        INFO("action " << static_cast<int>(row.action) << ": " << row.what);
        row.check(allowed(mode, row.action));
    }
}

TEST_CASE("Show mode refused while paused says why") {
    ManualClock clock;
    TestApp app(threeSoundSteps());
    app.waitHeads();
    auto& main = app.main();
    AppTestAccess::go(main);
    app.audio(5);
    auto& pause = AppTestAccess::pauseButton(main);
    tap(pause, centre(pause));
    app.audio(2);
    REQUIRE(app.session().snapshot().paused);

    // The strip's tap ends in pickMode(); a windowless test cannot tap it
    // (Component::contains() needs a window), so the test enters there.
    AppTestAccess::pickMode(main, ShowMode::Show);
    app.tick();
    REQUIRE(app.session().mode() == ShowMode::Edit);
    auto& notice = AppTestAccess::statusMessage(main);
    requireHittable(notice);
    REQUIRE(notice.getText() == juce::String::fromUTF8(ShowSession::kShowRefusedPaused));
}

TEST_CASE("hold bar stays inside the button's rounded corners") {
    Holding h;
    holdFor(h.clock, h.bar, 1900);  // about 95 %: the bar reaches the bottom-left and bottom-right corners
    const auto image = h.edit.createComponentSnapshot(h.edit.getLocalBounds(), true, 1.0f);
    const int bottom = image.getHeight() - 1;
    // Outside the rounded corner the button paints nothing: the strip shows through.
    CHECK(image.getPixelAt(0, bottom).getAlpha() == 0);
    CHECK(image.getPixelAt(1, bottom - 1).getAlpha() == 0);
    // The bar itself is painted along the bottom edge.
    CHECK(image.getPixelAt(image.getWidth() / 2, bottom - 2).getAlpha() == 255);
}

TEST_CASE("hidden status-bar buttons leave the rest in place") {
    TestApp app(threeSoundSteps());
    auto& main = app.main();
    auto& showSettings = button(main, "Show settings");
    REQUIRE(showSettings.isVisible());
    auto* status = showSettings.getParentComponent();
    std::vector<juce::Rectangle<int>> before;
    for (auto* c : status->getChildren()) before.push_back(c->getBounds());
    AppTestAccess::pickMode(main, ShowMode::Show);
    app.tick();
    REQUIRE_FALSE(showSettings.isVisible());
    // Nothing moves under a finger: every part keeps its place, the hidden buttons leave a gap.
    for (int i = 0; i < status->getNumChildComponents(); ++i)
        if (auto* c = status->getChildComponent(i); c->isVisible()) CHECK(c->getBounds() == before[static_cast<size_t>(i)]);
}

namespace {

// threeSoundSteps() with "Pause in Show mode" set as given.
Show pauseShow(bool pauseInShow) {
    Show show = threeSoundSteps();
    show.settings.pauseInShow = pauseInShow;
    return show;
}

}  // namespace

TEST_CASE("with Pause in Show mode on, Show mode has Pause") {
    ManualClock clock;
    TestApp app(pauseShow(true));
    app.waitHeads();
    auto& main = app.main();
    AppTestAccess::go(main);
    app.audio(5);
    AppTestAccess::pickMode(main, ShowMode::Show);
    app.tick();
    REQUIRE(app.session().mode() == ShowMode::Show);
    auto& pause = AppTestAccess::pauseButton(main);
    requireHittable(pause);
    tap(pause, centre(pause));
    app.audio(2);
    REQUIRE(app.session().snapshot().paused);
    // Paused in Edit, then into Show: allowed when Show mode has Pause.
    AppTestAccess::pickMode(main, ShowMode::Edit);
    AppTestAccess::pickMode(main, ShowMode::Show);
    REQUIRE(app.session().mode() == ShowMode::Show);
}

TEST_CASE("turning Pause in Show mode off while paused keeps the pause and refuses Show") {
    ManualClock clock;
    TestApp app(pauseShow(true));
    app.waitHeads();
    auto& main = app.main();
    AppTestAccess::go(main);
    app.audio(5);
    AppTestAccess::pickMode(main, ShowMode::Show);
    auto& pause = AppTestAccess::pauseButton(main);
    tap(pause, centre(pause));
    app.audio(2);
    AppTestAccess::pickMode(main, ShowMode::Edit);
    REQUIRE(app.session().edit(Action::ShowSettings, [](Show& s) {
        s.settings.pauseInShow = false;
        return true;
    }));
    app.tick();
    REQUIRE(app.session().snapshot().paused);
    AppTestAccess::pickMode(main, ShowMode::Show);
    REQUIRE(app.session().mode() == ShowMode::Edit);
    REQUIRE(app.session().status().message == juce::String::fromUTF8(ShowSession::kShowRefusedPaused));
}

TEST_CASE("the Pause in Show mode tick box sets the show setting") {
    TestApp app(threeSoundSteps());
    auto& main = app.main();
    AppTestAccess::openShowSettings(main);
    // The tick box sits in the panel's scrolled content: search the whole tree.
    std::function<juce::Component*(juce::Component&)> find = [&](juce::Component& c) -> juce::Component* {
        for (auto* child : c.getChildren()) {
            if (child->getComponentID() == "pauseInShow") return child;
            if (auto* found = find(*child)) return found;
        }
        return nullptr;
    };
    auto* tick = dynamic_cast<juce::ToggleButton*>(find(AppTestAccess::settingsPanel(main)));
    REQUIRE(tick != nullptr);
    REQUIRE_FALSE(tick->getToggleState());
    tap(*tick, centre(*tick));
    REQUIRE(app.session().controller().show().settings.pauseInShow);
    REQUIRE(tick->getToggleState());
}

}  // namespace bigtop::app

namespace bigtop::app {

TEST_CASE("the GO mode frame follows the button and never lies on Pause, + Fade or Fade out") {
    // The border used to be painted by the window around the button's bounds; after a
    // mode switch moved the buttons, its old stroke stayed on Pause and Fade out.
    // As its own child, JUCE repaints the frame's old and new place itself.
    TestApp app(threeSoundSteps());
    auto& main = app.main();
    for (const auto mode : {ShowMode::Show, ShowMode::Rehearsal, ShowMode::Edit, ShowMode::Show, ShowMode::Edit}) {
        AppTestAccess::pickMode(main, mode);
        app.tick();
        INFO("mode " << static_cast<int>(mode));
        const auto& frame = AppTestAccess::goFrame(main);
        const auto& go = AppTestAccess::goButton(main);
        REQUIRE(frame.isVisible());
        CHECK(frame.getBounds() == go.getBounds().expanded(ModeFrame::kWidth));
        CHECK(frame.colour() == juce::Colour(modeColour(mode)));
        for (juce::Component* other : {static_cast<juce::Component*>(&AppTestAccess::pauseButton(main)),
                                       static_cast<juce::Component*>(&AppTestAccess::fadeButton(main)),
                                       static_cast<juce::Component*>(&AppTestAccess::addFadeButton(main))})
            if (other->isVisible()) CHECK_FALSE(frame.getBounds().intersects(other->getBounds()));
    }
}

}  // namespace bigtop::app

namespace bigtop::app {

TEST_CASE("Keep awake follows the mode") {
    const ManualClock clock;
    TestApp app(threeSoundSteps());
    auto& calls = app.keepAwakeCalls();
    REQUIRE((calls.empty() || calls.back() == false));  // Edit mode lets the laptop sleep
    AppTestAccess::pickMode(app.main(), ShowMode::Rehearsal);
    app.tick();
    CHECK(calls.back() == true);
    AppTestAccess::pickMode(app.main(), ShowMode::Show);
    app.tick();
    CHECK(calls.back() == true);
    AppTestAccess::pickMode(app.main(), ShowMode::Edit);
    app.tick();
    CHECK(calls.back() == false);
}

TEST_CASE("Quit in Show mode asks first, and Cancel keeps the app") {
    const ManualClock clock;
    TestApp app(threeSoundSteps());
    bool quit = false;
    AppTestAccess::onQuitConfirmed(app.main()) = [&] { quit = true; };
    AppTestAccess::pickMode(app.main(), ShowMode::Show);
    app.tick();
    CHECK_FALSE(app.main().quitRequested());
    auto* box = dynamic_cast<juce::AlertWindow*>(juce::Component::getCurrentlyModalComponent());
    REQUIRE(box != nullptr);
    CHECK(box->getNumButtons() == 2);
    CHECK(box->getHelpText() == "Quit Bigtop Player during the show?");
    box->exitModalState(0);  // Cancel
    app.pumpMessages(50);
    CHECK_FALSE(quit);
    CHECK_FALSE(app.main().quitRequested());
    box = dynamic_cast<juce::AlertWindow*>(juce::Component::getCurrentlyModalComponent());
    REQUIRE(box != nullptr);
    box->exitModalState(1);  // Quit
    app.pumpMessages(50);
    CHECK(quit);
}

TEST_CASE("Quit in Edit mode goes ahead") {
    const ManualClock clock;
    TestApp app(threeSoundSteps());
    CHECK(app.main().quitRequested());
    CHECK(juce::Component::getCurrentlyModalComponent() == nullptr);
}

}  // namespace bigtop::app

namespace bigtop::app {

TEST_CASE("Shows... while sounds play raises a banner and keeps the show") {
    ManualClock clock;
    TestApp app(threeSoundSteps());
    app.waitHeads();
    AppTestAccess::go(app.main());
    app.audio(4);
    AppTestAccess::chooseShow(app.main());
    app.tick();
    CHECK_FALSE(AppTestAccess::startPanel(app.main()).isVisible());
    const auto a = app.session().currentAlert();
    REQUIRE(a.has_value());
    CHECK(a->text.isNotEmpty());
}

TEST_CASE("A show that opens clears the old failure text") {
    const ManualClock clock;
    TestApp app(threeSoundSteps());
    auto& panel = AppTestAccess::startPanel(app.main());
    panel.showFailure(juce::File::getCurrentWorkingDirectory().getChildFile("Gone show"), "could not read show.json", {});
    REQUIRE(AppTestAccess::startError(panel).getText().isNotEmpty());
    AppTestAccess::tryOpen(panel, app.session().showDir());
    CHECK(AppTestAccess::startError(panel).getText().isEmpty());
}

TEST_CASE("A moved last show is named on the start panel") {
    const ManualClock clock;
    TestApp app(threeSoundSteps());
    auto& panel = AppTestAccess::startPanel(app.main());
    panel.showMissing("/shows/Winter gala");
    CHECK(AppTestAccess::startError(panel).getText() == "'Winter gala' was not found at /shows/Winter gala");
}

TEST_CASE("The folder choosers start next to the last show, and the title names the show") {
    const ManualClock clock;
    TestApp app(threeSoundSteps());
    CHECK(app.session().browseStart() == app.session().showDir().getParentDirectory());
    CHECK(app.main().windowTitle() == juce::String::fromUTF8("Bigtop Player \xe2\x80\x93 Test show"));
}

}  // namespace bigtop::app
