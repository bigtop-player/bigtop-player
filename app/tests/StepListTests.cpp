// StepListTests.cpp: the step list's own mouse handling (handle drag, the insertion line, the edge
// auto-scroll, a release that never came), file drops and the Details toggle, driven through the
// windowless app. Mouse events go to the rows component in content coordinates and reach StepList
// as its mouse listener, as JUCE delivers them.
#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <juce_audio_formats/juce_audio_formats.h>

#include "AppTestSupport.h"
#include "UiClock.h"

namespace bigtop::app {

namespace {

constexpr int kNameX = 200;  // a point in a row's name area, right of the handle

// threeSoundSteps() plus plain sound steps "Step 4".."Step n" on the same three files.
Show manySteps(int n) {
    Show show = threeSoundSteps();
    for (StepId id = 4; id <= static_cast<StepId>(n); ++id) {
        Sound sound;
        sound.file = static_cast<FileId>((id - 1) % 3 + 1);
        Step step = makeStep(id, show.settings, sound);
        step.name = "Step " + std::to_string(id);
        show.steps.push_back(step);
    }
    show.lastStepId = static_cast<StepId>(n);
    return show;
}

// The step ids in list order.
std::vector<StepId> order(TestApp& app) {
    std::vector<StepId> ids;
    for (const auto& s : app.session().controller().show().steps) ids.push_back(s.id);
    return ids;
}

// The step names in list order.
std::vector<std::string> names(TestApp& app) {
    std::vector<std::string> out;
    for (const auto& s : app.session().controller().show().steps) out.push_back(s.name);
    return out;
}

// The id of the step NEXT is on.
StepId nextId(TestApp& app) {
    const auto& c = app.session().controller();
    return c.show().steps.at(static_cast<size_t>(c.nextIndex())).id;
}

// A point in the top half of row @p gap's name area: the insertion gap above that row.
juce::Point<int> gapPoint(const StepList& list, int gap) { return {kNameX, AppTestAccess::rowTop(list, gap) + 5}; }

// @p ids with the element at @p from moved to index @p to.
std::vector<StepId> moved(std::vector<StepId> ids, int from, int to) {
    const auto id = ids[static_cast<size_t>(from)];
    ids.erase(ids.begin() + from);
    ids.insert(ids.begin() + to, id);
    return ids;
}

// A 0.5 s stereo sine WAV.
juce::File writeWav(const juce::File& file) {
    constexpr double kRate = 48000.0;
    juce::AudioBuffer<float> buf(2, static_cast<int>(0.5 * kRate));
    for (int i = 0; i < buf.getNumSamples(); ++i)
        for (int ch = 0; ch < 2; ++ch)
            buf.setSample(ch, i, 0.1f * static_cast<float>(std::sin(2.0 * juce::MathConstants<double>::pi * 440.0 * i / kRate)));
    std::unique_ptr<juce::OutputStream> out = std::make_unique<juce::FileOutputStream>(file);
    auto w = juce::WavAudioFormat().createWriterFor(
            out, juce::AudioFormatWriterOptions{}.withSampleRate(kRate).withNumChannels(2).withBitsPerSample(16));
    REQUIRE(w != nullptr);
    REQUIRE(w->writeFromAudioSampleBuffer(buf, 0, buf.getNumSamples()));
    return file;
}

// Two WAVs and a text file, in that order, in @p temp.
juce::StringArray dropFiles(const TempDir& temp) {
    const auto text = temp.dir.getChildFile("notes.txt");
    REQUIRE(text.replaceWithText("not audio"));
    return {writeWav(temp.dir.getChildFile("Drum roll.wav")).getFullPathName(),
            writeWav(temp.dir.getChildFile("Fanfare.wav")).getFullPathName(), text.getFullPathName()};
}

// Whether row @p row says "everything else fades out ...".
bool fadesElse(const StepList& list, int row) {
    const auto& lines = AppTestAccess::detailLines(list, row);
    return std::any_of(lines.begin(), lines.end(), [](const DetailLine& l) { return l.text.starts_with("everything else"); });
}

// threeSoundSteps() with Intro ending at 0.5 s, so it ends by itself within a few audio blocks.
Show shortIntro() {
    Show show = threeSoundSteps();
    show.steps[0].sound->endSeconds = 0.5;
    return show;
}

// Taps row @p row's name area: in Edit mode that makes it NEXT.
void tapRow(StepList& list, int row) {
    auto& rows = AppTestAccess::rows(list);
    requireHittable(rows);
    const juce::Point<int> at(kNameX, AppTestAccess::rowTop(list, row) + 24);
    press(rows, at, 0, &list);
    release(rows, at, at, 0, &list);
}

}  // namespace

TEST_CASE("a handle press whose release never came does not leave a row attached") {
    const ManualClock clock;
    TestApp app(manySteps(10));
    auto& list = AppTestAccess::steps(app.main());
    auto& rows = AppTestAccess::rows(list);
    requireHittable(rows);
    const auto before = order(app);
    REQUIRE(AppTestAccess::insertionLine(list) == -1);

    // Row 2 picked up by its handle and carried one row down; its release is lost.
    const auto handle = AppTestAccess::handlePoint(list, 2);
    press(rows, handle, 0, &list);
    drag(rows, handle, gapPoint(list, 4), 0, &list);
    REQUIRE(AppTestAccess::insertionLine(list) == 4);

    SECTION("the next press and release of that finger do not drop it") {
        const juce::Point<int> name(kNameX, AppTestAccess::rowTop(list, 6) + 24);
        press(rows, name, 0, &list);
        release(rows, name, name, 0, &list);
        REQUIRE(order(app) == before);
        REQUIRE(AppTestAccess::insertionLine(list) == -1);
    }
    SECTION("the pointer leaving with no button held drops nothing") {
        leave(rows, gapPoint(list, 4), 1, &list);  // another finger leaving: the drag stays
        REQUIRE(AppTestAccess::insertionLine(list) == 4);
        leave(rows, gapPoint(list, 4), 0, &list);
        REQUIRE(AppTestAccess::insertionLine(list) == -1);
        const juce::Point<int> name(kNameX, AppTestAccess::rowTop(list, 6) + 24);
        press(rows, name, 0, &list);
        release(rows, name, name, 0, &list);
        REQUIRE(order(app) == before);
    }

    // A full handle drag afterwards still works: row 3 to index 5 (gap 6).
    const auto handle3 = AppTestAccess::handlePoint(list, 3);
    press(rows, handle3, 0, &list);
    drag(rows, handle3, gapPoint(list, 6), 0, &list);
    release(rows, handle3, gapPoint(list, 6), 0, &list);
    REQUIRE(order(app) == moved(before, 3, 5));
}

TEST_CASE("a mouse exit with a button held does not cancel a handle drag") {
    const ManualClock clock;
    TestApp app(manySteps(10));
    auto& list = AppTestAccess::steps(app.main());
    auto& rows = AppTestAccess::rows(list);
    requireHittable(rows);
    const auto before = order(app);
    REQUIRE(AppTestAccess::insertionLine(list) == -1);

    const auto handle = AppTestAccess::handlePoint(list, 2);
    press(rows, handle, 0, &list);
    drag(rows, handle, gapPoint(list, 4), 0, &list);
    REQUIRE(AppTestAccess::insertionLine(list) == 4);

    // The pointer leaves the rows with the button still down: JUCE keeps the drag on them.
    leave(rows, gapPoint(list, 4), 0, &list, juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier));
    REQUIRE(AppTestAccess::insertionLine(list) == 4);
    drag(rows, handle, gapPoint(list, 6), 0, &list);
    REQUIRE(AppTestAccess::insertionLine(list) == 6);
    release(rows, handle, gapPoint(list, 6), 0, &list);
    REQUIRE(order(app) == moved(before, 2, 5));
}

