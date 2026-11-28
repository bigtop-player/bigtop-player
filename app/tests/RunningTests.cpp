// RunningTests.cpp: the running rows (fader and Keep, Fade then Stop, the position bar's seek) and
// Pause / Resume, driven through the windowless app with synthetic fingers and a manual clock.
#include <algorithm>
#include <cstdlib>
#include <optional>
#include <string>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "AppTestSupport.h"
#include "Pointer.h"
#include "UiClock.h"

namespace bigtop::app {

namespace {

using Catch::Matchers::WithinAbs;

constexpr int kBlock = 512;            // frames per TestApp::audio() block
constexpr juce::uint32 kPastGuardMs = 400;  // more than the default 0.3 s GO guard

// The step voice of @p key in the latest snapshot, if it plays.
std::optional<VoiceState> voice(TestApp& app, VoiceKey key) {
    const auto snap = app.session().snapshot();
    for (int i = 0; i < snap.numVoices; ++i)
        if (snap.voices[i].key == key) return snap.voices[i];
    return std::nullopt;
}

// The stored level of a sound step, in dB.
double levelDb(TestApp& app, StepId id) {
    const auto& steps = app.session().controller().show().steps;
    const auto it = std::find_if(steps.begin(), steps.end(), [id](const Step& s) { return s.id == id; });
    REQUIRE((it != steps.end() && it->sound));
    return it->sound->levelDb;
}

// Selects the step at @p index and presses GO, past the guard of the GO before.
void goAt(TestApp& app, ManualClock& clock, int index) {
    clock.advance(kPastGuardMs);
    app.session().controller().selectNext(index);
    AppTestAccess::go(app.main());
}

// threeSoundSteps(), but every GO keeps the sounds already running (two can play together).
Show keepingSteps() {
    Show show = threeSoundSteps();
    for (auto& s : show.steps) s.others = Others{};  // Others{} = keep
    return show;
}

// Drags a fader from its thumb, as a finger would, by the pixels that move it to @p db
// (the fader drags relative: JUCE's absolute mode with snap-to-mouse off).
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

juce::Point<int> centre(const juce::Component& c) { return c.getLocalBounds().getCentre(); }

// Waits (bounded) until every file of the open show has its waveform summary.
void waitPeaks(TestApp& app) {
    app.waitUntil([&] {
        for (const auto& f : app.session().controller().show().files)
            if (app.session().peaks(f.id) == nullptr) return false;
        return true;
    });
}

// Pixels of exactly @p colour in a component's rendering.
int countColour(juce::Component& c, juce::Colour colour) {
    const auto image = c.createComponentSnapshot(c.getLocalBounds(), false, 1.0f);
    int n = 0;
    for (int y = 0; y < image.getHeight(); ++y)
        for (int x = 0; x < image.getWidth(); ++x)
            if (image.getPixelAt(x, y) == colour) ++n;
    return n;
}

constexpr juce::uint32 kWaveGrey = 0xff8c959c;  // the waveform's unplayed part (RunningSounds.cpp)
constexpr juce::uint32 kTrackGrey = 0xff3a3a3a;  // the plain bar's track
constexpr juce::uint32 kPanelColour = 0xff15181b;  // the waveform's own background panel

}  // namespace

TEST_CASE("Keep folds -10 dB into the step level and resets the fader") {
    ManualClock clock;
    TestApp app(threeSoundSteps());
    app.waitHeads();
    goAt(app, clock, 1);  // Song A
    app.audio(5);
    auto& row = AppTestAccess::row(app.main(), 2);
    auto& fader = AppTestAccess::faderOf(row);
    auto& keep = AppTestAccess::keepOf(row);
    const double before = levelDb(app, 2);

    dragFader(fader, -10.0);
    app.tick();      // the fader stopped moving: a refused move would be resent here
    app.audio(2);    // the engine applied the trim Keep folds
    requireHittable(keep);
    REQUIRE(keep.isEnabled());
    tap(keep, centre(keep));

    REQUIRE_THAT(levelDb(app, 2), WithinAbs(before - 10.0, 0.05));
    REQUIRE(fader.getValue() == 0.0);
}

TEST_CASE("Keep is greyed at 0 dB and refused while the fader moves") {
    ManualClock clock;
    TestApp app(threeSoundSteps());
    app.waitHeads();
    goAt(app, clock, 1);
    app.audio(5);
    auto& row = AppTestAccess::row(app.main(), 2);
    auto& fader = AppTestAccess::faderOf(row);
    auto& keep = AppTestAccess::keepOf(row);
    const double before = levelDb(app, 2);

    // At 0 dB: shown in its place but greyed, and a tap does nothing.
    requireHittable(keep);
    REQUIRE(!keep.isEnabled());
    tap(keep, centre(keep));
    REQUIRE(levelDb(app, 2) == before);

    // A fader that has not reached the engine yet: many moves with no audio block fill the
    // 64-command queue, so the last value waits to be resent (what "still moving" means to Keep).
    requireHittable(fader);
    const juce::Point<int> from(juce::roundToInt(fader.getPositionOfValue(0.0)), fader.getHeight() / 2);
    press(fader, from);
    for (int i = 0; i < 80; ++i) drag(fader, from, from + juce::Point<int>(i % 2 == 0 ? -30 : -40, 0));
    release(fader, from, from + juce::Point<int>(-40, 0));
    const double moved = fader.getValue();
    REQUIRE(moved < -5.0);
    app.tick();  // lights Keep up; its resend is refused too, the queue is still full
    REQUIRE(keep.isEnabled());
    tap(keep, centre(keep));
    REQUIRE(app.session().status().message.contains("Fader still moving"));
    REQUIRE(levelDb(app, 2) == before);

    // Once the engine has the value, the same tap keeps it.
    app.audio(1);  // drains the queue; the tick after it resends the waiting value
    app.audio(1);  // applies it
    tap(keep, centre(keep));
    REQUIRE_THAT(levelDb(app, 2), WithinAbs(before + moved, 0.05));
}

TEST_CASE("Keep is refused while a finger holds the fader") {
    ManualClock clock;
    TestApp app(threeSoundSteps());
    app.waitHeads();
    goAt(app, clock, 1);
    app.audio(5);
    auto& row = AppTestAccess::row(app.main(), 2);
    auto& fader = AppTestAccess::faderOf(row);
    auto& keep = AppTestAccess::keepOf(row);
    const double before = levelDb(app, 2);
    dragFader(fader, -10.0);
    app.tick();
    app.audio(2);  // the engine has the value: only the held finger can refuse Keep now
    requireHittable(keep);
    REQUIRE(keep.isEnabled());

    {
        const PointerHeld held(fader);  // a second finger rests on the fader
        tap(keep, centre(keep));
        REQUIRE(app.session().status().message.contains("Fader still moving"));
        REQUIRE(levelDb(app, 2) == before);
    }
    tap(keep, centre(keep));  // the finger lifted: the same tap keeps it
    REQUIRE_THAT(levelDb(app, 2), WithinAbs(before - 10.0, 0.05));
}

TEST_CASE("Keep is absent in Show mode") {
    ManualClock clock;
    TestApp app(threeSoundSteps());
    app.waitHeads();
    goAt(app, clock, 1);
    app.audio(5);
    auto& row = AppTestAccess::row(app.main(), 2);
    auto& keep = AppTestAccess::keepOf(row);
    dragFader(AppTestAccess::faderOf(row), -4.5);
    app.audio(2);
    requireHittable(keep);  // Edit mode: there

    AppTestAccess::pickMode(app.main(), ShowMode::Show);
    app.tick();
    REQUIRE(app.session().mode() == ShowMode::Show);
    REQUIRE(!keep.isVisible());
}

TEST_CASE("the fader does not move when Keep lights up") {
    ManualClock clock;
    TestApp app(threeSoundSteps());
    app.waitHeads();
    const auto mode = GENERATE(ShowMode::Edit, ShowMode::Rehearsal);  // the two modes with Keep
    AppTestAccess::pickMode(app.main(), mode);
    REQUIRE(app.session().mode() == mode);
    goAt(app, clock, 1);
    app.audio(5);
    auto& row = AppTestAccess::row(app.main(), 2);
    auto& fader = AppTestAccess::faderOf(row);
    auto& keep = AppTestAccess::keepOf(row);
    REQUIRE(!keep.isEnabled());
    const auto bounds = fader.getBounds();

    dragFader(fader, -4.5);  // off 0 dB: Keep lights up
    app.tick();

    REQUIRE(keep.isEnabled());
    REQUIRE(fader.getBounds() == bounds);
}

TEST_CASE("Fade then Stop ends only that sound") {
    ManualClock clock;
    TestApp app(keepingSteps());
    app.waitHeads();
    goAt(app, clock, 0);  // Intro
    goAt(app, clock, 1);  // Song A, Intro keeps playing
    app.audio(5);
    REQUIRE(voice(app, 1).has_value());
    REQUIRE(voice(app, 2).has_value());
    const float introGain = voice(app, 1)->gain;
    const float songGain = voice(app, 2)->gain;

    auto& fade = AppTestAccess::fadeOf(AppTestAccess::row(app.main(), 2));
    requireHittable(fade);
    REQUIRE(fade.getButtonText() == "Fade");
    tap(fade, centre(fade));
    app.audio(20);  // about 0.2 s of the 2 s fade
    REQUIRE(voice(app, 2).has_value());
    REQUIRE(voice(app, 2)->gain < songGain);
    REQUIRE(fade.getButtonText() == "Stop");

    tap(fade, centre(fade));  // the clock has not moved: still within the fade
    app.audio(2);             // the 10 ms stop
    REQUIRE(!voice(app, 2).has_value());
    REQUIRE(voice(app, 1).has_value());
    REQUIRE(voice(app, 1)->gain == introGain);

    // A GO that restarts a fading sound shows Fade again: the new sound is not fading.
    auto& introFade = AppTestAccess::fadeOf(AppTestAccess::row(app.main(), 1));
    requireHittable(introFade);
    tap(introFade, centre(introFade));
    app.tick();
    REQUIRE(introFade.getButtonText() == "Stop");
    goAt(app, clock, 0);  // 0.4 s on: well inside the 2 s fade
    app.audio(2);
    REQUIRE(AppTestAccess::fadeOf(AppTestAccess::row(app.main(), 1)).getButtonText() == "Fade");
}

TEST_CASE("seek happens once, on release, at the release position") {
    const bool withWaveform = GENERATE(false, true);
    INFO("waveform drawn: " << withWaveform);
    ManualClock clock;
    TestApp app(threeSoundSteps());
    app.waitHeads();
    waitPeaks(app);
    if (!withWaveform) AppTestAccess::dropPeaks(app.session());
    goAt(app, clock, 0);
    app.audio(5);
    auto& bar = AppTestAccess::barOf(AppTestAccess::row(app.main(), 1));
    requireHittable(bar);
    const int w = bar.getWidth(), y = bar.getHeight() / 2;
    const int64_t length = app.session().sources().fileLengthFrames(1);
    REQUIRE(length > 0);
    const auto frameAt = [&](int x) { return seekFrameForX(x, w, 0, length); };

    const juce::Point<int> from(juce::roundToInt(static_cast<double>(voice(app, 1)->positionFrame) * w / length), y);
    press(bar, from);
    for (const int x : {w * 6 / 10, w * 7 / 10, w * 8 / 10}) {
        drag(bar, from, {x, y});
        app.audio(3);
        REQUIRE(voice(app, 1)->positionFrame < frameAt(w * 6 / 10));  // still where it played: no seek yet
    }
    const int x = w * 4 / 10;  // inside the 5 s head
    release(bar, from, {x, y});
    app.audio(1);

    REQUIRE(std::abs(voice(app, 1)->positionFrame - frameAt(x)) <= kBlock);
}

TEST_CASE("a running bar draws its file's waveform once it is read") {
    ManualClock clock;
    TestApp app(threeSoundSteps());
    app.waitHeads();
    waitPeaks(app);
    goAt(app, clock, 0);
    app.audio(5);
    auto& bar = AppTestAccess::barOf(AppTestAccess::row(app.main(), 1));
    requireHittable(bar);
    REQUIRE(countColour(bar, juce::Colour(kWaveGrey)) > 0);
    REQUIRE(countColour(bar, juce::Colour(kTrackGrey)) == 0);

    AppTestAccess::dropPeaks(app.session());  // as before the summary was read
    app.tick();
    REQUIRE(countColour(bar, juce::Colour(kWaveGrey)) == 0);
    REQUIRE(countColour(bar, juce::Colour(kTrackGrey)) > 0);  // today's plain bar
}

TEST_CASE("a running bar's playhead spans the whole waveform and the wave sits on its own panel") {
    ManualClock clock;
    TestApp app(threeSoundSteps());
    app.waitHeads();
    waitPeaks(app);
    goAt(app, clock, 0);
    app.audio(5);
    auto& bar = AppTestAccess::barOf(AppTestAccess::row(app.main(), 1));
    requireHittable(bar);
    const auto image = bar.createComponentSnapshot(bar.getLocalBounds(), false, 1.0f);
    const int h = image.getHeight(), w = image.getWidth();
    const auto whiteIn = [&](int y) {
        for (int x = 0; x < w; ++x)
            if (image.getPixelAt(x, y) == juce::Colours::white) return true;
        return false;
    };
    // The wave stays 2 px inside the bar, so rows 2 and h-3 are the wave area's own top and bottom.
    REQUIRE(whiteIn(2));
    REQUIRE(whiteIn(h - 3));

    // Beside the playhead and above the wave lies the panel, not the row's background (transparent here).
    REQUIRE(image.getPixelAt(w - 10, 2) == juce::Colour(kPanelColour));
}

TEST_CASE("another show drops the old show's waveforms") {
    TestApp app(threeSoundSteps());  // 8 s files
    waitPeaks(app);
    const auto old = app.session().peaks(1);
    REQUIRE(old->frames == 8 * 48000);

    TempDir other;
    const auto folder = writeShow(other, threeSoundSteps(), 3.0);  // same file ids, other lengths
    std::string error;
    REQUIRE(app.session().open(folder, error, nullptr));
    AppTestAccess::showOpened(app.main());
    app.tick();
    for (FileId id = 1; id <= 3; ++id) REQUIRE(app.session().peaks(id) == nullptr);
    REQUIRE(old->frames == 8 * 48000);  // a copy someone still holds stays valid

    waitPeaks(app);
    for (FileId id = 1; id <= 3; ++id) REQUIRE(app.session().peaks(id)->frames == 3 * 48000);
}

TEST_CASE("a second finger does not move the seek") {
    ManualClock clock;
    TestApp app(threeSoundSteps());
    app.waitHeads();
    goAt(app, clock, 0);
    app.audio(5);
    auto& bar = AppTestAccess::barOf(AppTestAccess::row(app.main(), 1));
    requireHittable(bar);
    const int w = bar.getWidth(), y = bar.getHeight() / 2;
    const int64_t length = app.session().sources().fileLengthFrames(1);
    const auto frameAt = [&](int x) { return seekFrameForX(x, w, 0, length); };

    const juce::Point<int> a(2, y), b(w * 4 / 10, y), c(w / 2, y), d(w * 8 / 10, y);
    press(bar, a, 0);
    drag(bar, a, b, 0);
    press(bar, c, 1);  // finger 1 lands while finger 0 holds the bar
    drag(bar, c, d, 1);
    release(bar, c, d, 1);
    app.audio(1);
    REQUIRE(voice(app, 1)->positionFrame < frameAt(w * 4 / 10));  // finger 1 sought nothing

    release(bar, a, b, 0);
    app.audio(1);
    REQUIRE(std::abs(voice(app, 1)->positionFrame - frameAt(b.x)) <= kBlock);
}

TEST_CASE("Pause, GO refused, Resume, Esc while paused") {
    ManualClock clock;
    TestApp app(threeSoundSteps());
    app.waitHeads();
    goAt(app, clock, 0);
    app.audio(5);
    auto& main = app.main();
    auto& pause = AppTestAccess::pauseButton(main);
    auto& banner = AppTestAccess::pausedBanner(main);
    requireHittable(pause);
    REQUIRE(!app.session().snapshot().paused);
    REQUIRE(!banner.isVisible());

    tap(pause, centre(pause));
    app.audio(2);
    REQUIRE(app.session().snapshot().paused);
    requireHittable(banner);
    REQUIRE(pause.getButtonText() == "Resume (P)");

    const int next = app.session().controller().nextIndex();
    clock.advance(kPastGuardMs);
    AppTestAccess::go(main);
    // The exact words MainComponent flashes (en dash), as the rehearsal checklist quotes them.
    REQUIRE(app.session().status().message == juce::String::fromUTF8("Paused \xe2\x80\x93 press Resume"));
    REQUIRE(app.session().controller().nextIndex() == next);

    REQUIRE(!main.keyPressed(juce::KeyPress('p', juce::ModifierKeys::ctrlModifier, 'p')));  // Ctrl+P is not ours
    app.audio(2);
    REQUIRE(app.session().snapshot().paused);

    tap(pause, centre(pause));  // Resume
    app.audio(2);
    REQUIRE(!app.session().snapshot().paused);
    REQUIRE(!banner.isVisible());

    REQUIRE(main.keyPressed(juce::KeyPress('p', {}, 'p')));  // P pauses as the button does
    app.audio(2);
    REQUIRE(app.session().snapshot().paused);
    REQUIRE(main.keyPressed(juce::KeyPress(juce::KeyPress::escapeKey)));
    app.audio(5);
    REQUIRE(app.session().snapshot().numVoices == 0);
    REQUIRE(!app.session().snapshot().paused);
}

TEST_CASE("no Pause button in Show mode") {
    ManualClock clock;
    TestApp app(threeSoundSteps());
    app.waitHeads();
    goAt(app, clock, 0);
    app.audio(5);
    auto& pause = AppTestAccess::pauseButton(app.main());
    requireHittable(pause);  // Edit mode: there

    AppTestAccess::pickMode(app.main(), ShowMode::Show);
    app.tick();
    REQUIRE(app.session().mode() == ShowMode::Show);
    REQUIRE(!pause.isVisible());
    // P stays the app's key (keyPressed says it was used) even though Show mode has no Pause.
    REQUIRE(app.main().keyPressed(juce::KeyPress('p', {}, 'p')));
    app.audio(2);
    REQUIRE(!app.session().snapshot().paused);
}

}  // namespace bigtop::app

