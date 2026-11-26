#include "bigtop/Engine.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace bigtop {

namespace {

// Scratch size for reading source audio on the stack. process() splits larger
// blocks into passes of at most this many frames, so the stack use per voice
// is bounded (2 x 2 KiB) no matter what block size the device picks.
constexpr int kMaxPassFrames = 512;

// Most commands applied per process() call: the queue capacity. A control
// thread posting nonstop then cannot keep the audio thread in the drain loop;
// anything left over is applied on the next block.
constexpr int kMaxCommandsPerBlock = 64;

// Step voices are the ones the others rule and fade-out-all act on. Key 0 is a
// retired voice (still sounding, no longer addressable); pads sit at kPadKeyBase+.
bool isStepVoice(VoiceKey key) noexcept { return key != 0 && key < kPadKeyBase; }

}  // namespace

Engine::Engine(double sampleRate) : sampleRate_(sampleRate) {}

double Engine::sampleRate() const noexcept { return sampleRate_; }

bool Engine::post(const Command& command) noexcept { return queue_.push(command); }

void Engine::process(float* left, float* right, int frames) noexcept {
    // A zero-length block has no "first frame" for commands to land on, so
    // leave the queue alone and the buffers untouched.
    if (frames <= 0) return;
    drainCommands();
    for (int offset = 0; offset < frames; offset += kMaxPassFrames) {
        const int n = std::min(kMaxPassFrames, frames - offset);
        std::fill(left + offset, left + offset + n, 0.0f);
        std::fill(right + offset, right + offset + n, 0.0f);
        // The pause fade is one engine-wide fader, so it steps once per frame
        // here and every step voice reads the same per-frame gain. Whether
        // step voices are frozen is decided per pass, before stepping, so the
        // pass in which the fade ends still plays it out (and the rest of
        // that pass at gain 0); null tells renderVoice they are frozen.
        float pauseGain[kMaxPassFrames];
        const bool stepsFrozen = paused_ && !pause_.moving();
        for (int k = 0; k < n; ++k) pauseGain[k] = pause_.next();
        for (Voice& v : voices_)
            if (v.source) renderVoice(v, left + offset, right + offset, stepsFrozen ? nullptr : pauseGain, n);
    }
    framesProcessed_ += frames;
    publishSnapshot();
}

void Engine::drainCommands() noexcept {
    Command command;
    for (int i = 0; i < kMaxCommandsPerBlock && queue_.pop(command); ++i) {
        // get_if rather than std::visit: visit may throw bad_variant_access,
        // and nothing on this thread may throw.
        if (const auto* step = std::get_if<StepCommand>(&command)) applyStep(*step);
        else if (const auto* all = std::get_if<FadeOutAllCommand>(&command)) fadeOutAll(all->frames);
        else if (const auto* sk = std::get_if<SeekCommand>(&command)) seek(sk->key, sk->frame);
        else if (const auto* tr = std::get_if<TrimCommand>(&command)) trim(tr->key, tr->gain);
        else if (const auto* pa = std::get_if<PauseCommand>(&command)) pause(pa->paused);
        else if (const auto* kt = std::get_if<KeepTrimCommand>(&command)) keepTrim(kt->key);
    }
}

void Engine::applyStep(const StepCommand& command) noexcept {
    // Show data is not trusted to keep the count in range.
    const int numChanges = std::clamp(command.numChanges, 0, kMaxChanges);
    // A new GO on the show ends any fade-out-all "still fading" state: a later
    // fade-out-all is a fresh fade, not the hurry-up stop. Only commands that
    // touch step sounds count as such a GO: a change aimed at a step key, an
    // others FadeOut, or a step start. Starting or stopping a pad (or an empty
    // command) must not slow the panic stop down.
    bool touchesSteps = command.othersKind == ChangeKind::FadeOut ||
                        (command.hasStart && isStepVoice(command.start.key));
    for (int c = 0; c < numChanges && !touchesSteps; ++c) touchesSteps = isStepVoice(command.changes[c].target);
    if (touchesSteps) fadeOutAllFading_ = false;

    // (1) Explicit changes. Target 0 would match retired voices, which must
    // never be touched, so it is skipped.
    for (int c = 0; c < numChanges; ++c) {
        const ChangeSpec& change = command.changes[c];
        if (change.target == 0) continue;
        for (Voice& v : voices_)
            if (v.source && v.key == change.target) applyChange(v, change.kind, change.gain, change.frames);
    }

    // (2) The others rule. Only FadeOut is meaningful (there is no others
    // gain); anything else leaves the voices alone. Pending voices are
    // skipped: another GO must not cancel a start that is still in its delay
    // (only an explicit FadeOut on that key, or fade-out-all, cancels it).
    if (command.othersKind == ChangeKind::FadeOut) {
        for (Voice& v : voices_) {
            if (!v.source || !isStepVoice(v.key) || v.delay > 0) continue;
            if (command.hasStart && v.key == command.start.key) continue;  // restarted in (3) instead
            bool named = false;
            for (int c = 0; c < numChanges && !named; ++c) named = command.changes[c].target == v.key;
            if (!named) applyChange(v, ChangeKind::FadeOut, 0.0f, command.othersFrames);
        }
    }

    // (3) The start (which retires a voice already holding the key).
    if (command.hasStart) startVoice(command.start, command.restartFrames);
}