TEST_CASE("handle drag moves the row, NEXT stays on its step") {
    const ManualClock clock;
    TestApp app(manySteps(10));
    auto& list = AppTestAccess::steps(app.main());
    auto& rows = AppTestAccess::rows(list);
    requireHittable(rows);
    app.session().controller().selectNext(1);
    app.tick();
    const auto before = order(app);
    const StepId next = nextId(app);
    REQUIRE(AppTestAccess::insertionLine(list) == -1);

    const auto handle = AppTestAccess::handlePoint(list, 3);
    press(rows, handle, 0, &list);
    drag(rows, handle, gapPoint(list, 0), 0, &list);
    REQUIRE(AppTestAccess::insertionLine(list) == 0);  // the line at the gap the row will land in
    REQUIRE(order(app) == before);                     // nothing moves before the release
    release(rows, handle, gapPoint(list, 0), 0, &list);
    app.tick();

    REQUIRE(order(app) == moved(before, 3, 0));
    REQUIRE(nextId(app) == next);
    REQUIRE(app.session().controller().nextIndex() == 2);  // its step moved down by one
    REQUIRE(AppTestAccess::insertionLine(list) == -1);
}

TEST_CASE("dragging elsewhere in a row does not reorder or change NEXT") {
    const ManualClock clock;
    TestApp app(manySteps(10));
    auto& list = AppTestAccess::steps(app.main());
    auto& rows = AppTestAccess::rows(list);
    requireHittable(rows);
    const auto before = order(app);
    const int next = app.session().controller().nextIndex();
    REQUIRE(next != 2);  // a tap on row 2 would change it: the drag must not

    const juce::Point<int> from(kNameX, AppTestAccess::rowTop(list, 2) + 24);
    press(rows, from, 0, &list);
    drag(rows, from, gapPoint(list, 6), 0, &list);
    REQUIRE(AppTestAccess::insertionLine(list) == -1);
    release(rows, from, gapPoint(list, 6), 0, &list);
    app.tick();

    REQUIRE(order(app) == before);
    REQUIRE(app.session().controller().nextIndex() == next);
}

