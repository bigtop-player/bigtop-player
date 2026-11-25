// ShowEditsTests.cpp: the model edit operations (ids, files, insert/move/remove).
#include <catch2/catch_test_macros.hpp>

#include <limits>
#include <vector>

#include "bigtop/Engine.h"  // kMaxChanges
#include "bigtop/ShowEdits.h"

using namespace bigtop;

namespace {
std::vector<StepId> ids(const Show& s) {
    std::vector<StepId> out;
    for (const Step& st : s.steps) out.push_back(st.id);
    return out;
}
Show threeSteps() {
    Show s;
    for (int i = 0; i < 3; ++i) s.steps.push_back(makeStep(*allocateStepId(s), s.settings, std::nullopt));
    return s;
}

// The owner's reduced show, in list order 2, 4, 3: step 2 starts the atmo at
// -12.5 dB, step 4 (no sound) moves it to -3 dB, step 3 starts music and moves
// the atmo to -4 dB.
Show atmoShow() {
    Show s;
    s.steps.push_back(makeStep(2, s.settings, Sound{.file = 2, .levelDb = -12.5}));
    Step fade = makeStep(4, s.settings, std::nullopt);
    setChange(fade, {2, ChangeKind::Level, -3.0, 2.0});
    s.steps.push_back(fade);
    Step music = makeStep(3, s.settings, Sound{.file = 1});
    setChange(music, {2, ChangeKind::Level, -4.0, 2.0});
    s.steps.push_back(music);
    return s;
}
}  // namespace

TEST_CASE("Keep after a fade step updates that step's level change", "[edits]") {
    Show s = atmoShow();
    CHECK(keepIntoShow(s, 2, 4, -6.0));                  // step 4 set the level heard
    CHECK(s.steps[1].changes[0].levelDb == -9.0);
    CHECK(s.steps[0].sound->levelDb == -12.5);
    CHECK(s.steps[2].changes[0].levelDb == -4.0);
}
TEST_CASE("Keep with no later change updates the start level", "[edits]") {
    Show s = atmoShow();
    CHECK(keepIntoShow(s, 2, 2, -6.0));                  // setter = the start step itself
    CHECK(s.steps[0].sound->levelDb == -18.5);
    CHECK(s.steps[1].changes[0].levelDb == -3.0);
    s = atmoShow();
    CHECK(keepIntoShow(s, 2, 0, -6.0));                  // unknown setter
    CHECK(s.steps[0].sound->levelDb == -18.5);
    s = atmoShow(); setChange(s.steps[1], {2, ChangeKind::FadeOut, 0.0, 2.0});
    CHECK(keepIntoShow(s, 2, 4, -6.0));                  // the setter's change is no longer a Level change
    CHECK(s.steps[0].sound->levelDb == -18.5);
    s = atmoShow(); removeChange(s.steps[2], 2);
    CHECK(keepIntoShow(s, 2, 3, -6.0));                  // the setter lost its change: do not borrow an older step's
    CHECK(s.steps[0].sound->levelDb == -18.5);
    CHECK(s.steps[1].changes[0].levelDb == -3.0);
    s = atmoShow();
    CHECK(keepIntoShow(s, 2, 99, -6.0));                 // the setter step was deleted
    CHECK(s.steps[0].sound->levelDb == -18.5);
}
TEST_CASE("Keep updates the setter's change, not the newest in the list", "[edits]") {
    Show s = atmoShow();
    CHECK(keepIntoShow(s, 2, 3, -6.0));
    CHECK(s.steps[2].changes[0].levelDb == -10.0);
    CHECK(s.steps[1].changes[0].levelDb == -3.0);
}
TEST_CASE("Keep moves only the setter's slot", "[edits]") {
    Show s = atmoShow();
    CHECK(keepIntoShow(s, 2, 4, -6.0));
    CHECK(s.steps[1].changes[0].levelDb == -9.0);        // the intended slot moved ...
    CHECK(s.steps[2].changes[0].levelDb == -4.0);        // ... and step 3, not run yet, did not
}
TEST_CASE("Keep finds the sound's change among changes for other sounds", "[edits]") {
    Show s = atmoShow();
    Step other = makeStep(5, s.settings, std::nullopt);
    setChange(other, {3, ChangeKind::Level, -1.0, 1.0});
    setChange(other, {2, ChangeKind::Level, -7.0, 1.0});
    s.steps.push_back(other);
    CHECK(keepIntoShow(s, 2, 5, -2.0));
    CHECK(s.steps[3].changes[1].levelDb == -9.0);
    CHECK(s.steps[3].changes[0].levelDb == -1.0);
}
TEST_CASE("Keep into the show refuses bad input and leaves the show alone", "[edits]") {
    Show s = atmoShow(); const Show before = s;
    CHECK_FALSE(keepIntoShow(s, 99, 4, -6.0));           // unknown sound
    CHECK_FALSE(keepIntoShow(s, 4, 4, -6.0));            // start step without a sound
    CHECK_FALSE(keepIntoShow(s, 2, 4, std::numeric_limits<double>::infinity()));
    CHECK(s.steps[0].sound->levelDb == before.steps[0].sound->levelDb);
    CHECK(s.steps[1].changes == before.steps[1].changes);
}

