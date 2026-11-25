// ShowControllerTests.cpp: the control-thread layer between the show and the
// engine: GO and its guard, refusing unusable sounds, clamping, pads, seek,
// live trim, expected sounds and timecode.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <limits>

#include "TestSources.h"
#include "bigtop/ShowEdits.h"

using Catch::Approx;
using namespace bigtop;

namespace {
// Engine at 1 kHz so frame counts read as milliseconds; files 1 and 2 of
// twoTrackShow() are loaded (10 s each).
struct Fixture {
    Engine e{1000};
    MapProvider provider;
    ShowController c{e, provider};
    Fixture() { addTwoTrackSources(provider); }
};
}  // namespace

TEST_CASE_METHOD(Fixture, "GO runs a step and advances; default fades the previous sound out over 2 s", "[controller]") {
    c.setShow(twoTrackShow());
    CHECK(c.go(0.0) == GoResult::Ran); CHECK(c.nextIndex() == 1); render(e, 1000);
    CHECK(c.go(1.0) == GoResult::Ran);
    auto out = render(e, 2100);
    CHECK(out[0] == Approx(1.0f + 0.5f));            // old still full, new cut in at trim -6 dB
    CHECK(out[2050] == Approx(0.5f));                // old gone after 2 s
    CHECK(c.go(5.0) == GoResult::EndOfList);
}
TEST_CASE_METHOD(Fixture, "A second GO within 300 ms is ignored", "[controller]") {
    c.setShow(twoTrackShow());
    CHECK(c.go(10.0) == GoResult::Ran); CHECK(c.go(10.29) == GoResult::IgnoredGuard);
    CHECK(c.nextIndex() == 1); CHECK(c.go(10.31) == GoResult::Ran);
}
TEST_CASE_METHOD(Fixture, "GO refuses an unusable sound and stays on the step", "[controller]") {           // MissingFile, not a silent GO
    Show s = twoTrackShow(); s.steps[0].sound->file = 99;                   // no such file
    c.setShow(s); CHECK(c.go(0.0) == GoResult::MissingFile); CHECK(c.nextIndex() == 0);
    s = twoTrackShow(); s.steps[0].sound->startSeconds = 10.0;              // start at the end
    c.setShow(s); CHECK(c.go(1.0) == GoResult::MissingFile);
    s = twoTrackShow(); s.steps[0].sound->endSeconds = 0.0;                 // end before start
    c.setShow(s); CHECK(c.go(2.0) == GoResult::MissingFile);
    provider.add(3, ConstSource(0)); s = twoTrackShow(); s.files.push_back({3, "z.wav"}); s.steps[0].sound->file = 3;
    c.setShow(s); CHECK(c.go(3.0) == GoResult::MissingFile);
}
TEST_CASE_METHOD(Fixture, "Negative times and absurd levels are clamped", "[controller]") {                 // show data is not trusted
    Show s = twoTrackShow(); auto& snd = *s.steps[0].sound;
    snd.fadeInSeconds = -1; snd.startDelaySeconds = -5; snd.levelDb = 200;
    c.setShow(s); CHECK(c.go(0.0) == GoResult::Ran);
    CHECK(render(e, 10)[0] == Approx(float(dbToGain(12.0))));
}
TEST_CASE_METHOD(Fixture, "A full engine queue gives EngineBusy and does not advance", "[controller]") {    // nothing sent, guard not started
    c.setShow(twoTrackShow()); for (int i = 0; i < 64; ++i) e.post(TrimCommand{9, 1.0f});
    CHECK(c.go(0.0) == GoResult::EngineBusy); CHECK(c.nextIndex() == 0);
    render(e, 64); CHECK(c.go(0.01) == GoResult::Ran);                      // guard was not started
}
TEST_CASE_METHOD(Fixture, "Selecting another step changes nothing that is playing", "[controller]") {
    c.setShow(twoTrackShow()); c.go(0.0); render(e, 64);
    c.selectNext(0); CHECK(c.nextIndex() == 0); c.selectNext(50); CHECK(c.nextIndex() == 2);
    CHECK(render(e, 64)[0] == 1.0f);
}
TEST_CASE_METHOD(Fixture, "expectedRunningAt walks the list", "[controller]") {
    c.setShow(specExampleShow());                                   // list indices 0..4 hold step ids 3..7
    CHECK(c.expectedRunningAt(0).empty());
    CHECK(c.expectedRunningAt(1) == std::vector<StepId>{3});        // Music A
    CHECK(c.expectedRunningAt(2) == std::vector<StepId>{4});        // Atmo only
    CHECK(c.expectedRunningAt(4) == std::vector<StepId>{4, 6});
    CHECK(c.expectedRunningAt(5) == std::vector<StepId>{6});
}
TEST_CASE_METHOD(Fixture, "A pad starts on tap, stops on the next tap, and survives fade out all", "[controller]") {
    Show s = twoTrackShow(); s.pads[0] = {"Fanfare", 1, 0.0}; c.setShow(s);
    CHECK(c.triggerPad(0)); render(e, 64); CHECK(c.fadeOutAll()); CHECK(render(e, 64)[10] == 1.0f);
    CHECK(c.triggerPad(0)); auto out = render(e, 128); CHECK(out[100] == 0.0f);
    CHECK_FALSE(c.triggerPad(1));                                           // empty pad
}
TEST_CASE_METHOD(Fixture, "levelPad ramps a playing pad to its stored level and file trim", "[controller]") {
    Show s = twoTrackShow(); s.pads[0] = {"Fanfare", 1, 0.0}; c.setShow(s);
    CHECK_FALSE(c.levelPad(0));                                             // not playing: nothing to change
    CHECK(c.triggerPad(0)); render(e, 64);
    s.pads[0].levelDb = -6.0206; c.updateShow(s);
    CHECK(c.levelPad(0)); auto out = render(e, 20);
    CHECK(out[5] < 1.0f); CHECK(out[5] > 0.5f);                             // mid declick, no jump
    CHECK(out[15] == Approx(0.5f));                                         // there after the 10 ms declick
    s.files[0].trimDb = -6.0206; c.updateShow(s);                           // the file trim counts too
    CHECK(c.levelPad(0)); CHECK(render(e, 20)[15] == Approx(0.25f));
    CHECK_FALSE(c.levelPad(1)); CHECK_FALSE(c.levelPad(-1));                // empty, out of range
}
TEST_CASE_METHOD(Fixture, "levelPad leaves a pad that a tap is fading to its fade", "[controller]") {
    Show s = twoTrackShow(); s.pads[0] = {"Fanfare", 1, 0.0}; c.setShow(s);
    CHECK(c.triggerPad(0)); render(e, 64);
    CHECK(c.triggerPad(0)); render(e, 1);                                   // second tap: fading out
    CHECK_FALSE(c.levelPad(0)); render(e, 128);                             // a Level would keep it alive
    CHECK(e.snapshot().numVoices == 0);
}
TEST_CASE_METHOD(Fixture, "A pad hidden by the grid does not play", "[controller]") {
    Show s = twoTrackShow(); s.pads[3] = {"Hidden", 1, 0.0}; s.settings.padRows = 1; s.settings.padCols = 2;  // shows pads 0..1
    c.setShow(s);
    CHECK_FALSE(c.triggerPad(3)); CHECK_FALSE(c.triggerPad(8)); CHECK_FALSE(c.triggerPad(-1));
    s.settings.padRows = 2; c.setShow(s); CHECK(c.triggerPad(3));
}
TEST_CASE_METHOD(Fixture, "Timecode is start value plus position in the file and follows seeks", "[controller]") {
    Show s = twoTrackShow(); s.steps[0].sound->timecodeStart = Timecode{1,0,0,0}; c.setShow(s);
    CHECK_FALSE(c.currentTimecode()); c.go(0.0); render(e, 2000);
    CHECK(*c.currentTimecode() == Timecode{1,0,2,0});
    c.seek(1, 7.5); render(e, 64);
    CHECK(*c.currentTimecode() == Timecode{1,0,7,14});                      // 7.564 s at 25 fps
}
TEST_CASE_METHOD(Fixture, "Live trim scales a running sound and resets on restart", "[controller]") {
    c.setShow(twoTrackShow()); c.go(0.0); render(e, 64);
    c.setLiveTrimDb(1, -6.0206); render(e, 64); CHECK(render(e, 10)[5] == Approx(0.5f));
    c.selectNext(0); c.go(1.0); render(e, 64); CHECK(render(e, 10)[5] == Approx(1.0f));
}