namespace bigtop::app {
namespace {
// Intro, Song A, Song B, Song C; Song A and Song B name a file the show does not have.
Show twoMissingInTheMiddle() {
    Show show = threeSoundSteps();
    Step extra = show.steps[2];
    extra.id = 4;
    extra.name = "Song C";
    extra.sound->file = 3;  // Song B's file, kept for Song C
    show.steps.push_back(extra);
    show.lastStepId = 4;
    show.files.erase(show.files.begin() + 1);  // Song A's file is gone from the show
    show.steps[1].sound->file = 98;
    show.steps[2].sound->file = 99;
    return show;
}
}  // namespace

TEST_CASE("GO on a missing file moves NEXT to the next playable step") {
    ManualClock clock;
    TestApp app(twoMissingInTheMiddle());
    app.waitHeads();
    auto& controller = app.session().controller();
    controller.selectNext(1);
    AppTestAccess::go(app.main());
    CHECK(app.session().snapshot().numVoices == 0);
    CHECK(controller.nextIndex() == 3);
    const auto a = app.session().currentAlert();
    REQUIRE(a.has_value());
    CHECK(a->text == juce::String::fromUTF8("Step 2 'Song A' has no playable file \xe2\x80\x93 NEXT moved to step 4"));
    clock.advance(1000);  // past the GO guard
    AppTestAccess::go(app.main());
    CHECK(controller.nextIndex() == 4);
}

TEST_CASE("GO with no playable step left keeps NEXT") {
    const ManualClock clock;
    Show show = twoMissingInTheMiddle();
    show.steps.pop_back();  // nothing playable after Song A
    show.files.pop_back();  // and Song C's file goes with it
    TestApp app(show);
    app.waitHeads();
    auto& controller = app.session().controller();
    controller.selectNext(1);
    AppTestAccess::go(app.main());
    CHECK(controller.nextIndex() == 1);
    const auto a = app.session().currentAlert();
    REQUIRE(a.has_value());
    CHECK(a->text == "No playable step after 'Song A'");
}

}  // namespace bigtop::app

