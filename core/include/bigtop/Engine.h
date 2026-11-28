/// @file Engine.h
/// @brief The realtime mixer: plays up to kMaxVoices sounds ("voices") at once,
///        driven by commands from the control thread.
///
/// How it fits: the controller turns a GO on a show step into a StepCommand
/// (frames and linear gains already worked out) and post()s it. The audio
/// device calls process() for every block; process() first applies all queued
/// commands, then mixes the voices. Any thread can read a consistent
/// snapshot() of what is playing for the UI and for timecode output.
///
/// Thread contract: post() is called by one control thread, process() by the
/// audio thread, snapshot() by any thread. process() never blocks, allocates
/// or throws.
///
/// Timing: a command posted before a process() call takes effect at the first
/// frame of that call's block; StartSpec::delayFrames counts from there.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <variant>

#include "bigtop/Gain.h"
#include "bigtop/Show.h"
#include "bigtop/SoundSource.h"
#include "bigtop/SpscQueue.h"

namespace bigtop {

/// @brief Names a voice: a StepId for step sounds, or kPadKeyBase + pad index
///        for pads; 0 = none.
using VoiceKey = uint32_t;
/// @brief First key used for pads; keys below it are step voices.
inline constexpr VoiceKey kPadKeyBase = 0x80000000u;
/// @brief Number of voices that can play at once; further starts are dropped.
inline constexpr int kMaxVoices = 32;
/// @brief Maximum number of changes one StepCommand can carry.
inline constexpr int kMaxChanges = 16;
/// @brief Length in seconds of the short click-free fade used for hard stops
///        (restart, seek, second fade-out-all).
inline constexpr double kDeclickSeconds = 0.010;
/// @brief Highest live trim, as linear gain (+12 dB); larger TrimCommand gains
///        are clamped to it so a faulty controller cannot overdrive the PA.
inline constexpr float kMaxTrimGain = 3.9810717f;
/// @brief Highest programmed gain (StartSpec::gain and Level change gains), as
///        linear gain (+24 dB): the +12 dB level ceiling plus up to +12 dB of
///        loudness-normalisation boost. Larger gains are clamped to it.
/// @warning This caps one voice's programmed gain only, not the output. The
///          live trim (kMaxTrimGain, +12 dB) multiplies on top, so one voice
///          can reach +36 dB; up to kMaxVoices voices are summed and the
///          engine has no bus limiter.
inline constexpr float kMaxProgrammedGain = 15.848932f;
/// @brief Length in seconds of the fade out on pause and the fade in on resume
///        (PauseCommand).
inline constexpr double kPauseFadeSeconds = 0.1;

/// @brief How to start one voice.
struct StartSpec {
    VoiceKey key;           ///< @brief Key the new voice gets; changes address it by this.
    SoundSource* source;    ///< @brief Audio to play; must outlive the voice. Null = do not start.
    int64_t startFrame;     ///< @brief First source frame played (and the loop point); negative is clamped to 0.
    int64_t endFrame;       ///< @brief Source frame after the last one played; clamped to the source length.
    bool loop;              ///< @brief True: wrap from endFrame back to startFrame forever.
    float gain;             ///< @brief Programmed linear gain the voice plays at (after any fade-in); clamped to [0, kMaxProgrammedGain]; NaN/inf = 0.
    int64_t fadeInFrames;   ///< @brief Fade from silence to @c gain over this many frames; <= 0 = none.
    int64_t delayFrames;    ///< @brief Silence before the voice starts, counted from the block the command lands in; negative is clamped to 0.
    /// @brief True: this voice becomes the timecode driver at the frame it
    ///        starts to sound (when @c delayFrames has run out). While it is
    ///        pending the previous driver, if any, keeps driving; if it is
    ///        cancelled before it sounds, nothing changes.
    bool drivesTimecode;
    /// @brief Timecode shown when this voice is at its start point; meaningful
    ///        only when @c drivesTimecode. Published in Snapshot::timecodeStart
    ///        while the voice drives.
    Timecode timecodeStart;
};

/// @brief A change to one running (or pending) voice.
/// @note On a pending voice, FadeOut cancels it and Level sets the gain it will
///       start with. On a voice frozen by a pause (see PauseCommand), FadeOut
///       ends it at once (it is silent) and resume does not bring it back. A
///       FadeOut or Level on a sounding voice lasts at least kDeclickSeconds. A
///       non-finite Level gain is ignored.
struct ChangeSpec {
    VoiceKey target;   ///< @brief Key of the voice to change; missing voices are ignored.
    ChangeKind kind;   ///< @brief Keep, Level (fade to @c gain) or FadeOut (fade to 0, then end).
    float gain;        ///< @brief Target linear gain for Level; clamped to [0, kMaxProgrammedGain].
    int64_t frames;    ///< @brief Fade length in frames; on a sounding voice never shorter than kDeclickSeconds (0 = immediate, with that declick; no click).
};

/// @brief Everything one GO on a show step does, as one atomic command.
/// @note Applied at the first frame of the block, in this order: (1) @c changes,
///       (2) the others rule on step voices (key < kPadKeyBase) not named in
///       @c changes and not keyed @c start.key, (3) the start. Starting a key
///       that is already playing restarts it: the old voice loses its key and
///       fades out over StepCommand::restartFrames; an older copy of the same
///       step still fading from an earlier restart ends over kDeclickSeconds,
///       so a step never holds more than one fading copy. Pads and pending voices (still in
///       their start delay) are never touched by the others rule, so a GO
///       does not cancel an earlier step's pending start; an explicit change
///       naming a pending voice still applies (see ChangeSpec). Only the start
///       honours StartSpec::delayFrames.
struct StepCommand {
    bool hasStart = false;               ///< @brief True if @c start should be started.
    StartSpec start{};                   ///< @brief The sound this step starts.
    int numChanges = 0;                  ///< @brief Used entries in @c changes; clamped to 0..kMaxChanges.
    ChangeSpec changes[kMaxChanges]{};   ///< @brief Changes to named voices; a Keep entry exempts its target from the others rule.
    ChangeKind othersKind = ChangeKind::Keep;  ///< @brief What to do to unnamed sounding step voices: Keep or FadeOut (Level is treated as Keep); pending voices are skipped.
    int64_t othersFrames = 0;            ///< @brief Fade length for othersKind, in frames; shorter than kDeclickSeconds (0 included) fades over kDeclickSeconds.
    /// @brief Fade-out length, in frames, for a sounding voice that already holds
    ///        start.key (GO on the step that is playing): it loses its key and fades
    ///        out over this while the new voice starts. Shorter than kDeclickSeconds
    ///        (0 included) fades over kDeclickSeconds. A voice already fading out
    ///        keeps its own fade; a voice still in its start delay is cancelled.
    int64_t restartFrames = 0;
};

/// @brief Fade all step voices out (the panic/stop button). Cancels pending
///        step voices; pads are not touched.
/// @note Sent again while a previous fade-out-all is still fading (step voices
///       left, and no StepCommand in between that touches step voices: a
///       change aimed at a step key, an others FadeOut, or a step start), it
///       stops all step voices over kDeclickSeconds instead. Starting or
///       stopping a pad does not cancel this.
struct FadeOutAllCommand {
    int64_t frames;  ///< @brief Fade length in frames; at least kDeclickSeconds.
};

/// @brief Move a playing voice to a new source position without a click.
/// @note The voice is replaced by a copy at @c frame whose declick fader rises
///       over kDeclickSeconds while the old one loses its key and declicks
///       out. The copy keeps the programmed fade, trim and timecode role. An
///       unknown key or a pending voice: nothing happens. No free voice: the
///       seek is dropped (counted in Snapshot::droppedStarts), the sound plays on.
struct SeekCommand {
    VoiceKey key;   ///< @brief Voice to move.
    int64_t frame;  ///< @brief New source frame; clamped to [startFrame, endFrame - 1].
};

/// @brief Set a voice's trim (the operator's live fader: an extra linear gain
///        on top of the programmed level), smoothed over kDeclickSeconds.
/// @note A new voice always starts with trim 1. Unknown key: nothing happens.
struct TrimCommand {
    VoiceKey key;  ///< @brief Voice to trim.
    float gain;    ///< @brief Trim as linear gain; 1 = unchanged. Clamped to kMaxTrimGain; negative or NaN/inf is ignored.
};

/// @brief Pause or resume all step voices (the Pause / Resume button).
/// @note Pause: every step voice (any voice not started as a pad, including
///       ones fading out after a restart or seek) fades out over
///       kPauseFadeSeconds; once that fade is done they are neither rendered
///       nor advanced, so positions and the timecode frame freeze. Start
///       delays stop counting at once (a pending voice is silent, so there is
///       nothing to fade). Resume fades them back in over kPauseFadeSeconds
///       from where they froze; resuming during the pause fade turns it round
///       without a jump. Pads are never paused, and pads started while paused
///       play. A pause while paused, or a resume while not paused, does
///       nothing. While paused, a FadeOutAllCommand stops every step voice at
///       once (over kDeclickSeconds from their current level, so no click) and
///       ends the pause; a SeekCommand on a frozen voice just moves it.
struct PauseCommand {
    bool paused;  ///< @brief True = pause, false = resume.
};

/// @brief Fold a voice's live trim into its programmed gain (the Keep button):
///        in one frame the programmed gain (current value and fade target) is
///        multiplied by the current trim and the trim becomes 1, so the output
///        does not change.
/// @note If the trim is still smoothing towards a new value, its current
///       (mid-ramp) value is kept and the rest of that ramp is dropped. The
///       programmed gain is capped at kMaxProgrammedGain, so at that ceiling
///       the output can drop. Unknown key: nothing happens.
struct KeepTrimCommand {
    VoiceKey key;  ///< @brief Voice whose trim to keep.
};

/// @brief Any command the control thread can send to the engine.
using Command = std::variant<StepCommand, FadeOutAllCommand, SeekCommand, TrimCommand, PauseCommand, KeepTrimCommand>;

/// @brief What one voice is doing, as seen in a Snapshot.
struct VoiceState {
    VoiceKey key;           ///< @brief The voice's key.
    bool pending;           ///< @brief True while the start delay is still counting down.
    /// @brief True when the voice ends once its fade reaches silence (a FadeOut
    ///        change, fade out all, a per-sound fade); false after a Level change,
    ///        even one to silence, which keeps the voice alive.
    bool fadingOut;
    int64_t positionFrame;  ///< @brief Next source frame to be played.
    float gain;             ///< @brief Current programmed linear gain (mid-fade value while fading).
    float trim;             ///< @brief Current trim as linear gain.
};

/// @brief A consistent picture of the engine at the end of a process() block.
struct Snapshot {
    int numVoices = 0;                  ///< @brief Used entries in @c voices.
    /// @brief Voices that still have their key (not ones fading out after a
    ///        restart or seek), in slot order. The order is not stable (it can
    ///        change after a seek or restart); look voices up by key.
    VoiceState voices[kMaxVoices]{};
    /// @brief Key of the voice with drivesTimecode that most recently started
    ///        to sound, 0 = none. A pending voice never drives; no fallback to
    ///        an older one once the driver ends.
    VoiceKey timecodeKey = 0;
    int64_t timecodeFrame = 0;          ///< @brief That voice's next source frame.
    /// @brief The driver's StartSpec::timecodeStart; all zero when timecodeKey == 0.
    Timecode timecodeStart{};
    /// @brief Total frames rendered by process() since construction (blocks of
    ///        <= 0 frames add nothing); lets the io layer tell whether the audio
    ///        thread is still running.
    int64_t framesProcessed = 0;
    uint32_t droppedStarts = 0;         ///< @brief Starts and seeks refused since construction because all voices were busy; the UI should word it as "a start or seek was dropped".
    /// @brief True from the block a pause lands in until the block its resume
    ///        (or a fade-out-all while paused) lands in; see PauseCommand.
    bool paused = false;
};

/// @brief The realtime mixer. See the file comment for the thread contract.
/// @warning Worst-case level: each voice's programmed gain is capped at
///          +24 dB (kMaxProgrammedGain) and its live trim at +12 dB
///          (kMaxTrimGain), so one voice can reach +36 dB; up to kMaxVoices
///          voices are summed and there is no bus limiter.
/// @note The object is about 30 KB (mostly the 64-command queue); owners
///       should not put it on a small stack (create it on the heap in the app).
class Engine {
public:
    /// @brief Creates a silent engine.
    /// @param sampleRate Frames per second that sources and frame counts use.
    /// @note Control thread, before audio starts.
    explicit Engine(double sampleRate);