// NaN/inf times that slip past the loader act as 0; a NaN level is silence.
TEST_CASE_METHOD(Fixture, "Non-finite times count as 0 and a non-finite level as silence", "[controller]") {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    Show s = twoTrackShow(); auto& snd = *s.steps[0].sound;
    snd.fadeInSeconds = nan; snd.startDelaySeconds = inf; snd.startSeconds = nan;
    c.setShow(s); CHECK(c.go(0.0) == GoResult::Ran);
    CHECK(render(e, 10)[0] == 1.0f);                                        // at once, full level, from the top
    s = twoTrackShow(); s.steps[1].sound->levelDb = nan; s.steps[1].others = {ChangeKind::FadeOut, nan};
    c.setShow(s); c.selectNext(1); CHECK(c.go(0.0) == GoResult::Ran);
    CHECK(render(e, 20)[10] == 0.0f);                                       // step 1 gone after the 10 ms declick, step 2 silent
}
// A Level change applies the target file's trim: -6.0206 dB on file 2 halves it.
TEST_CASE_METHOD(Fixture, "A Level change includes the target file's trim", "[controller]") {
    Show s = twoTrackShow();
    Step dip = makeStep(3, s.settings, std::nullopt);
    dip.changes = {{2, ChangeKind::Level, -6.0206, 0.0}};
    s.steps.push_back(dip); c.setShow(s);
    c.go(0.0); c.go(1.0); render(e, 2100);                                  // only file 2 left, at 0.5
    CHECK(c.go(2.0) == GoResult::Ran);
    CHECK(render(e, 20)[15] == Approx(0.5f * float(dbToGain(-6.0206))));  // after the 10 ms declick
}
// Changes aimed at the step's own id or at no step at all must not break the start.
TEST_CASE_METHOD(Fixture, "Changes to the own id or an unknown id are harmless", "[controller]") {
    Show s = twoTrackShow();
    s.steps[0].changes = {{1, ChangeKind::FadeOut, 0.0, 0.0}, {99, ChangeKind::Level, 0.0, 0.0},
                          {98, ChangeKind::FadeOut, 0.0, 0.0}};
    c.setShow(s); CHECK(c.go(0.0) == GoResult::Ran);
    CHECK(render(e, 10)[5] == 1.0f);
}
// A step with no sound, no changes and others Keep still counts as a GO.
TEST_CASE_METHOD(Fixture, "An empty step runs and advances", "[controller]") {
    Show s; s.steps.push_back(makeStep(1, s.settings, std::nullopt)); c.setShow(s);
    CHECK(c.go(0.0) == GoResult::Ran); CHECK(c.nextIndex() == 1);
}
// fadeOutAll stops step sounds but leaves the GO position and guard alone.
TEST_CASE_METHOD(Fixture, "fadeOutAll keeps the next index and the guard", "[controller]") {
    c.setShow(twoTrackShow()); c.go(0.0); render(e, 64);
    CHECK(c.fadeOutAll()); CHECK(c.nextIndex() == 1);
    CHECK(c.go(0.1) == GoResult::IgnoredGuard);
    render(e, 2100); CHECK(render(e, 10)[0] == 0.0f);
}

