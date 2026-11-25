// StepDetailsTests.cpp: the wording of the step list's Details lines.
// Every element read is behind a REQUIRE on the size, so a failure never indexes past the end.
#include <catch2/catch_test_macros.hpp>

#include "bigtop/StepDetails.h"

using namespace bigtop;

namespace {

// UTF-8 pieces of the wording, spelled once so the expectations stay readable.
const std::string kDot = " \xc2\xb7 ";      // " · "
const std::string kArrow = " \xe2\x86\x92 ";  // " → "
const std::string kMinus = "\xe2\x88\x92";   // "−"
const std::string kDash = "\xe2\x80\x93";    // "–"

// A sound step that keeps everything else, so its sound stays running for the later steps.
Step keeping(StepId id, const char* name, FileId file, const Settings& settings) {
    Step step = makeStep(id, settings, Sound{.file = file});
    step.name = name;
    step.others = Others{};
    return step;
}

// Invented demo show: three sound steps that keep each other running: Atmo (1), Music A (2), Crowd (3).
Show demo() {
    Show show;
    show.files = {{1, "audio/Atmo.flac", 0.0, ""}, {2, "audio/Music A.flac", 0.0, ""}, {3, "audio/Crowd.flac", 0.0, ""}};
    show.steps = {keeping(1, "Atmo", 1, show.settings), keeping(2, "Music A", 2, show.settings),
                  keeping(3, "Crowd", 3, show.settings)};
    show.lastStepId = 3;
    return show;
}

// Appends @p step to the show and describes it there (after every step before it).
std::vector<DetailLine> describeAppended(Show& show, const Step& step, bool fileMissing = false) {
    show.steps.push_back(step);
    return describeStep(show, show.steps.size() - 1, fileMissing);
}

std::vector<std::string> texts(const std::vector<DetailLine>& lines) {
    std::vector<std::string> out;
    for (const auto& l : lines) out.push_back(l.text);
    return out;
}

// A fade step (no sound) with the given changes and everything else fading over 1 s.
Step fadeStep(StepId id, std::vector<Change> changes) {
    Step step = makeStep(id, Settings{}, std::nullopt);
    step.changes = std::move(changes);
    step.others = Others{ChangeKind::FadeOut, 1.0};
    return step;
}

const std::string kElse = "everything else fades out in 1.0 s";

}  // namespace

TEST_CASE("formatDecimal keeps one or two decimals like the editor's boxes") {
    CHECK(formatDecimal(2.5) == "2.5");
    CHECK(formatDecimal(3.0) == "3.0");
    CHECK(formatDecimal(1.25) == "1.25");
    CHECK(formatDecimal(0.0) == "0.0");
    CHECK(formatDecimal(-6.0) == "-6.0");
    CHECK(formatDecimal(-0.001) == "0.0");  // rounds to zero: no "-0.0"
    CHECK(formatDecimal(1.996) == "2.0");
}

TEST_CASE("formatMinSec reads m:ss.d and rounds into the next minute") {
    CHECK(formatMinSec(12.0) == "0:12.0");
    CHECK(formatMinSec(150.0) == "2:30.0");
    CHECK(formatMinSec(5.25) == "0:05.3");
    CHECK(formatMinSec(59.97) == "1:00.0");  // not "0:60.0"
    CHECK(formatMinSec(0.0) == "0:00.0");
    CHECK(formatMinSec(3725.0) == "62:05.0");  // minutes do not wrap at an hour
}

TEST_CASE("describeStep: a sound step with defaults shows only the file name") {
    const Show show = demo();
    const auto plain = describeStep(show, 0);
    REQUIRE(plain.size() == 1);
    CHECK(plain[0].text == "Atmo.flac");
    CHECK_FALSE(plain[0].missingFile);
}

TEST_CASE("describeStep: an index past the list gives no lines") {
    const Show show = demo();
    CHECK(describeStep(show, 3).empty());
    CHECK(describeStep(show, 99).empty());
}

TEST_CASE("describeStep: every sound field that is not at its default") {
    Show show = demo();
    Step step = makeStep(4, show.settings, Sound{.file = 2,
                                                 .levelDb = 2.0,
                                                 .fadeInSeconds = 1.5,
                                                 .startSeconds = 12.0,
                                                 .endSeconds = 150.0,
                                                 .loop = true,
                                                 .startDelaySeconds = 0.5,
                                                 .timecodeStart = Timecode{1, 0, 0, 0},
                                                 .ontimeCue = "Opening"});
    step.others = Others{};
    const auto lines = describeAppended(show, step);
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].text == "Music A.flac" + kDot + "+2.0 dB" + kDot + "fade in 1.5 s" + kDot + "wait 0.5 s" + kDot +
                               "0:12.0" + kDash + "2:30.0" + kDot + "loop" + kDot + "TC 01:00:00:00" + kDot +
                               "OnTime \"Opening\"");
}