    /// @brief The sample rate given at construction.
    /// @return Frames per second.
    /// @note Any thread.
    double sampleRate() const noexcept;

    /// @brief Queues a command for the audio thread (capacity 64 commands).
    /// @param command [in] Command to copy into the queue.
    /// @return False when the queue is full; the command is then not sent.
    /// @note Exactly one control thread may call this. Lock-free, never blocks.
    bool post(const Command& command) noexcept;

    /// @brief Applies queued commands, then renders one block of audio.
    /// @param left [out] Left channel, @p frames samples; overwritten.
    /// @param right [out] Right channel, @p frames samples; overwritten.
    /// @param frames [in] Block length; <= 0 does nothing (no commands applied).
    /// @note Audio thread only. Never blocks, allocates or throws.
    void process(float* left, float* right, int frames) noexcept;

    /// @brief A consistent copy of the state at the end of the last block.
    /// @return The snapshot; all zero before the first process().
    /// @note Any thread; several readers may call it at once. The published
    ///       snapshot is held as an array of atomic 32-bit words guarded by a
    ///       sequence counter (odd while process() writes it). The reader
    ///       loads all words and retries if the counter was odd or changed
    ///       meanwhile, so it may spin briefly but never sees a torn copy. The
    ///       audio thread never waits on readers. No locks, no allocation.
    Snapshot snapshot() const noexcept;

private:
    // One playing (or pending) sound. A slot is free when source == nullptr.
    struct Voice {
        VoiceKey key = 0;             // 0 once the voice lost its key (fading out after a restart, seek or second fade-out-all)
        VoiceKey formerKey = 0;       // the step key a restart took away; lets the next restart cut this copy
        SoundSource* source = nullptr;
        int64_t position = 0;         // next source frame to read
        int64_t startFrame = 0;
        int64_t endFrame = 0;
        bool loop = false;
        Fader programmed{0.0f};       // fade-in and level changes (braced: Fader's ctor is explicit)
        Fader declick{1.0f};          // kDeclickSeconds fades for hard stops and the rise of a seek copy
        Fader trim{1.0f};             // operator's live fader, smoothed against zipper noise
        int64_t delay = 0;            // frames left before the voice sounds
        int64_t fadeInFrames = 0;     // kept so a Level change on a pending voice keeps its fade-in
        bool stopWhenSilent = false;  // end the voice once a fade to 0 (programmed or declick) has finished
        bool drivesTimecode = false;  // takes the timecode role when delay reaches 0
        Timecode timecodeStart{};     // published while this voice drives; copied with it on a seek
        bool pad = false;             // started with a pad key; kept when the key is lost, so pause still skips it
        bool declickEnd = false;      // an end point before the file's end (not looping): ramp out before it
        bool endRampStarted = false;  // that ramp has begun
    };