TEST_CASE("no handles and no reorder in Rehearsal and Show") {
    const ManualClock clock;
    TestApp app(manySteps(10));
    const auto mode = GENERATE(ShowMode::Rehearsal, ShowMode::Show);
    AppTestAccess::pickMode(app.main(), mode);
    app.tick();
    REQUIRE(app.session().mode() == mode);
    auto& list = AppTestAccess::steps(app.main());
    auto& rows = AppTestAccess::rows(list);
    requireHittable(rows);
    const auto before = order(app);

    // Where the handle is in Edit mode.
    const auto handle = AppTestAccess::handlePoint(list, 2);
    press(rows, handle, 0, &list);
    drag(rows, handle, gapPoint(list, 6), 0, &list);
    REQUIRE(AppTestAccess::insertionLine(list) == -1);
    release(rows, handle, gapPoint(list, 6), 0, &list);
    app.tick();

    REQUIRE(order(app) == before);
}

TEST_CASE("held at the bottom edge the list scrolls and the drop lands on the line") {
    const ManualClock clock;
    TestApp app(manySteps(40));
    auto& list = AppTestAccess::steps(app.main());
    auto& rows = AppTestAccess::rows(list);
    auto& view = AppTestAccess::view(list);
    requireHittable(rows);
    const int viewH = view.getHeight();
    REQUIRE(AppTestAccess::rowTop(list, 40) > viewH + 400);  // room to scroll
    REQUIRE(view.getViewPositionY() == 0);
    const auto before = order(app);

    const auto handle = AppTestAccess::handlePoint(list, 1);
    const juce::Point<int> edge(kNameX, viewH - 10);  // 10 px above the bottom edge, content = view at scroll 0
    press(rows, handle, 0, &list);
    drag(rows, handle, edge, 0, &list);
    const int gapAtStart = AppTestAccess::insertionLine(list);
    REQUIRE(gapAtStart > 0);

    for (int i = 0; i < 30; ++i) AppTestAccess::scrollTimer(list);  // about a second of resting there

    const int scrolled = view.getViewPositionY();
    REQUIRE(scrolled > 200);
    const int gap = AppTestAccess::insertionLine(list);
    REQUIRE(gap > gapAtStart);  // the line moved with the content under the resting finger
    // The finger has not moved on screen; in content coordinates it is lower by the scroll.
    release(rows, handle, edge + juce::Point<int>(0, scrolled), 0, &list);

    REQUIRE(order(app) == moved(before, 1, movedIndex(1, gap)));
}