void Engine::applyChange(Voice& v, ChangeKind kind, float gain, int64_t frames) noexcept {
    const bool pending = v.delay > 0;
    switch (kind) {
    case ChangeKind::Keep:
        break;
    case ChangeKind::Level:
        if (!std::isfinite(gain)) break;  // a NaN/inf gain must never reach the mix
        gain = std::clamp(gain, 0.0f, kMaxProgrammedGain);  // defence in depth: protect the PA
        if (pending) {
            // Not sounding yet: just change the gain it will start with,
            // keeping its own fade-in.
            v.programmed = Fader(v.fadeInFrames > 0 ? 0.0f : gain);
            if (v.fadeInFrames > 0) v.programmed.fadeTo(gain, v.fadeInFrames);
        } else {
            // A cut would jump the gain (a click): never faster than the declick time.
            v.programmed.fadeTo(gain, std::max(frames, declickFrames()));
        }
        v.stopWhenSilent = false;  // a Level after a FadeOut keeps the sound alive
        break;
    case ChangeKind::FadeOut:
        // Never sounded, or frozen by a pause (silent): just end it, no click
        // possible, and resume must not bring it back.
        if (pending || frozen(v)) { endVoice(v); break; }
        // A cut (0 frames) would jump the gain to 0, a click: never fade out
        // faster than the declick time.
        v.programmed.fadeTo(0.0f, std::max(frames, declickFrames()));
        v.stopWhenSilent = true;  // renderVoice ends it once the fade reaches 0
        break;
    }
}

void Engine::fadeOutAll(int64_t frames) noexcept {
    if (paused_) {
        // While paused, Esc means "reset": every step voice goes now and the
        // pause ends. Once the pause fade is done they are silent and simply
        // freed. Mid pause fade they are still audible, so each declicks out
        // from its current level times the pause gain; that lets the pause
        // fader jump back to 1 below without a blip.
        const float level = pause_.current();
        for (Voice& v : voices_) {
            if (!v.source || v.pad) continue;
            if (level == 0.0f || v.delay > 0) { endVoice(v); continue; }
            v.declick = Fader(v.declick.current() * level);
            retire(v);
        }
        paused_ = false;
        pause_ = Fader(1.0f);
        fadeOutAllFading_ = false;
        return;
    }
    bool anyStep = false;
    for (const Voice& v : voices_) anyStep = anyStep || (v.source && isStepVoice(v.key) && v.delay == 0);
    // A second press while the first is still fading means "now": stop over
    // the declick time. Retiring makes them unaddressable, so a later change
    // cannot revive a voice whose declick is heading to 0.
    const bool hurry = fadeOutAllFading_ && anyStep;
    for (Voice& v : voices_) {
        // A step sound that lost its key (the old copy of a restarted step, fading out
        // over the step's everything-else time) has no row and no fader: Esc is the only
        // way to stop it, so it ends now, over the declick time.
        if (v.source && v.key == 0 && !v.pad) {
            if (v.delay > 0) endVoice(v);
            else retire(v);
            continue;
        }
        if (!v.source || !isStepVoice(v.key)) continue;
        if (v.delay > 0) { endVoice(v); continue; }  // pending starts are cancelled
        if (hurry) retire(v);
        else applyChange(v, ChangeKind::FadeOut, 0.0f, frames);
    }
    fadeOutAllFading_ = anyStep && !hurry;
}

