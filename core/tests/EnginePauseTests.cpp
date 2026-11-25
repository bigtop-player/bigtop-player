// EnginePauseTests.cpp: pause/resume of all step sounds and the
// Keep button's KeepTrimCommand. Rendered offline at 48 kHz so the
// 0.1 s pause fade is 4800 frames: 75 blocks of 64, ending on a block edge.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

#include "TestSources.h"

using Catch::Approx;
using namespace bigtop;

namespace {
constexpr double kRate = 48000.0;
constexpr int kPauseFrames = 4800;  // kPauseFadeSeconds at kRate

// Pauses and renders the whole pause fade, so step voices are frozen afterwards.
std::vector<float> pauseFully(Engine& e) {
    e.post(PauseCommand{true});
    return render(e, kPauseFrames);
}
}  // namespace

TEST_CASE("Pause fades step voices out over kPauseFadeSeconds, then output is silent") {
    Engine e(kRate); ConstSource a(480000); e.post(startCmd(1, a)); render(e, 640);
    e.post(PauseCommand{true});
    auto out = render(e, kPauseFrames + 64);
    // The largest frame-to-frame step a 0.1 s equal-power fade from 1 to 0 takes.
    Fader ref(1.0f); ref.fadeTo(0.0f, kPauseFrames);
    float maxRefStep = 0.0f, prev = ref.next();
    for (int i = 1; i <= kPauseFrames; ++i) { const float g = ref.next(); maxRefStep = std::max(maxRefStep, std::abs(g - prev)); prev = g; }
    float maxStep = std::abs(out[0] - 1.0f);
    for (size_t i = 1; i < out.size(); ++i) maxStep = std::max(maxStep, std::abs(out[i] - out[i - 1]));
    CHECK(maxStep <= maxRefStep + 1e-6f);
    CHECK(out[kPauseFrames / 2] > 0.0f);                       // still fading half way
    for (int i = kPauseFrames; i < kPauseFrames + 64; ++i) CHECK(std::abs(out[static_cast<size_t>(i)]) == 0.0f);
}
TEST_CASE("Positions freeze while paused") {
    Engine e(kRate); ConstSource a(480000); e.post(startCmd(1, a)); render(e, 640);
    pauseFully(e);
    const int64_t frozen = e.snapshot().voices[0].positionFrame;
    CHECK(frozen == 640 + kPauseFrames);                       // advanced during the fade, not after
    for (int block = 0; block < 48000 / 64; ++block) {
        CHECK(render(e, 64)[0] == 0.0f);
        CHECK(e.snapshot().voices[0].positionFrame == frozen);
    }
}
// Pending starts stop counting at once (they are silent, so there is nothing
// to fade): 0.5 s of a 1 s delay counted before the pause, 0.5 s after resume.
TEST_CASE("Start delays freeze while paused") {
    Engine e(kRate); ConstSource a(480000);
    auto c = startCmd(1, a); c.start.delayFrames = 48000; e.post(c); render(e, 24000);
    e.post(PauseCommand{true}); auto paused = render(e, 48000);
    CHECK(std::all_of(paused.begin(), paused.end(), [](float v) { return v == 0.0f; }));
    CHECK(e.snapshot().voices[0].pending);
    e.post(PauseCommand{false}); auto out = render(e, 24064);
    CHECK(out[23999] == 0.0f);                                 // not earlier
    CHECK(out[24000] == 1.0f);                                 // on time, resume fade long over
}
TEST_CASE("Resume continues from the frozen position and fades in over kPauseFadeSeconds") {
    Engine e(kRate); RampSource r(480000); e.post(startCmd(1, r)); render(e, 4800);
    pauseFully(e);
    const int64_t frozen = e.snapshot().voices[0].positionFrame;
    e.post(PauseCommand{false}); auto out = render(e, kPauseFrames + 64);
    Fader ref(0.0f); ref.fadeTo(1.0f, kPauseFrames);
    for (size_t k = 0; k < out.size(); ++k)
        CHECK(out[k] == Approx(static_cast<float>(frozen + static_cast<int64_t>(k)) * ref.next()));
}
TEST_CASE("Pads play on while paused") {
    Engine e(kRate); ConstSource step(480000), pad(480000);
    e.post(startCmd(1, step)); e.post(startCmd(kPadKeyBase, pad)); render(e, 640);
    pauseFully(e);
    auto out = render(e, 640);
    CHECK(out[0] == 1.0f); CHECK(out[639] == 1.0f);            // the pad alone, at full level
    const Snapshot s = e.snapshot();
    for (int i = 0; i < s.numVoices; ++i)
        if (s.voices[i].key == kPadKeyBase) CHECK(s.voices[i].positionFrame == 640 + kPauseFrames + 640);
}
TEST_CASE("Snapshot reports paused") {
    Engine e(kRate); render(e, 64);
    CHECK_FALSE(e.snapshot().paused);
    e.post(PauseCommand{true}); render(e, 64); CHECK(e.snapshot().paused);
    e.post(PauseCommand{true}); render(e, 64); CHECK(e.snapshot().paused);   // a second pause changes nothing
    e.post(PauseCommand{false}); render(e, 64); CHECK_FALSE(e.snapshot().paused);
}
// In rehearsal, Esc after pause means "reset": every step voice goes now, and
// the show is no longer paused.
TEST_CASE("Fade out all while paused stops every step voice and clears pause") {
    Engine e(kRate); ConstSource a(480000), b(480000), pad(480000);
    e.post(startCmd(1, a)); e.post(startCmd(2, b)); e.post(startCmd(kPadKeyBase, pad));
    auto pending = startCmd(3, a); pending.start.delayFrames = 48000; e.post(pending);
    render(e, 640);
    pauseFully(e);
    e.post(FadeOutAllCommand{96000}); auto out = render(e, 64);
    const Snapshot s = e.snapshot();
    CHECK_FALSE(s.paused);
    CHECK(std::count_if(s.voices, s.voices + s.numVoices, [](const VoiceState& v) { return v.key < kPadKeyBase; }) == 0);
    CHECK(s.numVoices == 1); CHECK(s.voices[0].key == kPadKeyBase);
    for (float v : out) CHECK(v == 1.0f);                      // only the pad, no blip of the stopped sounds
}
// Mid pause fade the sounds are still audible: they declick out from where the
// pause fade has got to instead of jumping.
TEST_CASE("Fade out all during the pause fade declicks from the faded level") {
    Engine e(kRate); ConstSource a(480000); e.post(startCmd(1, a)); render(e, 640);
    e.post(PauseCommand{true}); auto fading = render(e, kPauseFrames / 2);
    e.post(FadeOutAllCommand{96000}); auto out = render(e, 960);
    CHECK(std::abs(out[0] - fading.back()) < 0.01f);           // continues from the faded level
    CHECK(out[480] == 0.0f);                                   // declicked out within 10 ms
    CHECK(e.snapshot().numVoices == 0); CHECK_FALSE(e.snapshot().paused);
}
TEST_CASE("Seek while paused moves the frozen position; resume plays from there") {
    Engine e(kRate); RampSource r(480000); e.post(startCmd(1, r)); render(e, 4800);
    pauseFully(e);
    e.post(SeekCommand{1, 100000}); CHECK(render(e, 64)[0] == 0.0f);
    CHECK(e.snapshot().numVoices == 1); CHECK(e.snapshot().voices[0].positionFrame == 100000);
    e.post(PauseCommand{false}); auto out = render(e, 64);
    Fader ref(0.0f); ref.fadeTo(1.0f, kPauseFrames);
    for (size_t k = 0; k < out.size(); ++k)
        CHECK(out[k] == Approx(static_cast<float>(100000 + static_cast<int64_t>(k)) * ref.next()));
}
TEST_CASE("KeepTrim leaves the output unchanged and sets trim to 1") {
    Engine e(kRate); ConstSource a(480000);
    auto c = startCmd(1, a); c.start.gain = 0.5f; e.post(c); render(e, 64);
    e.post(TrimCommand{1, 2.0f}); render(e, 960);              // trim settled (declick is 480 frames)
    auto before = render(e, 64);
    e.post(KeepTrimCommand{1}); auto after = render(e, 64);
    for (size_t k = 0; k < after.size(); ++k) CHECK(std::abs(after[k] - before[k]) <= 1e-6f);
    CHECK(e.snapshot().voices[0].trim == 1.0f); CHECK(e.snapshot().voices[0].gain == Approx(1.0f));
}
// The programmed fade keeps its curve: both ends are scaled by the trim.
TEST_CASE("KeepTrim during a level fade keeps the fade curve") {
    Engine e(kRate), ref(kRate); ConstSource a(480000);
    StepCommand level; level.numChanges = 1; level.changes[0] = {1, ChangeKind::Level, 0.25f, 4800};
    for (Engine* x : {&e, &ref}) {
        x->post(startCmd(1, a)); x->post(TrimCommand{1, 2.0f}); render(*x, 960);
        x->post(level); render(*x, 1000);
    }
    e.post(KeepTrimCommand{1});
    auto out = render(e, 4800), want = render(ref, 4800);
    for (size_t k = 0; k < out.size(); k += 50) CHECK(std::abs(out[k] - want[k]) <= 1e-6f);
    CHECK(out.back() == Approx(0.5f));
}
TEST_CASE("KeepTrim clamps the programmed gain; an unknown key does nothing") {
    Engine e(kRate); ConstSource a(480000);
    auto c = startCmd(1, a); c.start.gain = kMaxProgrammedGain; e.post(c); e.post(TrimCommand{1, 2.0f}); render(e, 960);
    e.post(KeepTrimCommand{7}); render(e, 64);
    CHECK(e.snapshot().voices[0].trim == 2.0f);
    e.post(KeepTrimCommand{1}); render(e, 64);
    CHECK(e.snapshot().voices[0].gain == kMaxProgrammedGain); CHECK(e.snapshot().voices[0].trim == 1.0f);
}
// A 0-frame FadeOut would jump the gain to 0 (a click): it is stretched to
// the declick time. The others rule and fade-out-all go through the same path.
TEST_CASE("A FadeOut of 0 frames fades over the declick time instead of clicking") {
    Engine e(kRate); ConstSource a(480000); e.post(startCmd(1, a)); render(e, 640);
    StepCommand f; f.numChanges = 1; f.changes[0] = {1, ChangeKind::FadeOut, 0.0f, 0}; e.post(f);
    auto out = render(e, 480 + 64);
    Fader ref(1.0f); ref.fadeTo(0.0f, 480);
    float maxRefStep = 0.0f, prev = ref.next();
    for (int i = 1; i <= 480; ++i) { const float g = ref.next(); maxRefStep = std::max(maxRefStep, std::abs(g - prev)); prev = g; }
    float maxStep = std::abs(out[0] - 1.0f);
    for (size_t i = 1; i < out.size(); ++i) maxStep = std::max(maxStep, std::abs(out[i] - out[i - 1]));
    CHECK(maxStep <= maxRefStep + 1e-6f);
    CHECK(out[240] > 0.0f);                                    // still fading half way
    CHECK(e.snapshot().numVoices == 0);                        // ended within 10 ms + one block
}
// Per-sound Fade works while paused: a frozen voice is silent, so it just ends.
TEST_CASE("A FadeOut on a frozen voice ends it at once; resume does not bring it back") {
    Engine e(kRate); ConstSource a(480000), b(480000); e.post(startCmd(1, a)); e.post(startCmd(2, b)); render(e, 640);
    pauseFully(e);
    StepCommand f; f.numChanges = 1; f.changes[0] = {1, ChangeKind::FadeOut, 0.0f, 96000}; e.post(f); render(e, 64);
    CHECK(e.snapshot().numVoices == 1); CHECK(e.snapshot().voices[0].key == 2);
    e.post(PauseCommand{false}); auto out = render(e, kPauseFrames + 64);
    CHECK(out.back() == 1.0f);                                 // only step 2, back at full level
    CHECK(e.snapshot().numVoices == 1);
}
// A Level change of 0 s on a sounding voice would jump the gain (a click):
// like FadeOut it is stretched to the declick time.
TEST_CASE("A Level change of 0 frames fades over the declick time instead of jumping") {
    Engine e(kRate); ConstSource a(480000); e.post(startCmd(1, a)); render(e, 640);
    StepCommand c; c.numChanges = 1; c.changes[0] = {1, ChangeKind::Level, 0.25f, 0}; e.post(c);
    auto out = render(e, 480 + 64);
    CHECK(out[0] > 0.9f);                                      // not cut: still near the old level
    CHECK(out[240] > 0.25f); CHECK(out[240] < 1.0f);           // half way through the declick
    CHECK(out[480 + 10] == Approx(0.25f));                     // there after 10 ms
    CHECK(e.snapshot().numVoices == 1);
}