TEST_CASE("with Details on and mixed heights the drop lands on the right gap") {
    const ManualClock clock;
    TestApp app(manySteps(6));
    AppTestAccess::setDetails(app.main(), true);
    REQUIRE(app.session().addFadeStep().has_value());  // after NEXT (row 0): row 1, fewer lines
    app.tick(2);
    auto& list = AppTestAccess::steps(app.main());
    auto& rows = AppTestAccess::rows(list);
    requireHittable(rows);
    const auto height = [&](int i) { return AppTestAccess::rowTop(list, i + 1) - AppTestAccess::rowTop(list, i); };
    REQUIRE(height(1) != height(2));  // mixed
    REQUIRE(height(2) > StepList::kRowHeight + StepList::kRowHeight / 2);  // tall enough that 48 px rows would differ
    const auto before = order(app);

    // The bottom half of row 2 (tall): gap 3. With 48 px rows this point would be row 3 or 4.
    const juce::Point<int> to(kNameX, AppTestAccess::rowTop(list, 2) + height(2) * 3 / 4);
    const auto handle = AppTestAccess::handlePoint(list, 0);
    press(rows, handle, 0, &list);
    drag(rows, handle, to, 0, &list);
    REQUIRE(AppTestAccess::insertionLine(list) == 3);
    release(rows, handle, to, 0, &list);

    REQUIRE(order(app) == moved(before, 0, 2));
}

TEST_CASE("file drop inserts at the line in drop order; a text file is refused") {
    const ManualClock clock;
    TestApp app(threeSoundSteps());
    const TempDir temp;
    const auto files = dropFiles(temp);
    auto& list = AppTestAccess::steps(app.main());
    requireHittable(list);
    REQUIRE(names(app) == std::vector<std::string>{"Intro", "Song A", "Song B"});

    const auto at = gapPoint(list, 1);  // the view is at the list's top and not scrolled
    list.fileDragEnter(files, at.x, at.y);
    REQUIRE(AppTestAccess::insertionLine(list) == 1);
    list.filesDropped(files, at.x, at.y);
    REQUIRE(AppTestAccess::insertionLine(list) == -1);
    REQUIRE(app.session().status().message.contains("notes.txt"));

    app.waitUntil([&] { return app.session().controller().show().steps.size() == 5; });
    REQUIRE(names(app) == std::vector<std::string>{"Intro", "Drum roll", "Fanfare", "Song A", "Song B"});
    app.waitUntil([&] { return !app.session().importsRunning(); });  // the conversions end before the app does
}

TEST_CASE("drops wait while not in Edit mode and arrive on return to Edit") {
    const ManualClock clock;
    TestApp app(threeSoundSteps());
    const TempDir temp;
    const auto files = dropFiles(temp);
    auto& list = AppTestAccess::steps(app.main());
    requireHittable(list);
    const auto at = gapPoint(list, 1);

    SECTION("a drop in Rehearsal is refused and promises no line") {
        AppTestAccess::pickMode(app.main(), ShowMode::Rehearsal);
        list.fileDragEnter(files, at.x, at.y);
        REQUIRE(AppTestAccess::insertionLine(list) == -1);
        list.filesDropped(files, at.x, at.y);
        REQUIRE(app.session().status().message.contains("Rehearsal mode"));
        REQUIRE(names(app).size() == 3);
    }
    SECTION("files still being read when Edit mode ends wait for it") {
        list.filesDropped(files, at.x, at.y);
        // The hash thread reports back through the message queue, which has not run yet.
        AppTestAccess::pickMode(app.main(), ShowMode::Rehearsal);
        app.waitUntil([&] { return app.session().status().message.contains("2 dropped files wait for Edit mode"); });
        REQUIRE(names(app).size() == 3);

        AppTestAccess::pickMode(app.main(), ShowMode::Edit);
        REQUIRE(names(app) == std::vector<std::string>{"Intro", "Drum roll", "Fanfare", "Song A", "Song B"});
        app.waitUntil([&] { return !app.session().importsRunning(); });
    }
}