TEST_CASE("describeStep: a range shows when only the start or only the end is set") {
    Show show = demo();
    Step step = makeStep(4, show.settings, Sound{.file = 1, .levelDb = -3.0, .startSeconds = 12.0});
    step.others = Others{};
    const auto open = describeAppended(show, step);
    REQUIRE(open.size() == 1);
    CHECK(open[0].text == "Atmo.flac" + kDot + kMinus + "3.0 dB" + kDot + "0:12.0" + kDash + "end");
    show.steps.back().sound->startSeconds = 0.0;
    show.steps.back().sound->endSeconds = 150.0;
    const auto closed = describeStep(show, show.steps.size() - 1);
    REQUIRE(closed.size() == 1);
    CHECK(closed[0].text == "Atmo.flac" + kDot + kMinus + "3.0 dB" + kDot + "0:00.0" + kDash + "2:30.0");
}

TEST_CASE("describeStep: Level and FadeOut lines are named by their target step; Keep gets none") {
    Show show = demo();
    Step fade = makeStep(4, show.settings, std::nullopt);
    fade.changes = {{1, ChangeKind::Level, -12.0, 2.0}, {2, ChangeKind::Keep, 0.0, 0.0}, {2, ChangeKind::FadeOut, 0.0, 5.0}};
    CHECK(texts(describeAppended(show, fade)) ==
          std::vector<std::string>{"Atmo" + kArrow + kMinus + "12.0 dB in 2.0 s", "Music A fades out in 5.0 s"});
}

TEST_CASE("describeStep: 0 s reads immediately") {
    Show show = demo();
    Step fade = makeStep(4, show.settings, std::nullopt);
    fade.changes = {{1, ChangeKind::Level, 0.0, 0.0}, {2, ChangeKind::FadeOut, 0.0, 0.0}};
    fade.others = Others{ChangeKind::FadeOut, 0.0};  // Crowd is still running and not named
    CHECK(texts(describeAppended(show, fade)) == std::vector<std::string>{"Atmo" + kArrow + "0.0 dB immediately",
                                                                          "Music A fades out immediately",
                                                                          "everything else fades out immediately"});
}

TEST_CASE("describeStep: the everything-else line only when it fades") {
    Show show = demo();
    Step step = makeStep(4, show.settings, Sound{.file = 1});
    step.others = Others{ChangeKind::FadeOut, 3.0};
    CHECK(texts(describeAppended(show, step)) == std::vector<std::string>{"Atmo.flac", "everything else fades out in 3.0 s"});
    show.steps.back().others = Others{};  // keep playing: nothing to say
    CHECK(texts(describeStep(show, show.steps.size() - 1)) == std::vector<std::string>{"Atmo.flac"});
}

TEST_CASE("describeStep: a fade step with nothing to do has no lines") {
    Show show = demo();
    Step fade = makeStep(4, show.settings, std::nullopt);
    CHECK(describeAppended(show, fade).empty());
    show.steps.back().changes = {{1, ChangeKind::Keep, 0.0, 0.0}};  // a Keep says nothing either
    CHECK(describeStep(show, show.steps.size() - 1).empty());
}

TEST_CASE("describeStep: a change whose target step is gone, or unnamed, still reads") {
    Show show = demo();
    show.steps[1].name.clear();
    Step fade = makeStep(4, show.settings, std::nullopt);
    fade.changes = {{99, ChangeKind::FadeOut, 0.0, 1.0}, {2, ChangeKind::FadeOut, 0.0, 1.0}};
    CHECK(texts(describeAppended(show, fade)) ==
          std::vector<std::string>{"a deleted step fades out in 1.0 s", "step 2 fades out in 1.0 s"});
}

TEST_CASE("describeStep: a missing file is flagged on the sound line only") {
    Show show = demo();
    Step step = makeStep(4, show.settings, Sound{.file = 1});  // fades the three others: two lines
    const auto missing = describeAppended(show, step, true);
    REQUIRE(missing.size() == 2);
    CHECK(missing[0].missingFile);
    CHECK_FALSE(missing[1].missingFile);
    const auto usable = describeStep(show, show.steps.size() - 1);
    REQUIRE(usable.size() == 2);
    CHECK_FALSE(usable[0].missingFile);
    show.steps.back().sound->file = 42;  // not in Show::files at all
    const auto unknown = describeStep(show, show.steps.size() - 1);
    REQUIRE(unknown.size() == 2);
    CHECK(unknown[0].missingFile);
    CHECK(unknown[0].text == "unknown file");
}

// The everything-else line says only what would really happen: the list is walked
// in order up to the step, and it shows only when a running sound is neither named nor the step's own.

TEST_CASE("describeStep: the first step has nothing to fade") {
    Show show;
    show.files = {{1, "audio/Atmo.flac", 0.0, ""}};
    const auto lines = describeAppended(show, makeStep(1, show.settings, Sound{.file = 1}));  // fades "the others"
    CHECK(texts(lines) == std::vector<std::string>{"Atmo.flac"});
}

