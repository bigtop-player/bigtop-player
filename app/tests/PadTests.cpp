// PadTests.cpp: the pad grid and the pad editor as the operator uses them (a pad fires on the
// release, a second tap fades it, "Edit pads" opens the editor instead, a drop names the pad),
// plus earlier pad and session defects, each pinned by a test written before its fix.
#include <cmath>
#include <functional>
#include <optional>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "AppTestSupport.h"
#include "DemoAudio.h"
#include "UiClock.h"
#include "bigtop/Gain.h"
#include "bigtop/io/AudioOutput.h"

namespace bigtop::app {

namespace {

constexpr FileId kPadFile = 4;  // the pad's own file, after threeSoundSteps()' three

juce::Point<int> centre(const juce::Component& c) { return c.getLocalBounds().getCentre(); }

// threeSoundSteps() with pad 0 playing its own file "Jingle"; the other pads are empty.
Show padShow() {
    Show show = threeSoundSteps();
    show.files.push_back({kPadFile, "audio/jingle.flac", 0.0, ""});
    show.pads[0].name = "Jingle";
    show.pads[0].file = kPadFile;
    return show;
}

// Pad @p index's voice in the latest snapshot, if it plays.
std::optional<VoiceState> padVoice(TestApp& app, int index) {
    const auto snap = app.session().snapshot();
    for (int i = 0; i < snap.numVoices; ++i)
        if (snap.voices[i].key == kPadKeyBase + static_cast<VoiceKey>(index)) return snap.voices[i];
    return std::nullopt;
}

// The centre of pad @p index in the painted cells' coordinates (the layout of padCellSize()).
juce::Point<int> padPoint(TestApp& app, int index) {
    const auto& s = app.session().controller().show().settings;
    auto& cells = AppTestAccess::padCells(AppTestAccess::pads(app.main()));
    const int cw = (cells.getWidth() - kPadGap * (s.padCols + 1)) / s.padCols;
    const int ch = (cells.getHeight() - kPadGap * (s.padRows + 1)) / s.padRows;
    return {kPadGap + (index % s.padCols) * (cw + kPadGap) + cw / 2, kPadGap + (index / s.padCols) * (ch + kPadGap) + ch / 2};
}

// Taps pad @p index on the grid.
void tapPad(TestApp& app, int index) {
    auto& cells = AppTestAccess::padCells(AppTestAccess::pads(app.main()));
    requireHittable(cells);
    tap(cells, padPoint(app, index));
}

// Turns "Edit pads" on with a tap on the toggle.
void editPadsOn(TestApp& app) {
    auto& toggle = AppTestAccess::editPads(AppTestAccess::pads(app.main()));
    requireHittable(toggle);
    REQUIRE(!toggle.getToggleState());
    tap(toggle, centre(toggle));
    REQUIRE(toggle.getToggleState());
}

// Opens the pad editor on pad @p index the way the operator does: "Edit pads" on, tap the pad.
PadEditor& openPadEditor(TestApp& app, int index) {
    auto& editor = AppTestAccess::padEditor(app.main());
    REQUIRE(!editor.isVisible());
    editPadsOn(app);
    tapPad(app, index);
    app.tick();
    requireHittable(editor);
    return editor;
}

const Pad& pad(TestApp& app, int index) { return app.session().controller().show().pads.at(static_cast<size_t>(index)); }

}  // namespace

TEST_CASE("a pad fires on release") {
    ManualClock clock;
    TestApp app(padShow());
    app.waitHeads();
    auto& cells = AppTestAccess::padCells(AppTestAccess::pads(app.main()));
    requireHittable(cells);
    const auto at = padPoint(app, 0);
    press(cells, at);
    app.audio(2);
    REQUIRE(!padVoice(app, 0));  // the press alone plays nothing
    release(cells, at, at);
    app.audio(2);
    REQUIRE(padVoice(app, 0));
}

TEST_CASE("second tap fades the pad") {
    ManualClock clock;
    TestApp app(padShow());
    app.waitHeads();
    tapPad(app, 0);
    app.audio(2);
    REQUIRE(padVoice(app, 0));
    tapPad(app, 0);
    app.audio(2);
    REQUIRE(padVoice(app, 0)->gain < 1.0f);  // fading over kPadStopSeconds, not cut
    app.audio(20);                           // about 0.2 s, past the 0.05 s fade
    REQUIRE(!padVoice(app, 0));
}

TEST_CASE("Edit pads on: tap opens the editor, nothing plays") {
    ManualClock clock;
    TestApp app(padShow());
    app.waitHeads();
    openPadEditor(app, 0);
    app.audio(2);
    REQUIRE(!padVoice(app, 0));
}

TEST_CASE("drop on a pad in Edit names it after the file") {
    ManualClock clock;
    TestApp app(padShow());
    TempDir source;
    const auto file = source.dir.getChildFile("Fanfare.flac");
    REQUIRE(writeSineFlac(file, 1.0, 0.1f));
    auto& grid = AppTestAccess::pads(app.main());
    requireHittable(grid);
    REQUIRE(pad(app, 1).name.empty());
    const auto at = grid.getLocalPoint(&AppTestAccess::padCells(grid), padPoint(app, 1));
    grid.filesDropped(juce::StringArray(file.getFullPathName()), at.x, at.y);
    app.waitUntil([&] { return pad(app, 1).file.has_value(); });
    REQUIRE(pad(app, 1).name == "Fanfare");
    app.waitUntil([&] { return !app.session().importsRunning(); });
}

// Regression: tapped() trusted the toggle without a mode check, so the tap between entering Show and
// the next tick (which turns the toggle off) neither opened the editor nor played.
TEST_CASE("in Show mode a pad tap never opens the pad editor") {
    ManualClock clock;
    TestApp app(padShow());
    app.waitHeads();
    editPadsOn(app);
    AppTestAccess::pickMode(app.main(), ShowMode::Show);  // what the strip's tap on SHOW does; no UI tick follows
    REQUIRE(app.session().mode() == ShowMode::Show);
    REQUIRE(!padVoice(app, 0));
    tapPad(app, 0);
    REQUIRE(!AppTestAccess::padEditor(app.main()).isVisible());
    app.audio(2);
    REQUIRE(padVoice(app, 0));
}

// Regression guard: edit() grows a short pad list before change() may refuse. The growth is on the copy
// ShowSession::edit() throws away, so this held on the code as it was (no fix).
TEST_CASE("PadEditor leaves the pads alone when the change is refused") {
    ManualClock clock;
    TestApp app(padShow());
    // A list shorter than the grid, as fromJson may leave it (a saved show loads with all 8).
    Show shortList = app.session().controller().show();
    shortList.pads.resize(1);
    app.session().controller().updateShow(shortList);
    auto& editor = openPadEditor(app, 5);
    REQUIRE(app.session().controller().show().pads.size() == 1);

    SECTION("refused by the mode") {
        app.session().setMode(ShowMode::Show);  // not about the hold; no tick closes the panel first
        auto& plus = AppTestAccess::plus(AppTestAccess::padLevel(editor));
        requireHittable(plus);
        tap(plus, centre(plus));
    }
    SECTION("refused by the change (trim of a pad without a file)") {
        AppTestAccess::padTrim(editor).onCommit(3.0);  // what the field calls when it commits
    }
    REQUIRE(app.session().controller().show().pads.size() == 1);
}

// The pad editor works while the pad plays (an operator fixes a level mid-show). These replace
// earlier tests that pinned a refusal ("Pad is playing: change it after it stops").

TEST_CASE("a playing pad is renamed at once and plays on") {
    ManualClock clock;
    TestApp app(padShow());
    app.waitHeads();
    tapPad(app, 0);
    app.audio(2);
    REQUIRE(padVoice(app, 0));
    const float gain = padVoice(app, 0)->gain;
    auto& editor = openPadEditor(app, 0);
    auto& name = AppTestAccess::padName(editor);
    requireHittable(name);
    REQUIRE(name.getText() == "Jingle");
    REQUIRE(AppTestAccess::padLabel(AppTestAccess::pads(app.main()), 0) == "Jingle");

    name.setText("Fanfare", false);  // typed, then Return commits
    name.onReturnKey();

    REQUIRE(pad(app, 0).name == "Fanfare");
    REQUIRE(AppTestAccess::padLabel(AppTestAccess::pads(app.main()), 0) == "Fanfare");
    app.audio(2);
    REQUIRE(padVoice(app, 0));
    REQUIRE(padVoice(app, 0)->gain == gain);
    app.tick();
    CHECK(name.getText() == "Fanfare");
}

namespace {

constexpr int kSlice = 48;  // frames per rendered slice: 1 ms at 48 kHz, so the 10 ms declick spans 10 slices

// Renders @p slices slices of kSlice frames and returns pad @p index's gain after each (-1 once gone).
std::vector<float> padGains(TestApp& app, int index, int slices) {
    std::vector<float> left(kSlice), right(kSlice), gains;
    for (int i = 0; i < slices; ++i) {
        app.session().engineForScreenshots().process(left.data(), right.data(), kSlice);
        const auto v = padVoice(app, index);
        gains.push_back(v ? v->gain : -1.0f);
    }
    return gains;
}

// Pad 0 plays at gain 1; @p commit stores a -12 dB change through the editor (level or trim).
// The running gain must reach the new value within the declick and never step faster than the
// engine's 10 ms equal-power ramp allows.
void requireRampTo(const std::function<void(PadEditor&)>& commit, const std::function<double(TestApp&)>& stored) {
    ManualClock clock;
    TestApp app(padShow());
    app.waitHeads();
    tapPad(app, 0);
    app.audio(2);
    REQUIRE(padVoice(app, 0));
    REQUIRE(padVoice(app, 0)->gain == 1.0f);
    auto& editor = openPadEditor(app, 0);
    REQUIRE(stored(app) == 0.0);

    commit(editor);

    REQUIRE(stored(app) == -12.0);
    const float from = 1.0f;
    const auto to = static_cast<float>(dbToGain(-12.0));
    const auto gains = padGains(app, 0, 30);
    const double declickFrames = kDeclickSeconds * io::kEngineSampleRate;
    // The steepest slice of a sin/cos ramp over the declick: the first (rising) or last (falling).
    const float maxStep = (from - to) * static_cast<float>(std::sin(1.5707963 * kSlice / declickFrames)) + 1e-4f;
    float previous = from;
    bool between = false;
    for (float g : gains) {
        REQUIRE(g >= 0.0f);  // the voice plays on
        REQUIRE(std::abs(g - previous) <= maxStep);
        between = between || (g < from - 1e-3f && g > to + 1e-3f);
        previous = g;
    }
    REQUIRE(between);  // a ramp, not a jump
    REQUIRE(gains.back() == Catch::Approx(to));
}

}  // namespace

TEST_CASE("a playing pad's level change ramps the running sound") {
    requireRampTo(
        [](PadEditor& editor) {
            auto& level = AppTestAccess::padLevel(editor);
            requireHittable(level);
            level.onCommit(-12.0);  // what the field calls when it commits
        },
        [](TestApp& app) { return pad(app, 0).levelDb; });
}

TEST_CASE("a playing pad's file trim change ramps the running sound") {
    requireRampTo(
        [](PadEditor& editor) {
            auto& trim = AppTestAccess::padTrim(editor);
            requireHittable(trim);
            trim.onCommit(-12.0);
        },
        [](TestApp& app) {
            for (const auto& f : app.session().controller().show().files)
                if (f.id == kPadFile) return f.trimDb;
            return 99.0;
        });
}

TEST_CASE("Clear pad on a playing pad fades it like a second tap, then the pad is empty") {
    ManualClock clock;
    TestApp app(padShow());
    app.waitHeads();
    tapPad(app, 0);
    app.audio(2);
    REQUIRE(padVoice(app, 0));
    auto& editor = openPadEditor(app, 0);
    auto& clear = button(editor, "Clear pad");
    requireHittable(clear);
    tap(clear, centre(clear));

    REQUIRE(!pad(app, 0).file);
    // Half of the second tap's kPadStopSeconds fade: cos(pi/4) of the way, not cut and not held.
    const auto gains = padGains(app, 0, static_cast<int>(kPadStopSeconds * io::kEngineSampleRate / 2 / kSlice));
    REQUIRE(gains.back() == Catch::Approx(std::cos(1.5707963 / 2)).margin(0.02));
    app.audio(20);  // about 0.2 s, past the 0.05 s fade
    REQUIRE(!padVoice(app, 0));

    // Close the editor (the grid is hidden behind it), Edit pads off; a tap on the cleared pad starts nothing.
    auto& close = button(editor, "Close");
    requireHittable(close);
    tap(close, centre(close));
    app.tick();
    auto& toggle = AppTestAccess::editPads(AppTestAccess::pads(app.main()));
    requireHittable(toggle);
    tap(toggle, centre(toggle));
    REQUIRE(!toggle.getToggleState());
    tapPad(app, 0);
    app.audio(2);
    REQUIRE(!padVoice(app, 0));
}

// Review: Clear stored the empty pad even when its fade could not be sent (full engine queue),
// leaving a voice no tap can fade. Now the fade goes first and a refused fade refuses the Clear.
TEST_CASE("Clear pad is refused when the playing pad's fade cannot be sent") {
    ManualClock clock;
    TestApp app(padShow());
    app.waitHeads();
    tapPad(app, 0);
    app.audio(2);
    REQUIRE(padVoice(app, 0));
    auto& editor = openPadEditor(app, 0);
    auto& clear = button(editor, "Clear pad");
    requireHittable(clear);
    int posted = 0;  // fill the engine queue with harmless commands (key 9 plays nothing)
    while (app.session().engineForScreenshots().post(TrimCommand{9, 1.0f}) && posted < 100000) ++posted;
    REQUIRE(!app.session().engineForScreenshots().post(TrimCommand{9, 1.0f}));

    tap(clear, centre(clear));

    REQUIRE(pad(app, 0).file == kPadFile);  // kept, so a tap can still fade it
    REQUIRE(app.session().status().message.contains("Engine busy"));
    app.audio(20);  // the queue drains; nothing faded the pad
    REQUIRE(padVoice(app, 0));
    tap(clear, centre(clear));  // the same Clear now goes through
    REQUIRE(!pad(app, 0).file);
    app.audio(20);
    REQUIRE(!padVoice(app, 0));
}

TEST_CASE("a pad edit while the pad is silent still goes through") {
    ManualClock clock;
    TestApp app(padShow());
    app.waitHeads();
    auto& editor = openPadEditor(app, 0);
    auto& plus = AppTestAccess::plus(AppTestAccess::padLevel(editor));
    requireHittable(plus);
    const double level = pad(app, 0).levelDb;
    tap(plus, centre(plus));
    REQUIRE(pad(app, 0).levelDb > level);
    app.audio(2);
    REQUIRE(!padVoice(app, 0));  // an edit never starts the pad
}

TEST_CASE("the pad editor closes on a show switch") {
    ManualClock clock;
    TestApp app(padShow());
    auto& editor = openPadEditor(app, 0);
    auto& shows = button(app.main(), juce::String(juce::CharPointer_UTF8("Shows\xe2\x80\xa6")));
    requireHittable(shows);
    tap(shows, centre(shows));
    REQUIRE(!editor.isVisible());

    TempDir other;
    const auto folder = writeShow(other, padShow());
    std::string error;
    REQUIRE(app.session().open(folder, error, nullptr));
    AppTestAccess::showOpened(app.main());
    app.tick();
    REQUIRE(!editor.isVisible());
    requireHittable(AppTestAccess::pads(app.main()));
}

// The STATUS fallback: the controller does not know about this pause, only the engine does.
TEST_CASE("setMode refuses Show while the engine reports paused") {
    ManualClock clock;
    TestApp app(padShow());
    app.waitHeads();
    AppTestAccess::go(app.main());
    app.audio(2);
    REQUIRE(app.session().engineForScreenshots().post(PauseCommand{true}));
    app.audio(2);
    REQUIRE(app.session().snapshot().paused);
    REQUIRE(!app.session().controller().paused());
    REQUIRE(app.session().mode() == ShowMode::Edit);
    REQUIRE(!app.session().setMode(ShowMode::Show));
    REQUIRE(app.session().mode() == ShowMode::Edit);
    REQUIRE(app.session().status().message == juce::String::fromUTF8(ShowSession::kShowRefusedPaused));
}

TEST_CASE("pad editor start and end: stored in Edit mode, end before start refused") {
    ManualClock clock;
    TestApp app(padShow());
    app.waitHeads();
    auto& editor = openPadEditor(app, 0);
    auto& start = AppTestAccess::padStart(editor);
    auto& end = AppTestAccess::padEnd(editor);
    requireHittable(start);
    requireHittable(end);
    end.onCommit(4.0);
    start.onCommit(1.5);
    CHECK(pad(app, 0).startSeconds == 1.5);
    CHECK(pad(app, 0).endSeconds == 4.0);
    end.onCommit(1.0);  // not after the start: refused with a notice, the old end stays
    CHECK(pad(app, 0).endSeconds == 4.0);
    CHECK(app.session().status().message == "End must be after Start");
    end.onClear();  // empty: play to the end of the file
    CHECK_FALSE(pad(app, 0).endSeconds.has_value());
}

TEST_CASE("a pad's start and end are Edit-only, like a step's") {
    ManualClock clock;
    TestApp app(padShow());
    app.waitHeads();
    AppTestAccess::pickMode(app.main(), ShowMode::Rehearsal);
    app.tick();
    auto& editor = openPadEditor(app, 0);
    CHECK_FALSE(AppTestAccess::padStart(editor).isEnabled());
    CHECK(AppTestAccess::padLevel(editor).isEnabled());
}

namespace {

// Pad @p index's centre in the step rows' coordinates, where a handle drag's events land.
juce::Point<int> padInRows(TestApp& app, int index) {
    auto& rows = AppTestAccess::rows(AppTestAccess::steps(app.main()));
    auto& cells = AppTestAccess::padCells(AppTestAccess::pads(app.main()));
    return rows.getLocalPoint(&cells, padPoint(app, index));
}

std::vector<StepId> stepOrder(TestApp& app) {
    std::vector<StepId> ids;
    for (const auto& s : app.session().controller().show().steps) ids.push_back(s.id);
    return ids;
}

}  // namespace

TEST_CASE("dragging a row by its handle onto a pad assigns it") {
    ManualClock clock;
    Show show = padShow();
    show.steps[1].sound->startSeconds = 1.0;
    show.pads[3].levelDb = -6.0;
    TestApp app(show);
    app.waitHeads();
    auto& list = AppTestAccess::steps(app.main());
    auto& rows = AppTestAccess::rows(list);
    const auto before = stepOrder(app);
    const auto handle = AppTestAccess::handlePoint(list, 1);
    const auto target = padInRows(app, 3);
    press(rows, handle, 0, &list);
    drag(rows, handle, target, 0, &list);
    CHECK(AppTestAccess::padDropHighlight(AppTestAccess::pads(app.main())) == 3);
    CHECK(AppTestAccess::insertionLine(list) == -1);  // over a pad: no line in the list
    release(rows, handle, target, 0, &list);
    app.tick();
    const Pad& p = pad(app, 3);
    CHECK(p.name == "Song A");
    CHECK(p.file == show.steps[1].sound->file);
    CHECK(p.startSeconds == 1.0);
    CHECK(p.levelDb == -6.0);  // the pad keeps its own level
    CHECK(stepOrder(app) == before);
    CHECK(AppTestAccess::padDropHighlight(AppTestAccess::pads(app.main())) == -1);
}

TEST_CASE("a drag that leaves for a pad and comes back still reorders") {
    ManualClock clock;
    TestApp app(padShow());
    app.waitHeads();
    auto& list = AppTestAccess::steps(app.main());
    auto& rows = AppTestAccess::rows(list);
    const auto before = stepOrder(app);
    const auto handle = AppTestAccess::handlePoint(list, 2);
    const juce::Point<int> top{120, AppTestAccess::rowTop(list, 0) + 5};
    press(rows, handle, 0, &list);
    drag(rows, handle, padInRows(app, 3), 0, &list);
    drag(rows, handle, top, 0, &list);
    CHECK(AppTestAccess::padDropHighlight(AppTestAccess::pads(app.main())) == -1);
    release(rows, handle, top, 0, &list);
    app.tick();
    CHECK(stepOrder(app) == std::vector<StepId>{before[2], before[0], before[1]});
    CHECK(pad(app, 3).name.empty());
}

TEST_CASE("a fade step dropped on a pad is refused") {
    ManualClock clock;
    Show show = padShow();
    show.steps[1].sound.reset();  // a fade step: no sound to give a pad
    TestApp app(show);  // no waitHeads(): its file is used by nothing now, so it is never read
    auto& list = AppTestAccess::steps(app.main());
    auto& rows = AppTestAccess::rows(list);
    const auto handle = AppTestAccess::handlePoint(list, 1);
    const auto target = padInRows(app, 3);
    press(rows, handle, 0, &list);
    drag(rows, handle, target, 0, &list);
    release(rows, handle, target, 0, &list);
    app.tick();
    CHECK_FALSE(pad(app, 3).file.has_value());
    CHECK(app.session().status().message.isNotEmpty());
}

// A new file on a pad starts with the whole file: the old range belonged to the old sound.
TEST_CASE("a file dropped on a pad clears the pad's old start and end") {
    ManualClock clock;
    Show show = padShow();
    show.pads[1] = Pad{.name = "Long", .file = kPadFile, .levelDb = 0.0, .startSeconds = 90.0, .endSeconds = 120.0};
    TestApp app(show);
    TempDir source;
    const auto file = source.dir.getChildFile("Fanfare.flac");
    REQUIRE(writeSineFlac(file, 1.0, 0.1f));
    auto& grid = AppTestAccess::pads(app.main());
    const auto at = grid.getLocalPoint(&AppTestAccess::padCells(grid), padPoint(app, 1));
    grid.filesDropped(juce::StringArray(file.getFullPathName()), at.x, at.y);
    app.waitUntil([&] { return pad(app, 1).name == "Fanfare"; });
    CHECK(pad(app, 1).startSeconds == 0.0);
    CHECK_FALSE(pad(app, 1).endSeconds.has_value());
    app.waitUntil([&] { return !app.session().importsRunning(); });
}

}  // namespace bigtop::app