TEST_CASE("Details toggle changes row heights and is stored") {
    const ManualClock clock;
    TestApp app(threeSoundSteps());
    auto& list = AppTestAccess::steps(app.main());
    auto& box = AppTestAccess::detailsBox(AppTestAccess::modeBar(app.main()));
    requireHittable(box);
    REQUIRE(!list.details());
    REQUIRE(!AppTestAccess::settings(list).stepDetails());
    for (int i = 0; i < 3; ++i) REQUIRE(AppTestAccess::rowTop(list, i + 1) - AppTestAccess::rowTop(list, i) == 48);

    tap(box, box.getLocalBounds().getCentre());
    app.tick();

    REQUIRE(box.getToggleState());
    REQUIRE(list.details());
    REQUIRE(AppTestAccess::rowTop(list, 1) - AppTestAccess::rowTop(list, 0) > 48);  // a sound step has lines
    REQUIRE(AppTestAccess::settings(list).stepDetails());
}

// Final review I1: the line goes, its room stays, so no row moves under a finger.
TEST_CASE("the NEXT row keeps its height when its everything-else line goes") {
    const ManualClock clock;
    TestApp app(shortIntro());
    AppTestAccess::setDetails(app.main(), true);
    app.waitHeads();
    auto& list = AppTestAccess::steps(app.main());
    requireHittable(AppTestAccess::rows(list));
    const auto allTops = [&] {
        std::vector<int> t;
        for (int i = 0; i <= 3; ++i) t.push_back(AppTestAccess::rowTop(list, i));
        return t;
    };
    const auto fresh = allTops();
    CHECK_FALSE(fadesElse(list, 0));  // nothing plays: no line on NEXT

    AppTestAccess::go(app.main());  // Intro plays, Song A (fades everything else) is NEXT
    app.audio(2);
    REQUIRE(app.session().controller().nextIndex() == 1);
    REQUIRE(fadesElse(list, 1));
    const auto playing = allTops();
    REQUIRE(playing == fresh);

    app.audio(60);  // Intro ends by itself
    app.tick();

    REQUIRE(app.session().engineForScreenshots().snapshot().numVoices == 0);
    REQUIRE_FALSE(fadesElse(list, 1));
    REQUIRE(allTops() == playing);  // row 1's height and the rows below it did not move
}

// The NEXT row tells what will really be faded, from the engine snapshot.
TEST_CASE("the NEXT row's line disappears when the song ends by itself") {
    const ManualClock clock;
    TestApp app(shortIntro());
    AppTestAccess::setDetails(app.main(), true);
    app.waitHeads();
    auto& list = AppTestAccess::steps(app.main());
    AppTestAccess::go(app.main());  // Intro plays
    app.audio(2);
    tapRow(list, 2);  // Song B is NEXT; it fades everything else
    app.tick();
    REQUIRE(app.session().controller().nextIndex() == 2);
    REQUIRE(fadesElse(list, 2));  // Intro would be faded
    REQUIRE(fadesElse(list, 1));

    app.audio(60);  // about 0.64 s: Intro has ended by itself
    app.tick();

    REQUIRE(app.session().engineForScreenshots().snapshot().numVoices == 0);
    CHECK_FALSE(fadesElse(list, 2));
    CHECK(fadesElse(list, 1));  // not NEXT: the list walk still counts Intro
}

TEST_CASE("the NEXT row is reworded only when the running set changes") {
    const ManualClock clock;
    TestApp app(shortIntro());
    AppTestAccess::setDetails(app.main(), true);
    app.waitHeads();
    auto& list = AppTestAccess::steps(app.main());
    requireHittable(list);
    AppTestAccess::go(app.main());
    app.audio(2);
    const int started = AppTestAccess::describeCount(list);

    app.tick(4);
    app.audio(2);  // Intro moves on, the set stays {Intro}
    REQUIRE(AppTestAccess::describeCount(list) == started);

    tapRow(list, 2);  // NEXT changes
    app.tick();
    const int picked = AppTestAccess::describeCount(list);
    REQUIRE(picked > started);
    app.tick(4);
    REQUIRE(AppTestAccess::describeCount(list) == picked);

    app.audio(60);  // Intro ends: the set becomes empty
    REQUIRE(AppTestAccess::describeCount(list) > picked);
}