namespace bigtop::app {
namespace {
Show ontimeShow(bool follow) {
    Show show = threeSoundSteps();
    show.settings.ontimeEnabled = true;
    show.settings.ontimeHost = "192.0.2.10";
    show.settings.ontimeFollowTransport = follow;
    show.steps[0].sound->ontimeCue = "OPEN";
    show.steps[1].sound->ontimeCue = "A 1";
    return show;
}
const juce::String kBase = "http://192.0.2.10:4001";
}  // namespace

TEST_CASE("GO sends the step's Ontime cue, a repeat GO sends it again, a refused GO sends nothing") {
    ManualClock clock;
    TestApp app(ontimeShow(false));
    app.waitHeads();
    AppTestAccess::go(app.main());
    clock.advance(1000);
    app.session().controller().selectNext(0);
    AppTestAccess::go(app.main());  // the same step again: a restart in Bigtop is a restart in Ontime
    clock.advance(1000);
    app.session().controller().pause();
    AppTestAccess::go(app.main());  // refused while paused
    CHECK(app.ontimeUrls() == std::vector<juce::String>{kBase + "/api/start/cue/OPEN", kBase + "/api/start/cue/OPEN"});
}

TEST_CASE("Pause, Resume and Esc reach Ontime only with the follow option") {
    for (const bool follow : {false, true}) {
        ManualClock clock;
        TestApp app(ontimeShow(follow));
        app.waitHeads();
        auto& main = app.main();
        auto& pause = AppTestAccess::pauseButton(main);
        pause.onClick();  // Pause
        pause.onClick();  // Resume (a second P without a release would count as a held key)
        REQUIRE(main.keyPressed(juce::KeyPress(juce::KeyPress::escapeKey)));
        const auto urls = app.ontimeUrls();
        if (follow)
            CHECK(urls == std::vector<juce::String>{kBase + "/api/pause", kBase + "/api/start", kBase + "/api/stop"});
        else
            CHECK(urls.empty());
    }
}

TEST_CASE("The Ontime light shows only when the show sends to Ontime") {
    const ManualClock clock;
    TestApp off(threeSoundSteps());
    off.tick();
    CHECK_FALSE(AppTestAccess::ontimeLight(off.main()).isVisible());
    TestApp on(ontimeShow(false));
    on.tick();
    CHECK(AppTestAccess::ontimeLight(on.main()).isVisible());
}

}  // namespace bigtop::app