void Engine::seek(VoiceKey key, int64_t frame) noexcept {
    // Key 0 would match retired voices; a pending voice has nothing to jump in.
    if (key == 0) return;
    Voice* old = nullptr;
    for (Voice& v : voices_)
        if (v.source && v.key == key && v.delay == 0) { old = &v; break; }
    if (!old) return;
    // A frozen voice is silent, so it can jump in place: no crossfade needed,
    // and resume then fades in from the new position.
    if (frozen(*old)) {
        old->position = std::clamp(frame, old->startFrame, old->endFrame - 1);
        return;
    }
    // A copy made by a seek in this same block has not played a frame yet (its
    // rising declick is still at 0): just move it. Otherwise a burst of seeks
    // would leave one silent copy per seek holding a slot until the block ends.
    if (old->declick.moving() && old->declick.current() == 0.0f) {
        old->position = std::clamp(frame, old->startFrame, old->endFrame - 1);
        return;
    }
    Voice* slot = nullptr;
    for (Voice& v : voices_)
        if (!v.source) { slot = &v; break; }
    // Same fail-safe as restart: without a free voice the jump is refused and
    // counted, and the sound keeps playing where it is.
    if (!slot) { ++droppedStarts_; return; }

    // The copy carries the whole voice state (programmed fade in progress,
    // stopWhenSilent, trim ramp, timecode flag); only the position and the
    // declick differ. Copy before retire() so the copy keeps the key.
    const bool wasDriver = timecodeVoice_ == old;
    *slot = *old;
    slot->position = std::clamp(frame, old->startFrame, old->endFrame - 1);
    slot->endRampStarted = false;  // a jump back before the end point needs its end ramp again
    // Rising sin against the old voice's falling cos: an equal-power join.
    slot->declick = Fader(0.0f);
    slot->declick.fadeTo(1.0f, declickFrames());
    retire(*old);
    if (wasDriver) timecodeVoice_ = slot;
}

void Engine::trim(VoiceKey key, float gain) noexcept {
    // Trim comes from a live control surface: never let NaN/inf or a phase
    // flip reach the mix.
    if (key == 0 || !std::isfinite(gain) || gain < 0.0f) return;
    gain = std::min(gain, kMaxTrimGain);  // finite but huge (faulty controller): cap at +12 dB
    for (Voice& v : voices_) {
        if (!v.source || v.key != key) continue;
        // A pending voice is silent, so it can take the new trim at once; a
        // sounding one ramps to avoid zipper noise.
        if (v.delay > 0) v.trim = Fader(gain);
        else v.trim.fadeTo(gain, declickFrames());
    }
}

void Engine::keepTrim(VoiceKey key) noexcept {
    if (key == 0) return;  // would match retired voices
    for (Voice& v : voices_) {
        if (!v.source || v.key != key) continue;
        // Scaling both ends of the programmed fade by the trim's current value
        // keeps programmed x trim the same in every frame from here on (a
        // trim still ramping keeps its mid-ramp value; see KeepTrimCommand).
        v.programmed.scale(v.trim.current(), kMaxProgrammedGain);
        v.trim = Fader(1.0f);
    }
}

void Engine::pause(bool paused) noexcept {
    if (paused == paused_) return;  // a repeated pause or resume must not restart the fade
    paused_ = paused;
    // fadeTo starts from the current value, so a resume during the pause fade
    // (or the reverse) turns round without a jump.
    pause_.fadeTo(paused ? 0.0f : 1.0f, static_cast<int64_t>(std::llround(kPauseFadeSeconds * sampleRate_)));
}

bool Engine::frozen(const Voice& v) const noexcept {
    // Frozen = paused and the pause fade has reached 0: silent, so it neither
    // renders nor advances. During the fade itself step voices still play.
    return !v.pad && paused_ && !pause_.moving();
}

int64_t Engine::declickFrames() const noexcept {
    return static_cast<int64_t>(std::llround(kDeclickSeconds * sampleRate_));
}

void Engine::endVoice(Voice& v) noexcept {
    // Every place that frees a slot goes through here, so a stale driver
    // pointer can never land on a different voice that later reuses the slot.
    if (timecodeVoice_ == &v) timecodeVoice_ = nullptr;
    v = Voice{};
}

void Engine::retire(Voice& v) noexcept {
    // A retired voice is no longer the show's sound; a seek re-assigns the
    // timecode role to its copy, a restart to the new voice if it drives.
    if (timecodeVoice_ == &v) timecodeVoice_ = nullptr;
    v.key = 0;
    v.declick.fadeTo(0.0f, declickFrames());
    v.stopWhenSilent = true;
}