// A corrupt trim cannot overdrive the PA: level + trim is capped at +24 dB.
TEST_CASE_METHOD(Fixture, "Level plus file trim is capped at +24 dB", "[controller]") {
    Show s = twoTrackShow(); s.files[0].trimDb = 100; c.setShow(s);
    CHECK(c.go(0.0) == GoResult::Ran);
    CHECK(render(e, 10)[5] == Approx(float(dbToGain(24.0))));
}
// Ids in the pad half of the key range would dodge fade out all; refuse them.
TEST_CASE_METHOD(Fixture, "Step ids outside 1..kMaxStepId are refused, such targets dropped", "[controller]") {
    Show s = twoTrackShow(); s.steps[0].id = kMaxStepId + 1; c.setShow(s);
    CHECK(c.go(0.0) == GoResult::MissingFile); CHECK(c.nextIndex() == 0);
    s = twoTrackShow(); s.steps[0].id = 0; c.setShow(s);
    CHECK(c.go(0.0) == GoResult::MissingFile);
    s = twoTrackShow(); s.pads[0] = {"Fanfare", 1, 0.0};
    s.steps[1].changes = {{kPadKeyBase, ChangeKind::FadeOut, 0.0, 0.0}}; s.steps[1].others = {};
    c.setShow(s); CHECK(c.triggerPad(0)); render(e, 64);
    c.selectNext(1); CHECK(c.go(0.0) == GoResult::Ran);
    CHECK(render(e, 10)[5] == Approx(1.0f + 0.5f));                          // pad untouched, step 2 added
}
// Fade out all, stop a pad, fade out all again: the second press still stops at once.
TEST_CASE_METHOD(Fixture, "Stopping a pad keeps the second fade out all fast", "[controller]") {
    Show s = twoTrackShow(); s.pads[0] = {"Fanfare", 1, 0.0}; c.setShow(s);
    c.go(0.0); CHECK(c.triggerPad(0)); render(e, 64);
    CHECK(c.fadeOutAll()); render(e, 128);
    CHECK(c.triggerPad(0)); CHECK(c.fadeOutAll()); render(e, 64);
    CHECK(render(e, 64)[0] == 0.0f);
}
// A GO during the previous step's start delay does not cancel that start,
// even though the new step's default others rule fades everything else out.
TEST_CASE_METHOD(Fixture, "A GO during a start delay keeps the pending sound", "[controller]") {
    Show s = twoTrackShow(); s.steps[0].sound->startDelaySeconds = 0.5; c.setShow(s);
    CHECK(c.go(0.0) == GoResult::Ran); render(e, 350);
    CHECK(c.go(0.35) == GoResult::Ran);                                    // step 1 still pending
    auto out = render(e, 700);
    CHECK(out[660] == Approx(1.0f + 0.5f));                                 // 1.01 s after the first GO: both audible
}
// A NaN clock reading is accepted (fail open) but never stored, so the guard
// keeps counting from the last finite GO time.
TEST_CASE_METHOD(Fixture, "A non-finite GO time is accepted but does not move the guard", "[controller]") {
    Show s = twoTrackShow(); s.steps.push_back(makeStep(3, s.settings, std::nullopt)); c.setShow(s);
    CHECK(c.go(0.0) == GoResult::Ran);
    CHECK(c.go(std::numeric_limits<double>::quiet_NaN()) == GoResult::Ran);
    CHECK(c.go(0.1) == GoResult::IgnoredGuard);                             // guard from 0.0 still stands
    CHECK(c.go(0.4) == GoResult::Ran);
}
// The controller turns a NaN Level into gain 0; the engine alone would ignore it.
TEST_CASE_METHOD(Fixture, "A NaN Level change silences the running sound", "[controller]") {
    Show s = twoTrackShow();
    Step dip = makeStep(3, s.settings, std::nullopt);
    dip.changes = {{1, ChangeKind::Level, std::numeric_limits<double>::quiet_NaN(), 0.0}};
    s.steps = {s.steps[0], dip}; c.setShow(s);
    c.go(0.0); render(e, 64); CHECK(c.go(1.0) == GoResult::Ran);
    CHECK(render(e, 20)[15] == 0.0f);  // after the 10 ms declick
}

