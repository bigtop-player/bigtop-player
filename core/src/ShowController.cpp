#include "bigtop/ShowController.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "bigtop/Gain.h"
#include "bigtop/Timecode.h"

namespace bigtop {

// Step ids double as engine voice keys; they must never reach the pad range.
static_assert(kMaxStepId < kPadKeyBase, "step ids must stay below the pad keys");

namespace {
bool validStepId(StepId id) noexcept { return id != 0 && id <= kMaxStepId; }
}  // namespace

ShowController::ShowController(Engine& engine, SourceProvider& sources)
    : engine_(engine), sources_(sources) {
    // A pause left in the engine by an earlier controller (another show) would
    // freeze this show's first GO at gain 0 while paused() says false. The
    // engine ignores a resume when not paused; if the queue is full the resume
    // is not sent, so paused() says true and the UI offers Resume.
    paused_ = !engine_.post(PauseCommand{false});
}

void ShowController::setShow(Show show) {
    show_ = std::move(show);
    next_ = 0;
    levelSetter_.clear();
    lastGo_.reset();
}

StepId ShowController::levelSetter(StepId sound) const noexcept {
    const auto it = levelSetter_.find(sound);
    return it == levelSetter_.end() ? 0 : it->second;
}

void ShowController::updateShow(Show show) {
    // Remember the next step by id: its index changes when steps are inserted or moved.
    const bool hadNext = next_ < static_cast<int>(show_.steps.size());
    const StepId nextId = hadNext ? show_.steps[static_cast<size_t>(next_)].id : 0;
    show_ = std::move(show);
    const auto it = std::find_if(show_.steps.begin(), show_.steps.end(), [nextId](const Step& s) { return s.id == nextId; });
    if (hadNext && it != show_.steps.end()) next_ = static_cast<int>(it - show_.steps.begin());
    else next_ = std::clamp(next_, 0, static_cast<int>(show_.steps.size()));
}

const Show& ShowController::show() const noexcept { return show_; }

int ShowController::nextIndex() const noexcept { return next_; }

void ShowController::selectNext(int index) {
    next_ = std::clamp(index, 0, static_cast<int>(show_.steps.size()));
}

const Step* ShowController::findStep(StepId id) const noexcept {
    for (const Step& s : show_.steps)
        if (s.id == id) return &s;
    return nullptr;
}

double ShowController::fileTrimDb(FileId id) const noexcept {
    for (const AudioFile& f : show_.files)
        if (f.id == id) return f.trimDb;
    return 0.0;  // not listed: the provider may still have it, play it untrimmed
}

int64_t secondsToFrames(double seconds, double rate) noexcept {
    // Negative, NaN and infinite times all become 0. The upper cap keeps
    // llround defined for absurd finite values (1e18 frames is millions of
    // years, so nothing real is cut off).
    const double frames = seconds * rate;
    if (!std::isfinite(frames) || frames <= 0.0) return 0;
    return std::llround(std::min(frames, 1e18));
}

int64_t ShowController::toFrames(double seconds) const noexcept { return secondsToFrames(seconds, engine_.sampleRate()); }

float ShowController::toGain(double levelDb, FileId file) const noexcept {
    // std::min(NaN, x) returns NaN, so a NaN level survives to the check below.
    // The outer cap stops a corrupt file trim from overdriving the PA.
    const double db = std::min(std::min(levelDb, kMaxLevelDb) + fileTrimDb(file), kMaxTotalDb);
    return std::isfinite(db) ? static_cast<float>(dbToGain(db)) : 0.0f;
}

bool ShowController::makeStart(StartSpec& out, VoiceKey key, const Sound& sound) {
    // A start at or after an explicit end point is refused from the show
    // alone, before the provider opens the file (spec: a refused start opens
    // no file). Clamping the end to the length below can only shorten it.
    const int64_t start = toFrames(sound.startSeconds);
    if (sound.endSeconds && toFrames(*sound.endSeconds) <= start) return false;
    // Fetched afresh every time: the provider may have reloaded the file since.
    SoundSource* src = sources_.sourceFor(key, sound.file, start);
    if (src == nullptr) return false;
    const int64_t length = src->lengthFrames();
    const int64_t end = sound.endSeconds ? std::min(toFrames(*sound.endSeconds), length) : length;
    // Covers length 0 too. An unusable sound is refused here so the operator
    // sees MissingFile instead of a GO that silently plays nothing.
    if (start >= end) return false;
    out = StartSpec{key, src, start, end, sound.loop, toGain(sound.levelDb, sound.file),
                    toFrames(sound.fadeInSeconds), toFrames(sound.startDelaySeconds),
                    sound.timecodeStart.has_value(), sound.timecodeStart.value_or(Timecode{})};
    return true;
}

GoResult ShowController::go(double nowSeconds) {
    if (next_ >= static_cast<int>(show_.steps.size())) return GoResult::EndOfList;
    // Checked before the guard: a GO pressed while paused must show the
    // paused notice, not be swallowed as a double tap.
    if (paused_) return GoResult::Paused;
    // A clock that went backwards is accepted: a stuck guard would block GO.
    if (lastGo_ && nowSeconds >= *lastGo_ && nowSeconds - *lastGo_ < show_.settings.goGuardSeconds)
        return GoResult::IgnoredGuard;

    const Step& step = show_.steps[static_cast<size_t>(next_)];
    // An id in the pad range would play as a pad that fade out all skips.
    if (!validStepId(step.id)) return GoResult::MissingFile;
    StepCommand cmd;
    if (step.sound) {
        if (!makeStart(cmd.start, step.id, *step.sound)) return GoResult::MissingFile;
        cmd.hasStart = true;
    }
    for (const Change& ch : step.changes) {
        if (cmd.numChanges == kMaxChanges) break;  // the editor prevents more; use the first 16
        if (!validStepId(ch.target)) continue;     // a step must never fade or level a pad
        ChangeSpec spec{ch.target, ch.kind, 0.0f, toFrames(ch.seconds)};
        if (ch.kind == ChangeKind::Level) {
            // The level of a running sound includes its file trim, so the trim
            // of the file the target step plays is applied again here.
            const Step* target = findStep(ch.target);
            if (target == nullptr || !target->sound) continue;  // nothing to level
            spec.gain = toGain(ch.levelDb, target->sound->file);
        }
        // Other targets (own id, unknown id) pass through: the engine ignores
        // unknown keys, and changes are applied before this step's start.
        cmd.changes[cmd.numChanges++] = spec;
    }
    // Level is not valid for Others; the engine treats it as Keep, say so here too.
    if (step.others.kind == ChangeKind::FadeOut) {
        cmd.othersKind = ChangeKind::FadeOut;
        cmd.othersFrames = toFrames(step.others.seconds);
    }

    if (!engine_.post(cmd)) return GoResult::EngineBusy;
    // A non-finite time was let through above (fail open) but is not stored:
    // a stored NaN would make every later guard comparison false for good.
    if (std::isfinite(nowSeconds)) lastGo_ = nowSeconds;
    ++next_;
    // Changes first, then the start: a restart of the sound ends up its own setter.
    for (int i = 0; i < cmd.numChanges; ++i)
        if (cmd.changes[i].kind == ChangeKind::Level) levelSetter_[cmd.changes[i].target] = step.id;
    if (cmd.hasStart) levelSetter_[step.id] = step.id;
    return GoResult::Ran;
}

bool ShowController::fadeOutAll() {
    if (!engine_.post(FadeOutAllCommand{toFrames(show_.settings.defaultFadeSeconds)})) return false;
    paused_ = false;  // the engine ends a pause on fade-out-all (see PauseCommand)
    return true;
}

bool ShowController::pause() {
    if (!engine_.post(PauseCommand{true})) return false;
    paused_ = true;
    return true;
}

bool ShowController::resume() {
    if (!engine_.post(PauseCommand{false})) return false;
    paused_ = false;
    return true;
}

bool ShowController::paused() const noexcept { return paused_; }

bool ShowController::fadeOutSound(StepId step, double seconds) {
    // One FadeOut change and others Keep: only this sound is touched.
    StepCommand cmd;
    cmd.numChanges = 1;
    cmd.changes[0] = {step, ChangeKind::FadeOut, 0.0f, toFrames(seconds)};
    return engine_.post(cmd);
}

bool ShowController::keepTrim(StepId step) { return engine_.post(KeepTrimCommand{step}); }

bool ShowController::seek(StepId step, double seconds) {
    return engine_.post(SeekCommand{step, toFrames(seconds)});
}

bool ShowController::setLiveTrimDb(StepId step, double db) {
    return engine_.post(TrimCommand{step, static_cast<float>(dbToGain(db))});
}

bool ShowController::triggerPad(int index) {
    // A pad hidden by a smaller grid keeps its data but must not play.
    const int visible = show_.settings.padRows * show_.settings.padCols;
    if (index < 0 || index >= visible || static_cast<size_t>(index) >= show_.pads.size()) return false;
    const Pad& pad = show_.pads[static_cast<size_t>(index)];
    if (!pad.file) return false;
    const VoiceKey key = kPadKeyBase + static_cast<VoiceKey>(index);

    StepCommand cmd;  // others stays Keep: a pad never touches the step sounds
    const Snapshot snap = engine_.snapshot();
    const bool playing = std::any_of(snap.voices, snap.voices + snap.numVoices,
                                     [key](const VoiceState& v) { return v.key == key; });
    if (playing) {
        cmd.numChanges = 1;
        cmd.changes[0] = {key, ChangeKind::FadeOut, 0.0f, toFrames(kPadStopSeconds)};
    } else {
        Sound sound;  // whole file, no fade, no delay, no loop
        sound.file = *pad.file;
        sound.levelDb = pad.levelDb;
        if (!makeStart(cmd.start, key, sound)) return false;
        cmd.hasStart = true;
    }
    return engine_.post(cmd);
}

bool ShowController::levelPad(int index) {
    if (index < 0 || static_cast<size_t>(index) >= show_.pads.size()) return false;
    const Pad& pad = show_.pads[static_cast<size_t>(index)];
    if (!pad.file) return false;
    const VoiceKey key = kPadKeyBase + static_cast<VoiceKey>(index);
    const Snapshot snap = engine_.snapshot();
    const auto* end = snap.voices + snap.numVoices;
    const auto* voice = std::find_if(snap.voices, end, [key](const VoiceState& v) { return v.key == key; });
    // A Level cancels a FadeOut's stop (Engine::applyChange): a pad a tap is fading must keep fading.
    if (voice == end || voice->fadingOut) return false;
    StepCommand cmd;  // others stays Keep; 0 frames = the engine's declick ramp
    cmd.numChanges = 1;
    cmd.changes[0] = {key, ChangeKind::Level, toGain(pad.levelDb, *pad.file), 0};
    return engine_.post(cmd);
}

std::vector<StepId> ShowController::expectedRunningAt(int stepIndex) const {
    const int last = std::clamp(stepIndex, 0, static_cast<int>(show_.steps.size()));
    std::vector<StepId> running;
    const auto named = [](const Step& s, StepId id) {
        return std::any_of(s.changes.begin(), s.changes.end(),
                           [id](const Change& c) { return c.target == id; });
    };
    for (int i = 0; i < last; ++i) {
        const Step& s = show_.steps[static_cast<size_t>(i)];
        // Same order as the engine applies a StepCommand: changes, others, start.
        for (const Change& c : s.changes)
            if (c.kind == ChangeKind::FadeOut) std::erase(running, c.target);
        if (s.others.kind == ChangeKind::FadeOut)
            std::erase_if(running, [&](StepId id) { return !named(s, id); });
        if (s.sound) {
            std::erase(running, s.id);  // a restart keeps one entry, now the newest
            running.push_back(s.id);
        }
    }
    return running;
}

std::optional<Timecode> snapshotTimecode(const Snapshot& snap, double sampleRate, int fps) noexcept {
    if (snap.timecodeKey == 0 || !isSupportedFps(fps)) return std::nullopt;
    return timecodeAt(snap.timecodeStart, static_cast<double>(snap.timecodeFrame) / sampleRate, fps);
}

std::optional<Timecode> ShowController::currentTimecode() const {
    return snapshotTimecode(engine_.snapshot(), engine_.sampleRate(), show_.settings.timecodeFps);
}

}  // namespace bigtop