void Engine::startVoice(const StartSpec& spec, int64_t restartFrames) noexcept {
    // Invalid starts are ignored quietly: they are a show-data problem, not a
    // voice shortage, so they do not count as dropped starts.
    if (!spec.source) return;
    // A NaN/inf gain must never reach the mix; play the voice silent instead.
    // Finite gains are capped too, so a faulty controller cannot overdrive the PA.
    const float gain = std::isfinite(spec.gain) ? std::clamp(spec.gain, 0.0f, kMaxProgrammedGain) : 0.0f;
    const int64_t startFrame = std::max<int64_t>(spec.startFrame, 0);
    const int64_t endFrame = std::min(spec.endFrame, spec.source->lengthFrames());
    if (startFrame >= endFrame) return;

    Voice* slot = nullptr;
    for (Voice& v : voices_)
        if (!v.source) { slot = &v; break; }
    // All voices busy: never steal a running sound mid-show; refuse and count.
    if (!slot) { ++droppedStarts_; return; }

    // Restart rule, applied only once the new voice is sure to start, so a
    // dropped or invalid start never silences the old one. A pending old
    // voice never sounded and is simply cancelled, as is one frozen by a pause
    // (silent). A sounding one loses its key to the new voice and fades out
    // like any sound a GO ends: over restartFrames (never faster than the
    // declick), unless it already fades out, which keeps its own fade.
    for (Voice& old : voices_) {
        if (!old.source || old.key == 0 || old.key != spec.key) continue;
        if (old.delay > 0 || frozen(old)) { endVoice(old); continue; }
        if (timecodeVoice_ == &old) timecodeVoice_ = nullptr;  // the new voice takes the role if it drives
        old.key = 0;
        if (!old.stopWhenSilent) {
            old.programmed.fadeTo(0.0f, std::max(restartFrames, declickFrames()));
            old.stopWhenSilent = true;
        }
    }

    Voice& v = *slot;
    v = Voice{};
    v.key = spec.key;
    v.source = spec.source;
    v.position = startFrame;
    v.startFrame = startFrame;
    v.endFrame = endFrame;
    v.loop = spec.loop;
    v.delay = std::max<int64_t>(spec.delayFrames, 0);
    v.drivesTimecode = spec.drivesTimecode;
    v.timecodeStart = spec.timecodeStart;
    v.pad = spec.key >= kPadKeyBase;
    // The newest sounding timecode voice drives. A pending one takes the role
    // in renderVoice when its delay runs out, so the current driver keeps
    // going meanwhile and lighting cues do not fire early by the delay.
    if (spec.drivesTimecode && v.delay == 0) timecodeVoice_ = &v;
    v.fadeInFrames = std::max<int64_t>(spec.fadeInFrames, 0);
    // The fader only advances once the delay is over (see renderVoice), so a
    // delayed fade-in still starts from silence when the sound starts.
    if (spec.fadeInFrames > 0) {
        v.programmed = Fader(0.0f);
        v.programmed.fadeTo(gain, spec.fadeInFrames);
    } else {
        v.programmed = Fader(gain);
        // A start point inside the music cuts in mid-waveform: rise over the declick.
        // A start at the file's beginning plays as recorded (files start from silence).
        if (startFrame > 0) {
            v.declick = Fader(0.0f);
            v.declick.fadeTo(1.0f, declickFrames());
        }
    }
    // Likewise an end point before the file's end: ramp out over the declick just before it.
    v.declickEnd = !spec.loop && endFrame < spec.source->lengthFrames();
}