TEST_CASE("rows with a running sound are tinted, NEXT keeps its look") {
    ManualClock clock;
    TestApp app(threeSoundSteps());
    app.waitHeads();
    auto& main = app.main();
    auto& list = AppTestAccess::steps(main);
    AppTestAccess::go(main);  // row 0 plays, row 1 is NEXT, row 2 is neither
    app.audio(5);
    app.tick();
    auto& rows = AppTestAccess::rows(list);
    const auto pixel = [&](int x, int row) {
        const auto image = rows.createComponentSnapshot(rows.getLocalBounds(), true, 1.0f);
        return image.getPixelAt(x, AppTestAccess::rowTop(list, row) + 2);
    };
    const juce::Colour tint(kPlayingTint);
    CHECK(pixel(2, 0) == tint);
    CHECK(pixel(rows.getWidth() / 2, 0) == tint);
    CHECK(pixel(2, 1) != tint);  // NEXT, not playing
    CHECK(pixel(2, 2) != tint);
    // Playing and NEXT: the NEXT colour, with the tint as a bar at the left edge.
    app.session().controller().selectNext(0);
    app.tick();
    CHECK(pixel(2, 0) == tint);
    CHECK(pixel(rows.getWidth() / 2, 0) != tint);
}

}  // namespace bigtop::app

namespace bigtop::app {
namespace {
// Advances past one check interval and waits for the check to finish.
void runFileCheck(TestApp& app, ManualClock& clock) {
    clock.advance(ShowSession::kFileCheckMs);
    app.tick();
    app.waitUntil([&] { return !app.session().fileCheckRunning(); });
    app.tick();
}
}  // namespace

TEST_CASE("A file removed while the show is open is flagged within one check, and clears when it returns") {
    ManualClock clock;
    TestApp app(threeSoundSteps());
    app.waitHeads();
    runFileCheck(app, clock);  // baseline
    const auto file = app.session().showDir().getChildFile("audio/sound2.flac");
    const auto aside = file.getSiblingFile("aside.bin");
    REQUIRE(file.moveFileTo(aside));
    runFileCheck(app, clock);
    CHECK(app.session().fileProblem(2));
    const auto a = app.session().currentAlert();
    REQUIRE(a.has_value());
    CHECK(a->text == "Step 2 'Song A': file missing");
    REQUIRE(aside.moveFileTo(file));
    runFileCheck(app, clock);
    CHECK_FALSE(app.session().fileProblem(2));
}

TEST_CASE("A file whose size changes counts as a problem") {
    ManualClock clock;
    TestApp app(threeSoundSteps());
    app.waitHeads();
    runFileCheck(app, clock);
    REQUIRE(app.session().showDir().getChildFile("audio/sound3.flac").appendText("junk"));
    runFileCheck(app, clock);
    CHECK(app.session().fileProblem(3));
    const auto a = app.session().currentAlert();
    REQUIRE(a.has_value());
    CHECK(a->text == "Step 3 'Song B': file changed");
}

TEST_CASE("Only one file check runs at a time") {
    ManualClock clock;
    TestApp app(threeSoundSteps());
    app.waitHeads();
    const int before = app.fileChecks();
    app.holdFileChecks(true);
    app.session().checkFilesNow();
    app.waitUntil([&] { return app.fileChecks() == before + 1; });
    app.session().checkFilesNow();
    clock.advance(ShowSession::kFileCheckMs);
    app.tick();
    CHECK(app.fileChecks() == before + 1);
    app.holdFileChecks(false);
    app.waitUntil([&] { return !app.session().fileCheckRunning(); });
}

TEST_CASE("A file check result for an old show is dropped") {
    ManualClock clock;
    TestApp app(threeSoundSteps());
    app.waitHeads();
    runFileCheck(app, clock);
    REQUIRE(app.session().showDir().getChildFile("audio/sound2.flac").deleteFile());
    app.holdFileChecks(true);
    const int before = app.fileChecks();
    app.session().checkFilesNow();
    app.waitUntil([&] { return app.fileChecks() == before + 1; });
    std::string error;
    TempDir other;
    REQUIRE(app.session().createNew(other.dir.getChildFile("Other show"), error));
    app.holdFileChecks(false);
    app.pumpMessages(200);  // the old check finishes and posts its result
    CHECK_FALSE(app.session().fileProblem(2));
    CHECK_FALSE(app.session().currentAlert().has_value());
}

}  // namespace bigtop::app