TEST_CASE("Step ids are never reused, even after deleting the newest step", "[edits]") {
    Show s;
    auto a = *allocateStepId(s);
    s.steps.push_back(makeStep(a, s.settings, std::nullopt));
    CHECK(removeStep(s, a));
    CHECK(*allocateStepId(s) == a + 1);
}
TEST_CASE("allocateStepId also clears ids already in the steps and stops at kMaxStepId", "[edits]") {
    Show s; s.steps.push_back(makeStep(7, s.settings, std::nullopt));      // lastStepId still 0
    CHECK(*allocateStepId(s) == 8);
    s.lastStepId = kMaxStepId;
    CHECK_FALSE(allocateStepId(s));
    CHECK(s.lastStepId == kMaxStepId);
}
TEST_CASE("A file with a known hash is found; an empty hash never matches", "[edits]") {
    Show s;
    FileId f = addFile(s, "audio/a.flac", "ff00");
    FileId g = addFile(s, "audio/b.flac", "");
    CHECK(f == 1); CHECK(g == 2);
    CHECK(findFileByHash(s, "ff00", 1) == f);
    CHECK_FALSE(findFileByHash(s, "ee", 1));
    CHECK_FALSE(findFileByHash(s, "", 1));
}
TEST_CASE("insert, move and remove keep the other steps in order", "[edits]") {
    Show s = threeSteps();
    const auto files = s.files;
    insertSteps(s, 1, {makeStep(4, s.settings, std::nullopt), makeStep(5, s.settings, std::nullopt)});
    CHECK(ids(s) == std::vector<StepId>{1, 4, 5, 2, 3});
    CHECK(moveStep(s, 1, 4));
    CHECK(ids(s) == std::vector<StepId>{4, 5, 2, 3, 1});
    CHECK(removeStep(s, 5));
    CHECK(ids(s) == std::vector<StepId>{4, 2, 3, 1});
    CHECK_FALSE(removeStep(s, 99));
    CHECK_FALSE(moveStep(s, 99, 0));
    CHECK(s.files == files);
}
TEST_CASE("Insert and move indices are clamped", "[edits]") {
    Show s = threeSteps();
    insertSteps(s, -5, {makeStep(4, s.settings, std::nullopt)});
    insertSteps(s, 100, {makeStep(5, s.settings, std::nullopt)});
    CHECK(ids(s) == std::vector<StepId>{4, 1, 2, 3, 5});
    CHECK(moveStep(s, 4, 100)); CHECK(ids(s) == std::vector<StepId>{1, 2, 3, 5, 4});
    CHECK(moveStep(s, 4, -1));  CHECK(ids(s) == std::vector<StepId>{4, 1, 2, 3, 5});
}

namespace {
// The path the app would choose: one per new file, numbered by the files already in the show.
std::string numberedPath(const DroppedFile& d, const Show& s) {
    return "audio/" + d.name + std::to_string(s.files.size()) + ".flac";
}
}  // namespace

TEST_CASE("Dropped files become steps at the drop position, in drop order, named by the file", "[edits][drop]") {
    Show s = threeSteps();
    const auto out = addDroppedFiles(s, 1, {{"Intro", "aa"}, {"Walk-in", "bb"}}, 1, numberedPath);
    REQUIRE(out.size() == 2);
    REQUIRE(s.steps.size() == 5);
    CHECK(s.steps[1].id == out[0].step);
    CHECK(s.steps[2].id == out[1].step);
    CHECK(s.steps[1].name == "Intro");
    CHECK(s.steps[2].name == "Walk-in");
    Step expected = makeStep(out[0].step, s.settings, Sound{.file = out[0].file});
    expected.name = "Intro";
    CHECK(s.steps[1] == expected);  // makeStep defaults otherwise
    CHECK(out[0].newFile);
    CHECK(out[1].newFile);
    REQUIRE(s.files.size() == 2);
    CHECK(s.files[0].path == "audio/Intro0.flac");  // the chooser sees the files added before it
    CHECK(s.files[1].path == "audio/Walk-in1.flac");
    CHECK(s.files[1].sourceHash == "bb");
}

TEST_CASE("A dropped file already in the show, or twice in one drop, is stored once", "[edits][drop]") {
    Show s;
    const FileId known = addFile(s, "audio/known.flac", "aa");
    const auto out = addDroppedFiles(s, 0, {{"Known", "aa"}, {"New", "bb"}, {"New again", "bb"}}, 1, numberedPath);
    REQUIRE(out.size() == 3);
    CHECK(out[0].file == known);
    CHECK_FALSE(out[0].newFile);
    CHECK(out[1].newFile);
    CHECK(out[2].file == out[1].file);
    CHECK_FALSE(out[2].newFile);
    CHECK(s.files.size() == 2);
    CHECK(s.steps.size() == 3);
}