TEST_CASE("describeStep: no everything-else line when every running sound is named") {
    Show show = demo();  // Atmo, Music A and Crowd all running
    const auto lines = describeAppended(show, fadeStep(4, {{1, ChangeKind::Level, -6.0, 2.0},
                                                           {2, ChangeKind::FadeOut, 0.0, 2.0},
                                                           {3, ChangeKind::Keep, 0.0, 0.0}}));
    CHECK(texts(lines) == std::vector<std::string>{"Atmo" + kArrow + kMinus + "6.0 dB in 2.0 s", "Music A fades out in 2.0 s"});
}

TEST_CASE("describeStep: the everything-else line when one running sound is not named") {
    Show show = demo();
    const auto lines = describeAppended(show, fadeStep(4, {{1, ChangeKind::FadeOut, 0.0, 2.0}, {2, ChangeKind::Keep, 0.0, 0.0}}));
    CHECK(texts(lines) == std::vector<std::string>{"Atmo fades out in 2.0 s", kElse});  // Crowd fades
}

TEST_CASE("describeStep: a sound an earlier FadeOut removed is not counted") {
    Show show = demo();
    show.steps.push_back(fadeStep(4, {{3, ChangeKind::FadeOut, 0.0, 2.0}, {1, ChangeKind::Keep, 0.0, 0.0},
                                      {2, ChangeKind::Keep, 0.0, 0.0}}));  // Crowd ends here
    const auto lines = describeAppended(show, fadeStep(5, {{1, ChangeKind::Keep, 0.0, 0.0}, {2, ChangeKind::Keep, 0.0, 0.0}}));
    CHECK(lines.empty());
}

TEST_CASE("describeStep: sounds an earlier everything-else fade removed are not counted") {
    Show show = demo();
    // Music B fades everything else: only its own sound is running afterwards.
    show.steps.push_back(makeStep(4, show.settings, Sound{.file = 2}));
    REQUIRE(show.steps.back().others.kind == ChangeKind::FadeOut);
    const auto lines = describeAppended(show, fadeStep(5, {{4, ChangeKind::Level, -3.0, 1.0}}));
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].text == "step 4" + kArrow + kMinus + "3.0 dB in 1.0 s");
}

TEST_CASE("describeStep: an earlier everything-else fade spares what that step names") {
    Show show = demo();
    // Step 4 fades everything else but keeps Atmo by naming it: Atmo still runs at step 5.
    Step keepAtmo = makeStep(4, show.settings, std::nullopt);
    keepAtmo.changes = {{1, ChangeKind::Keep, 0.0, 0.0}};
    keepAtmo.others = Others{ChangeKind::FadeOut, 2.0};
    show.steps.push_back(keepAtmo);
    CHECK(texts(describeAppended(show, fadeStep(5, {}))) == std::vector<std::string>{kElse});
}

TEST_CASE("song A, then a fade step fading A, then everything else fades: no line") {
    Show show;
    show.files = {{1, "audio/Song A.flac", 0.0, ""}};
    show.steps = {keeping(1, "Song A", 1, show.settings), fadeStep(2, {{1, ChangeKind::FadeOut, 0.0, 2.0}})};
    CHECK(describeAppended(show, fadeStep(3, {})).empty());
}

// The NEXT row passes what the engine really plays instead of the list walk.

TEST_CASE("live: a step ended by itself is not counted") {
    Show show = demo();  // the list walk has Atmo, Music A and Crowd running
    show.steps.push_back(fadeStep(4, {}));
    REQUIRE(texts(describeStep(show, 3)) == std::vector<std::string>{kElse});
    const std::vector<StepId> nothing;
    CHECK(describeStep(show, 3, false, &nothing).empty());
}

TEST_CASE("live: out-of-order GO counts what really runs") {
    Show show = demo();
    // Step 4 fades Atmo, keeps Music A; the list walk has Crowd (step 3) left to fade.
    show.steps.push_back(fadeStep(4, {{1, ChangeKind::FadeOut, 0.0, 2.0}, {2, ChangeKind::Keep, 0.0, 0.0},
                                      {3, ChangeKind::Keep, 0.0, 0.0}}));
    REQUIRE(describeStep(show, 3).size() == 1);  // no everything-else line from the walk
    const std::vector<StepId> runningNow{5};  // a later step GOne by hand before step 4
    show.steps.push_back(keeping(5, "Encore", 2, show.settings));
    CHECK(texts(describeStep(show, 3, false, &runningNow)) == std::vector<std::string>{"Atmo fades out in 2.0 s", kElse});
}

TEST_CASE("live: a running sound named by the step or its own does not count") {
    Show show = demo();
    show.steps.push_back(fadeStep(4, {{1, ChangeKind::Keep, 0.0, 0.0}}));
    const std::vector<StepId> runningNow{1, 4};
    CHECK(describeStep(show, 3, false, &runningNow).empty());
}