namespace bigtop::app {

TEST_CASE("A failed import is retried from its kept original, from the row's Retry button") {
    ManualClock clock;
    Show show = threeSoundSteps();
    show.files[1].sourcePath = "sources/b.flac";  // Song A's original, kept in the show folder
    TestApp app(show);
    app.waitHeads();
    const auto dir = app.session().showDir();
    const auto original = dir.getChildFile("sources/b.flac");
    REQUIRE(original.getParentDirectory().createDirectory().wasOk());
    REQUIRE(original.replaceWithText("not audio"));
    CHECK_FALSE(app.session().retryImport(2));  // a file that plays is never touched
    REQUIRE(dir.getChildFile("audio/sound2.flac").deleteFile());
    runFileCheck(app, clock);  // baseline taken before: now flagged missing
    runFileCheck(app, clock);
    REQUIRE(app.session().fileProblem(2));
    REQUIRE(app.session().retryImport(2));
    app.waitUntil([&] { return !app.session().importing(2); });
    REQUIRE(app.session().importError(2).isNotEmpty());  // a broken original fails: the row is red
    app.tick();

    REQUIRE(dir.getChildFile("audio/sound1.flac").copyFileTo(original));  // a good original now
    auto& list = AppTestAccess::steps(app.main());
    auto& rows = AppTestAccess::rows(list);
    requireHittable(rows);
    const juce::Point<int> at(StepList::retryX(rows.getWidth()) + 10, AppTestAccess::rowTop(list, 1) + 24);
    press(rows, at, 0, &list);
    release(rows, at, at, 0, &list);
    REQUIRE(app.session().importing(2));
    app.waitUntil([&] { return !app.session().importing(2); });
    INFO(app.session().importError(2).toStdString());
    CHECK(app.session().importError(2).isEmpty());
    app.waitHeads();
    CHECK(app.session().sources().fileUsable(2));
}

TEST_CASE("Retry without a kept original says so") {
    ManualClock clock;
    TestApp app(threeSoundSteps());
    app.waitHeads();
    runFileCheck(app, clock);
    REQUIRE(app.session().showDir().getChildFile("audio/sound2.flac").deleteFile());
    runFileCheck(app, clock);
    REQUIRE(app.session().fileProblem(2));
    CHECK_FALSE(app.session().retryImport(2));
    CHECK(app.session().status().message == "No copy of the original is kept: drop the file again");
}

}  // namespace bigtop::app

namespace bigtop::app {

TEST_CASE("The drop line hides while a drag is outside the list and comes back inside") {
    const ManualClock clock;
    TestApp app(manySteps(10));
    auto& list = AppTestAccess::steps(app.main());
    auto& rows = AppTestAccess::rows(list);
    requireHittable(rows);
    const auto handle = AppTestAccess::handlePoint(list, 2);
    press(rows, handle, 0, &list);
    drag(rows, handle, gapPoint(list, 4), 0, &list);
    REQUIRE(AppTestAccess::insertionLine(list) == 4);
    drag(rows, handle, {-300, gapPoint(list, 4).y}, 0, &list);  // the finger slid off the window
    CHECK(AppTestAccess::insertionLine(list) == -1);
    drag(rows, handle, gapPoint(list, 5), 0, &list);
    CHECK(AppTestAccess::insertionLine(list) == 5);
    release(rows, handle, gapPoint(list, 5), 0, &list);
}

TEST_CASE("A drag keeps its step when a row is added above it mid-drag") {
    const ManualClock clock;
    TestApp app(manySteps(10));
    auto& list = AppTestAccess::steps(app.main());
    auto& rows = AppTestAccess::rows(list);
    requireHittable(rows);
    const StepId carried = order(app)[3];
    const auto handle = AppTestAccess::handlePoint(list, 3);
    press(rows, handle, 0, &list);
    drag(rows, handle, gapPoint(list, 7), 0, &list);
    REQUIRE(app.session().addFadeStep().has_value());  // lands after NEXT (row 0): above the carried row
    app.tick();
    release(rows, handle, gapPoint(list, 7), 0, &list);
    const auto after = order(app);
    const auto at = std::find(after.begin(), after.end(), carried) - after.begin();
    CHECK(at == 6);  // in the gap the line showed: rows 0-6 above it are the six that were and the fade step
}

}  // namespace bigtop::app