TEST_CASE("Dropped files beyond the last step id are not added, and add no file", "[edits][drop]") {
    Show s;
    s.lastStepId = kMaxStepId - 1;
    const auto out = addDroppedFiles(s, 0, {{"A", "aa"}, {"B", "bb"}}, 1, numberedPath);
    REQUIRE(out.size() == 2);
    CHECK(out[0].step == kMaxStepId);
    CHECK(out[1].step == 0);
    CHECK(out[1].file == 0);
    CHECK(s.steps.size() == 1);
    CHECK(s.files.size() == 1);
}

// Value checks, change set/remove, pad grid, kept trim, import version.
TEST_CASE("checkedEdit rejects non-finite values and clamps the rest", "[edits]") {
    const double inf = std::numeric_limits<double>::infinity();
    CHECK_FALSE(checkedEdit(std::numeric_limits<double>::quiet_NaN(), -60, 12));
    CHECK_FALSE(checkedEdit(inf, -60, 12));
    CHECK_FALSE(checkedEdit(-inf, -60, 12));
    CHECK(*checkedEdit(13, -60, 12) == 12.0);
    CHECK(*checkedEdit(-3, -60, 12) == -3.0);
}
TEST_CASE("setChange replaces by target, appends otherwise and stops at kMaxChanges", "[edits]") {
    Step st;
    CHECK(setChange(st, {5, ChangeKind::Level, -6.0, 1.0}));
    CHECK(setChange(st, {5, ChangeKind::FadeOut, 0.0, 2.0}));
    REQUIRE(st.changes.size() == 1); CHECK(st.changes[0].kind == ChangeKind::FadeOut);
    for (StepId t = 6; t < 6 + kMaxChanges - 1; ++t) CHECK(setChange(st, {t, ChangeKind::Keep}));
    CHECK(st.changes.size() == static_cast<size_t>(kMaxChanges));
    CHECK_FALSE(setChange(st, {99, ChangeKind::Keep}));
    CHECK(setChange(st, {5, ChangeKind::Keep}));                       // replacing still works when full
    CHECK_FALSE(setChange(st, {0, ChangeKind::Keep}));
    CHECK_FALSE(setChange(st, {kMaxStepId + 1, ChangeKind::Keep}));
    CHECK_FALSE(setChange(st, {5, ChangeKind::Level, std::numeric_limits<double>::infinity(), 1.0}));
    CHECK_FALSE(setChange(st, {5, ChangeKind::Level, 0.0, std::numeric_limits<double>::quiet_NaN()}));
}
TEST_CASE("removeChange drops the target and ignores a missing one", "[edits]") {
    Step st; setChange(st, {5, ChangeKind::Keep}); setChange(st, {6, ChangeKind::Keep});
    removeChange(st, 9); CHECK(st.changes.size() == 2);
    removeChange(st, 5); REQUIRE(st.changes.size() == 1); CHECK(st.changes[0].target == 6);
}
TEST_CASE("resizePadGrid clamps and grows the pad vector but never shrinks it", "[edits]") {
    Show s; CHECK(s.pads.size() == 8);
    resizePadGrid(s, 2, 2); CHECK(s.pads.size() == 8); CHECK(s.settings.padRows == 2); CHECK(s.settings.padCols == 2);
    resizePadGrid(s, 6, 4); CHECK(s.pads.size() == 24);
    resizePadGrid(s, 1, 1); CHECK(s.pads.size() == 24);
    resizePadGrid(s, 9, 9); CHECK(s.settings.padRows == 6); CHECK(s.settings.padCols == 4);
    resizePadGrid(s, 0, -3); CHECK(s.settings.padRows == 1); CHECK(s.settings.padCols == 1);
}
TEST_CASE("keptLevelDb adds the live trim, clamped; a non-finite trim is ignored", "[edits]") {
    CHECK(keptLevelDb(10, 6) == 12.0);
    CHECK(keptLevelDb(-5, 2) == -3.0);
    CHECK(keptLevelDb(-5, std::numeric_limits<double>::quiet_NaN()) == -5.0);
}
TEST_CASE("findFileByHash ignores entries imported with an older version", "[edits]") {
    Show s; addFile(s, "a.flac", "ff"); s.files[0].importVersion = 1;
    CHECK(findFileByHash(s, "ff", 1)); CHECK_FALSE(findFileByHash(s, "ff", 2));
}

TEST_CASE("A drop stamps new files with the import version and re-imports older ones", "[edits][drop]") {
    Show s;
    addFile(s, "audio/old.flac", "aa");  // imported by version 1
    const auto out = addDroppedFiles(s, 0, {{"Old", "aa"}}, 2, numberedPath);
    REQUIRE(out.size() == 1);
    CHECK(out[0].newFile);  // the version-1 conversion is not reused
    REQUIRE(s.files.size() == 2);
    CHECK(s.files[1].importVersion == 2);
    // now current: the same content is merged
    CHECK_FALSE(addDroppedFiles(s, 0, {{"Old", "aa"}}, 2, numberedPath)[0].newFile);
}