    void drainCommands() noexcept;
    void applyStep(const StepCommand& command) noexcept;
    void applyChange(Voice& v, ChangeKind kind, float gain, int64_t frames) noexcept;
    void fadeOutAll(int64_t frames) noexcept;
    void seek(VoiceKey key, int64_t frame) noexcept;
    void trim(VoiceKey key, float gain) noexcept;
    void keepTrim(VoiceKey key) noexcept;
    void pause(bool paused) noexcept;
    bool frozen(const Voice& v) const noexcept;
    void retire(Voice& v) noexcept;
    void endVoice(Voice& v) noexcept;
    int64_t declickFrames() const noexcept;
    void startVoice(const StartSpec& spec, int64_t restartFrames) noexcept;
    void renderVoice(Voice& v, float* left, float* right, const float* pauseGain, int frames) noexcept;
    void publishSnapshot() noexcept;

    double sampleRate_;
    SpscQueue<Command, 64> queue_;
    Voice voices_[kMaxVoices];
    uint32_t droppedStarts_ = 0;
    int64_t framesProcessed_ = 0;    // audio thread only; published in the snapshot
    bool fadeOutAllFading_ = false;  // a fade-out-all is fading step voices (see FadeOutAllCommand)
    bool paused_ = false;            // a pause is in force (see PauseCommand); published in the snapshot
    Fader pause_{1.0f};              // the pause fade shared by every step voice; steps once per frame
    // The timecode driver, tracked by slot so the role follows a seek copy;
    // null once it ends or is retired (no fallback to an older voice).
    Voice* timecodeVoice_ = nullptr;
    // Published snapshot as atomic words, so readers on other threads never
    // touch memory the audio thread is writing non-atomically (defined
    // behaviour, unlike a plain-struct seqlock). Zero-initialised words
    // decode to an all-zero Snapshot.
    static_assert(std::is_trivially_copyable_v<Snapshot>, "Snapshot is copied as raw words");
    static_assert(std::atomic<uint32_t>::is_always_lock_free, "snapshot words must be lock-free");
    static constexpr size_t kSnapshotWords = (sizeof(Snapshot) + sizeof(uint32_t) - 1) / sizeof(uint32_t);
    std::atomic<uint32_t> snapshotSeq_{0};  // odd while process() writes snapshotWords_
    std::atomic<uint32_t> snapshotWords_[kSnapshotWords]{};
};

}  // namespace bigtop