namespace bigtop::app {

TEST_CASE("Held Down moves one row, then at most 8 rows per second after 400 ms") {
    ManualClock clock;
    TestApp app(manySteps(40));
    auto& main = app.main();
    auto& controller = app.session().controller();
    controller.selectNext(0);
    for (int t = 0; t <= 1400; t += 30) {  // the OS repeats a held key about every 30 ms
        REQUIRE(main.keyPressed(juce::KeyPress(juce::KeyPress::downKey)));
        clock.advance(30);
    }
    CHECK(controller.nextIndex() >= 2);
    CHECK(controller.nextIndex() <= 9);
}

TEST_CASE("Deleting a playing step asks first; Cancel keeps it, Delete keeps its sound playing") {
    ManualClock clock;
    TestApp app(threeSoundSteps());
    app.waitHeads();
    auto& main = app.main();
    AppTestAccess::go(main);  // step 1 plays, NEXT is step 2
    app.audio(4);
    app.session().controller().selectNext(0);
    REQUIRE(main.keyPressed(juce::KeyPress(juce::KeyPress::deleteKey)));
    auto* box = dynamic_cast<juce::AlertWindow*>(juce::Component::getCurrentlyModalComponent());
    REQUIRE(box != nullptr);
    CHECK(box->getHelpText() == "Step is playing. Delete it? The sound plays on.");
    box->exitModalState(0);
    app.pumpMessages(50);
    CHECK(app.session().controller().show().steps.size() == 3);
    REQUIRE(main.keyPressed(juce::KeyPress(juce::KeyPress::deleteKey)));
    box = dynamic_cast<juce::AlertWindow*>(juce::Component::getCurrentlyModalComponent());
    REQUIRE(box != nullptr);
    box->exitModalState(1);
    app.pumpMessages(50);
    CHECK(app.session().controller().show().steps.size() == 2);
    app.audio(2);
    CHECK(AppTestAccess::running(main).rows() == 1);  // its running row stays until the sound ends
    auto& row = AppTestAccess::row(main, 1);
    CHECK(AppTestAccess::nameOf(row).getText() == "Intro");
}

TEST_CASE("Deleting a silent step asks nothing") {
    const ManualClock clock;
    TestApp app(threeSoundSteps());
    REQUIRE(app.main().keyPressed(juce::KeyPress(juce::KeyPress::deleteKey)));
    CHECK(juce::Component::getCurrentlyModalComponent() == nullptr);
    CHECK(app.session().controller().show().steps.size() == 2);
}

}  // namespace bigtop::app

namespace bigtop::app {

TEST_CASE("The editor's Delete step asks first when the step is playing") {
    ManualClock clock;
    TestApp app(threeSoundSteps());
    app.waitHeads();
    AppTestAccess::go(app.main());
    app.audio(4);
    AppTestAccess::openEditor(app.main(), 1);
    app.tick();
    auto& del = button(AppTestAccess::editor(app.main()), "Delete step");
    REQUIRE(del.onClick != nullptr);
    del.onClick();
    auto* box = dynamic_cast<juce::AlertWindow*>(juce::Component::getCurrentlyModalComponent());
    REQUIRE(box != nullptr);
    box->exitModalState(0);
    app.pumpMessages(50);
    CHECK(app.session().controller().show().steps.size() == 3);
}

}  // namespace bigtop::app