TEST_CASE("sourceFor receives the voice key and the start frame", "[controller]") {
    Engine e{1000}; MapProvider map; addTwoTrackSources(map);
    RecordingProvider provider(map); ShowController c{e, provider};
    Show s = twoTrackShow(); s.steps[0].sound->startSeconds = 2.5; c.setShow(s); c.go(0.0);
    CHECK(provider.calls.back() == std::tuple<VoiceKey, FileId, int64_t>{1, 1, 2500});
}

TEST_CASE_METHOD(Fixture, "updateShow keeps the next step by id and resets nothing", "[controller]") {
    c.setShow(twoTrackShow()); c.go(0.0);                                   // next = index 1 (step 2)
    Show s = c.show(); insertSteps(s, 0, {makeStep(*allocateStepId(s), s.settings, std::nullopt)});
    c.updateShow(s); CHECK(c.nextIndex() == 2);
    CHECK(c.go(0.1) == GoResult::IgnoredGuard);                             // guard not reset
    s = c.show(); removeStep(s, 2); c.updateShow(s); CHECK(c.nextIndex() == 2);   // removed: index stays, clamped to size
    s = c.show(); removeStep(s, 1); c.updateShow(s); CHECK(c.nextIndex() == 1);
}

TEST_CASE("secondsToFrames rounds, maps bad times to 0 and caps huge ones", "[controller]") {
    CHECK(secondsToFrames(1.0, 48000.0) == 48000);
    CHECK(secondsToFrames(0.75, 2.0) == 2);  // exactly 1.5 frames: rounds half away from zero
    CHECK(secondsToFrames(1.4 / 1000.0, 1000.0) == 1);
    CHECK(secondsToFrames(-1.0, 48000.0) == 0);
    CHECK(secondsToFrames(std::numeric_limits<double>::quiet_NaN(), 48000.0) == 0);
    CHECK(secondsToFrames(std::numeric_limits<double>::infinity(), 48000.0) == 0);
    CHECK(secondsToFrames(-std::numeric_limits<double>::infinity(), 48000.0) == 0);
    CHECK(secondsToFrames(1e300, 48000.0) == 1000000000000000000LL);
}