void Engine::renderVoice(Voice& v, float* left, float* right, const float* pauseGain, int frames) noexcept {
    // Paused: a pending step voice does not count its delay down (it is
    // silent, so this holds from the first paused block), and a frozen one
    // (pauseGain null, see process) does nothing at all.
    if (!v.pad && (pauseGain == nullptr || (paused_ && v.delay > 0))) return;
    int i = 0;
    if (v.delay > 0) {
        const int wait = static_cast<int>(std::min<int64_t>(v.delay, frames));
        v.delay -= wait;
        i = wait;
        // The delay is over: from here on this voice sounds, so it takes the
        // timecode role (see startVoice).
        // shortcut: two timecode voices whose delays end in the same pass are
        // ordered by slot, not by frame; track the start frame if that matters.
        if (v.delay == 0 && v.drivesTimecode) timecodeVoice_ = &v;
    }
    float srcL[kMaxPassFrames];
    float srcR[kMaxPassFrames];
    while (i < frames) {
        // Read up to the end point (or the end of this pass, if sooner).
        int64_t upTo = v.endFrame;
        if (v.declickEnd && !v.endRampStarted) {
            // Stop at the ramp's first frame, start the ramp there, then go on to the end point.
            const int64_t rampAt = std::max(v.endFrame - declickFrames(), v.startFrame);
            if (v.position >= rampAt) {
                v.declick.fadeTo(0.0f, std::max<int64_t>(v.endFrame - v.position, 1));
                v.endRampStarted = true;
            } else {
                upTo = rampAt;
            }
        }
        const int n = static_cast<int>(std::min<int64_t>(frames - i, upTo - v.position));
        v.source->read(v.position, srcL, srcR, n);
        for (int k = 0; k < n; ++k) {
            // Output = sample x programmed x declick x trim. Each fader steps
            // exactly once per frame so fade lengths stay sample-exact.
            // Pads ignore the pause gain.
            const float g = v.programmed.next() * v.declick.next() * v.trim.next() * (v.pad ? 1.0f : pauseGain[i + k]);
            left[i + k] += srcL[k] * g;
            right[i + k] += srcR[k] * g;
        }
        v.position += n;
        i += n;
        if (v.position >= v.endFrame) {
            // Looping wraps within the same pass, so there is no gap at the seam.
            if (v.loop) { v.position = v.startFrame; continue; }
            endVoice(v);
            return;
        }
    }
    // A fade to silence that has finished ends the voice: a FadeOut change,
    // the others rule or fade-out-all (programmed fader at 0), or a restart,
    // seek or second fade-out-all (declick fader at 0).
    if (v.stopWhenSilent && ((!v.programmed.moving() && v.programmed.target() == 0.0f) ||
                             (!v.declick.moving() && v.declick.target() == 0.0f)))
        endVoice(v);
}

void Engine::publishSnapshot() noexcept {
    // Build the plain struct first, outside the odd window, so readers retry
    // for as short a time as possible.
    // Zero every byte (padding included) and set fields one by one rather
    // than assigning whole structs, so the words copied out below are all
    // defined. The void* cast tells GCC the raw memset is intended.
    Snapshot s;
    std::memset(static_cast<void*>(&s), 0, sizeof(Snapshot));
    for (const Voice& v : voices_) {
        // Voices that lost their key (fading out after restart/seek) are not shown.
        if (!v.source || v.key == 0) continue;
        VoiceState& out = s.voices[s.numVoices++];
        out.key = v.key;
        out.pending = v.delay > 0;
        out.fadingOut = v.stopWhenSilent;
        out.positionFrame = v.position;
        out.gain = v.programmed.current();
        out.trim = v.trim.current();
    }
    s.droppedStarts = droppedStarts_;
    if (timecodeVoice_) {
        s.timecodeKey = timecodeVoice_->key;
        s.timecodeFrame = timecodeVoice_->position;
        s.timecodeStart = timecodeVoice_->timecodeStart;
    }
    s.framesProcessed = framesProcessed_;
    s.paused = paused_;

    uint32_t words[kSnapshotWords] = {};
    std::memcpy(words, &s, sizeof(Snapshot));

    // Sequence-counter writer (only this thread writes, so a relaxed load of
    // our own counter is fine). Odd store, then a release fence: any reader
    // that sees even one new word (its acquire fence pairs with this one)
    // must also see the odd counter on its re-check, and retry. The final
    // release store orders all word stores before the even counter, so a
    // reader that acquires the even value sees every word of this snapshot.
    // The audio thread never waits for readers.
    const uint32_t seq = snapshotSeq_.load(std::memory_order_relaxed);
    snapshotSeq_.store(seq + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    for (size_t i = 0; i < kSnapshotWords; ++i) snapshotWords_[i].store(words[i], std::memory_order_relaxed);
    snapshotSeq_.store(seq + 2, std::memory_order_release);
}

Snapshot Engine::snapshot() const noexcept {
    // Sequence-counter reader: every access is atomic, so overlapping a write
    // is defined behaviour; a mixed copy is detected and discarded.
    uint32_t words[kSnapshotWords];
    for (;;) {
        // Acquire pairs with the writer's final release: we see all its words.
        const uint32_t before = snapshotSeq_.load(std::memory_order_acquire);
        if (before & 1u) continue;
        for (size_t i = 0; i < kSnapshotWords; ++i) words[i] = snapshotWords_[i].load(std::memory_order_relaxed);
        // Acquire fence keeps the word loads before the re-check below (and
        // pairs with the writer's release fence after its odd store).
        std::atomic_thread_fence(std::memory_order_acquire);
        if (snapshotSeq_.load(std::memory_order_relaxed) == before) break;
    }
    Snapshot copy;
    std::memcpy(&copy, words, sizeof(Snapshot));
    return copy;
}

}  // namespace bigtop