// GO into a paused show would start a sound that is then silent: refuse it.
TEST_CASE_METHOD(Fixture, "GO while paused returns Paused and sends nothing", "[controller]") {
    c.setShow(twoTrackShow());
    CHECK(c.pause()); CHECK(c.paused()); render(e, 64);
    CHECK(c.go(0.0) == GoResult::Paused); CHECK(c.nextIndex() == 0);
    for (int i = 0; i < 64; ++i) CHECK(e.post(TrimCommand{9, 1.0f}));     // the whole queue was still free
    render(e, 64); CHECK(e.snapshot().numVoices == 0);
    CHECK(c.resume()); CHECK_FALSE(c.paused());
    CHECK(c.go(0.0) == GoResult::Ran);                                     // guard was not started either
}
TEST_CASE_METHOD(Fixture, "pause and resume report a full queue", "[controller]") {
    for (int i = 0; i < 64; ++i) e.post(TrimCommand{9, 1.0f});
    CHECK_FALSE(c.pause()); CHECK_FALSE(c.paused());
    render(e, 64); CHECK(c.pause());
    for (int i = 0; i < 64; ++i) e.post(TrimCommand{9, 1.0f});
    CHECK_FALSE(c.resume()); CHECK(c.paused());
}
TEST_CASE_METHOD(Fixture, "fadeOutSound sends one FadeOut change for that step and keeps the others", "[controller]") {
    Show s = twoTrackShow(); s.steps[1].others = {}; c.setShow(s);
    c.go(0.0); c.go(1.0); render(e, 64);
    CHECK(c.fadeOutSound(1, 1.0));
    auto out = render(e, 1100);
    CHECK(out[0] == Approx(1.0f + 0.5f));
    CHECK(out[500] < 1.5f); CHECK(out[500] > 0.5f);                         // step 1 mid fade
    CHECK(out[1050] == Approx(0.5f));                                       // step 2 untouched
    CHECK(e.snapshot().numVoices == 1); CHECK(e.snapshot().voices[0].key == 2);
    CHECK(c.nextIndex() == 2);
}
TEST_CASE_METHOD(Fixture, "fadeOutAll while paused clears paused()", "[controller]") {
    c.setShow(twoTrackShow()); c.go(0.0); render(e, 64);
    CHECK(c.pause()); render(e, 200);
    CHECK(c.fadeOutAll()); CHECK_FALSE(c.paused());
    render(e, 64); CHECK_FALSE(e.snapshot().paused); CHECK(e.snapshot().numVoices == 0);
}
TEST_CASE_METHOD(Fixture, "keepTrim folds the live fader into the sound's level", "[controller]") {
    c.setShow(twoTrackShow()); c.go(0.0); render(e, 64);
    c.setLiveTrimDb(1, -6.0206); render(e, 64);
    CHECK(c.keepTrim(1)); auto out = render(e, 64);
    CHECK(out[5] == Approx(0.5f));
    CHECK(e.snapshot().voices[0].trim == 1.0f); CHECK(e.snapshot().voices[0].gain == Approx(0.5f));
}
TEST_CASE_METHOD(Fixture, "levelSetter follows the GOs that set a sound's level", "[controller]") {
    // List order 1, 5, 3: step 5 sets nothing, step 3 levels sound 1.
    Show s = twoTrackShow(); s.steps.pop_back();
    s.steps.push_back(makeStep(5, s.settings, std::nullopt));
    Step fade = makeStep(3, s.settings, std::nullopt);
    setChange(fade, {1, ChangeKind::Level, -3.0, 2.0});
    s.steps.push_back(fade);
    CHECK(c.levelSetter(1) == 0);
    c.setShow(s);
    CHECK(c.go(0.0) == GoResult::Ran); CHECK(c.levelSetter(1) == 1);        // the start sets it
    c.selectNext(2);
    CHECK(c.go(0.1) == GoResult::IgnoredGuard); CHECK(c.levelSetter(1) == 1);  // a refused GO sets nothing
    CHECK(c.go(1.0) == GoResult::Ran); CHECK(c.levelSetter(1) == 3);
    c.selectNext(1);
    CHECK(c.go(2.0) == GoResult::Ran); CHECK(c.levelSetter(1) == 3);        // run order, not list order
    c.updateShow(s); CHECK(c.levelSetter(1) == 3);                          // an edit does not forget it
    c.selectNext(0); CHECK(c.go(3.0) == GoResult::Ran); CHECK(c.levelSetter(1) == 1);  // a restart resets it
    c.setShow(s); CHECK(c.levelSetter(1) == 0);
}
// Keep after a fade step: the heard level came from the step's Level change,
// and folding the fader into it must not be audible (owner bug, 2026-11-18).
TEST_CASE_METHOD(Fixture, "Keep after a Level change step is inaudible", "[controller]") {
    Show s = twoTrackShow(); s.steps.pop_back();
    s.steps[0].sound->levelDb = -12.5;
    Step fade = makeStep(3, s.settings, std::nullopt);
    setChange(fade, {1, ChangeKind::Level, -3.0, 2.0});
    s.steps.push_back(fade);
    c.setShow(s); c.go(0.0); render(e, 500);
    c.go(1.0); render(e, 2500);                                           // the 2 s Level change is done
    c.setLiveTrimDb(1, -6.0); render(e, 1000);
    const float before = render(e, 64).back();
    CHECK(before == Approx(float(dbToGain(-9.0))).margin(1e-4));
    CHECK(c.keepTrim(1)); const auto after = render(e, 64);
    CHECK(after.front() == Approx(before).margin(1e-4));
    CHECK(after.back() == Approx(before).margin(1e-4));
    CHECK(e.snapshot().voices[0].trim == 1.0f);
}
// A refused start opens no file: a start point at or after the
// end point is known from the show alone, so the provider is never asked.
TEST_CASE("A GO refused for start >= end never asks the provider for a source", "[controller]") {
    Engine e{1000}; MapProvider map; addTwoTrackSources(map);
    RecordingProvider provider(map); ShowController c{e, provider};
    Show s = twoTrackShow(); s.steps[0].sound->startSeconds = 5.0; s.steps[0].sound->endSeconds = 2.0; c.setShow(s);
    CHECK(c.go(0.0) == GoResult::MissingFile);
    s.steps[0].sound->endSeconds = 5.0; c.setShow(s);
    CHECK(c.go(1.0) == GoResult::MissingFile);
    CHECK(provider.calls.empty());
}
TEST_CASE_METHOD(Fixture, "fadeOutSound while paused removes the sound at once", "[controller]") {
    c.setShow(twoTrackShow()); c.go(0.0); render(e, 64);
    CHECK(c.pause()); render(e, 200);                                       // past the 0.1 s pause fade
    CHECK(c.fadeOutSound(1, 2.0)); render(e, 64);
    CHECK(e.snapshot().numVoices == 0);
    CHECK(c.resume()); render(e, 200); CHECK(e.snapshot().numVoices == 0);
}
// Opening another show makes a new controller over the same engine. A pause
// left in the engine (Pause pressed with nothing playing) would freeze the new
// show's first GO at gain 0 while the new controller thinks it is not paused.
TEST_CASE("A new controller ends a pause left in the engine, so its first GO is heard", "[controller]") {
    Engine e{1000};
    MapProvider provider; addTwoTrackSources(provider);
    {
        ShowController old{e, provider};
        CHECK(old.pause()); render(e, 200);
    }
    REQUIRE(e.snapshot().paused);
    ShowController c{e, provider};
    CHECK_FALSE(c.paused());
    c.setShow(twoTrackShow()); render(e, 200);                            // past the 0.1 s resume fade
    CHECK_FALSE(e.snapshot().paused);
    CHECK(c.go(0.0) == GoResult::Ran);
    CHECK(render(e, 64)[10] == Approx(1.0f));
}
// If the engine queue is full when the controller is made, the end of the pause
// cannot be sent: the controller then says paused, so the UI shows Resume.
TEST_CASE("A new controller over a full queue reports paused until resumed", "[controller]") {
    Engine e{1000};
    MapProvider provider; addTwoTrackSources(provider);
    for (int i = 0; i < 64; ++i) e.post(TrimCommand{9, 1.0f});
    ShowController c{e, provider};
    CHECK(c.paused());
    render(e, 64); CHECK(c.resume()); CHECK_FALSE(c.paused());
}
