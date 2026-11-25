/// @file ShowController.h
/// @brief The control-thread layer between the Show model and the Engine: what
///        the GO button, "fade out all", the pads, the playhead and the live
///        faders call.
///
/// How it fits: the Show (Show.h) describes steps in seconds and dB; the Engine
/// (Engine.h) plays frames and linear gains. The controller turns "run the next
/// step" into exactly one StepCommand and owns the operator-facing safety
/// rules: the GO guard against double taps, refusing sounds that cannot play,
/// and clamping levels and times from the show.
///
/// Conversion rules: seconds become frames with llround(seconds * sampleRate);
/// negative and non-finite times become 0. A level becomes
/// dbToGain(min(min(levelDb, kMaxLevelDb) + file trimDb, kMaxTotalDb)); a
/// non-finite result is silence (gain 0). Step ids must be 1..kMaxStepId.
///
/// @note Every member is control-thread only. The controller reaches the audio
///       thread solely through Engine::post and Engine::snapshot.
#pragma once

#include <map>
#include <optional>
#include <vector>

#include "bigtop/Engine.h"
#include "bigtop/Show.h"
#include "bigtop/SoundSource.h"

namespace bigtop {

/// @brief Supplies the decoded audio for each file of the show.
///
/// Implemented by the io layer in the app and by a map in tests.
/// @note Returned sources must stay alive (at the same address) for as long
///       as the engine may play them; the controller fetches the pointer afresh
///       for every start and never caches it.
class SourceProvider {
public:
    /// @brief Virtual destructor so providers can be deleted through this interface.
    virtual ~SourceProvider() = default;
    /// @brief The source for one voice about to start.
    /// @param key [in] The voice the source is for (a StepId, or kPadKeyBase +
    ///        pad index); lets a streaming provider keep one source per voice.
    /// @param file [in] File to look up (AudioFile::id).
    /// @param startFrame [in] First frame the voice will play, so a streaming
    ///        provider can prefetch from there.
    /// @return The source, or nullptr when the file is missing or unreadable.
    /// @note Control thread.
    virtual SoundSource* sourceFor(VoiceKey key, FileId file, int64_t startFrame) noexcept = 0;
};

/// @brief The one seconds-to-frames rule for show times (start points, fades,
///        delays, seeks).
/// @param seconds [in] A time from the show, in seconds.
/// @param rate [in] Frames per second.
/// @return llround(seconds * rate); 0 for a negative, NaN or infinite product;
///         capped at 1e18 frames so llround stays defined.
/// @note A SourceProvider that keys data by start frame (the streaming
///       provider's heads) must use this too, so its frames match the ones
///       sourceFor() is called with. Any thread.
int64_t secondsToFrames(double seconds, double rate) noexcept;

/// @brief The timecode a snapshot's driver shows, from the snapshot alone.
/// @param snap [in] A snapshot from Engine::snapshot().
/// @param sampleRate [in] Engine sample rate, frames per second.
/// @param fps [in] Timecode frame rate; must be 24, 25 or 30.
/// @return Snapshot::timecodeStart plus the driver's position in its file, or
///         nullopt when nothing drives (timecodeKey == 0) or @p fps is unsupported.
/// @note Any thread: it uses no controller state, so the io layer can call it
///       from its timecode thread.
std::optional<Timecode> snapshotTimecode(const Snapshot& snap, double sampleRate, int fps) noexcept;

/// @brief What a GO press did.
enum class GoResult {
    Ran,           ///< @brief The step was sent to the engine; the next index advanced.
    IgnoredGuard,  ///< @brief Within Settings::goGuardSeconds of the previous accepted GO; nothing happened.
    MissingFile,   ///< @brief The step's sound cannot play (no source, empty, or start >= end) or its id is outside 1..kMaxStepId; nothing sent, index unchanged.
    EndOfList,     ///< @brief There is no next step.
    EngineBusy,    ///< @brief The engine queue was full; nothing sent, index and guard unchanged.
    Paused,        ///< @brief The show is paused (ShowController::paused()); nothing sent, index and guard unchanged. The UI should say "Paused - press Resume".
};

/// @brief Highest per-use level in dB; louder levels in the show are clamped to it.
inline constexpr double kMaxLevelDb = 12.0;
/// @brief Highest total programmed level in dB (level + file trim); larger sums
///        are clamped to it. Matches Engine.h kMaxProgrammedGain (+24 dB).
inline constexpr double kMaxTotalDb = 24.0;
/// @brief Fade length in seconds used when a second tap stops a playing pad.
inline constexpr double kPadStopSeconds = 0.05;

/// @brief Runs a Show on an Engine: GO, fade out all, pads, seek, live trim,
///        and the expected-sounds and timecode views for the UI.
/// @note Control thread only (all members). Not thread-safe.
class ShowController {
public:
    /// @brief Creates a controller with an empty show and ends any pause left
    ///        in the engine (a new show never starts paused behind the UI's back).
    /// @param engine [in] Engine to drive; must outlive the controller.
    /// @param sources [in] Provider of audio sources; must outlive the controller.
    /// @note Control thread.
    ShowController(Engine& engine, SourceProvider& sources);

    /// @brief Replaces the show; the next index becomes 0 and the GO guard is reset.
    /// @param show [in] The new show.
    /// @note Does not stop anything that is playing: stopping is the operator's
    ///       decision. Control thread.
    void setShow(Show show);

    /// @brief Replaces the show after an edit (steps added, removed or moved)
    ///        without disturbing playback.
    /// @param show [in] The edited show.
    /// @note The step that was next stays next, found by id at its new index; if
    ///       it was removed, the next index stays where it was, clamped to
    ///       0..steps.size(). The GO guard is not reset and nothing is started or
    ///       stopped. Control thread.
    void updateShow(Show show);

    /// @brief The current show.
    /// @return The show passed to the last setShow (empty initially).
    /// @note Control thread.
    const Show& show() const noexcept;

    /// @brief Index into Show::steps of the step the next GO runs.
    /// @return 0..steps.size(); steps.size() at the end of the list.
    /// @note Control thread.
    int nextIndex() const noexcept;

    /// @brief The step whose GO last set a running sound's level.
    /// @param sound [in] Id of the step that started the sound.
    /// @return The sound's own id after its start, or the id of the step whose
    ///         Level change last targeted it; 0 = unknown. Only GOs that ran
    ///         count (refused ones do not). Reset by setShow, kept by updateShow.
    /// @note Run order, not list order, so rewinding and re-running is followed.
    ///       Control thread.
    StepId levelSetter(StepId sound) const noexcept;

    /// @brief Moves the GO position (the operator picked another step).
    /// @param index [in] New next index; clamped to 0..steps.size().
    /// @note Never starts or stops anything; does not touch the guard. Control thread.
    void selectNext(int index);

    /// @brief Runs the next step (the GO button).
    /// @param nowSeconds [in] Current time in seconds from a monotonic clock;
    ///        used only for the GO guard. A time earlier than the last accepted
    ///        GO is accepted rather than blocking GO. A non-finite time (NaN,
    ///        inf) skips the guard check for this call (fail open) and is not
    ///        stored, so the guard keeps counting from the last finite GO time.
    /// @return What happened; see GoResult. Only Ran advances the next index
    ///         and restarts the guard timer. Refusals that need no audio
    ///         (end of list, paused, guard, a bad step id, a start point at or
    ///         after the step's end point) are checked before the source is
    ///         fetched, so they never open a file.
    /// @note Changes beyond kMaxChanges, and changes whose target is outside
    ///       1..kMaxStepId, are dropped. Control thread.
    GoResult go(double nowSeconds);

    /// @brief Fades every step sound out over Settings::defaultFadeSeconds;
    ///        pads keep playing.
    /// @return False when the engine queue is full (nothing sent).
    /// @note While paused it stops every step sound at once instead and ends
    ///       the pause (paused() becomes false); see PauseCommand. Does not move
    ///       the next index or reset the GO guard. Control thread.
    bool fadeOutAll();

    /// @brief Pauses every step sound (the Pause button): they fade out over
    ///        kPauseFadeSeconds and freeze, start delays included; pads play on.
    /// @return False when the engine queue is full (nothing sent, paused()
    ///         unchanged).
    /// @note While paused, go() is refused with GoResult::Paused; selectNext,
    ///       seek, live trim, fadeOutSound and pads still work. Pausing while
    ///       paused sends a command the engine ignores. Control thread.
    bool pause();

    /// @brief Resumes after pause(): step sounds fade back in over
    ///        kPauseFadeSeconds from where they froze.
    /// @return False when the engine queue is full (nothing sent, paused()
    ///         unchanged).
    /// @note Control thread.
    bool resume();

    /// @brief Whether the show is paused, from the controller's own record of
    ///        what it sent (no waiting for the engine snapshot).
    /// @return True after a successful pause() until a successful resume() or
    ///         fadeOutAll(); also true from construction when the engine queue
    ///         was full, so the end of an old pause could not be sent.
    /// @note Control thread.
    bool paused() const noexcept;

    /// @brief Fades one step sound out (the per-sound Fade button), leaving
    ///        every other sound alone; a direct command that changes nothing
    ///        in the show.
    /// @param step [in] Id of the step whose sound to fade; an unknown or
    ///        silent step is ignored by the engine; a pending one is
    ///        cancelled, and one frozen by pause() ends at once. A pad's key
    ///        (kPadKeyBase + index) with kPadStopSeconds fades that pad exactly
    ///        as a second tap does (the pad editor's Clear pad uses this).
    /// @param seconds [in] Fade length; anything shorter than kDeclickSeconds
    ///        (0, negative and non-finite included) fades over kDeclickSeconds,
    ///        so the UI's "second tap stops at once" sends kDeclickSeconds.
    /// @return False when the engine queue is full.
    /// @note Does not move the next index or touch the GO guard. Control thread.
    bool fadeOutSound(StepId step, double seconds);

    /// @brief The Keep button: folds a running step sound's live fader into
    ///        its programmed level and sets the fader back to 0 dB in the same
    ///        frame, so nothing audible changes (see KeepTrimCommand).
    /// @param step [in] Id of the step whose sound to change; unknown = ignored.
    /// @return False when the engine queue is full.
    /// @note Changes the running sound only, not the show; storing the new
    ///       level in the step is the caller's job. Control thread.
    bool keepTrim(StepId step);

    /// @brief Moves a playing step sound to a position in its file.
    /// @param step [in] Id of the step whose sound to move.
    /// @param seconds [in] Position in the file, seconds; negative/non-finite = 0.
    /// @return False when the engine queue is full.
    /// @note An unknown or silent step is ignored by the engine. Control thread.
    bool seek(StepId step, double seconds);

    /// @brief Sets the live fader of a running step sound.
    /// @param step [in] Id of the step whose sound to trim.
    /// @param db [in] Trim in dB on top of the programmed level; the engine
    ///        clamps it to +12 dB and ignores non-finite values.
    /// @return False when the engine queue is full.
    /// @note A restarted sound starts at trim 0 dB again. Control thread.
    bool setLiveTrimDb(StepId step, double db);

    /// @brief The pad button: starts the pad, or fades it out over
    ///        kPadStopSeconds if the engine snapshot shows it playing.
    /// @param index [in] Pad index, 0..padRows*padCols-1 (and < pads.size()); a pad hidden by the grid does not play.
    /// @return False for an invalid index, an empty pad, an unusable file, or a
    ///         full engine queue.
    /// @note The snapshot lags by one audio block, so two taps within one
    ///       block restart the pad instead of stopping it. Control thread.
    bool triggerPad(int index);

    /// @brief Moves a playing pad's sound to the pad's stored level (Pad::levelDb
    ///        plus its file's trim, clamped as for a start) over kDeclickSeconds,
    ///        so an edit made while the pad plays is heard without a jump.
    /// @param index [in] Pad index into Show::pads.
    /// @return True when the Level change was sent. False (nothing sent) for an
    ///         invalid index, an empty pad, a pad that is not playing or that a
    ///         second tap is fading out (a Level would keep it alive), or a full
    ///         engine queue.
    /// @note Reads the engine snapshot, which lags by one audio block. Changes
    ///       the running sound only; storing the level is the caller's job (call
    ///       this after updateShow()). Control thread.
    bool levelPad(int index);

    /// @brief Step sounds the show expects to be running just before a step,
    ///        from the step list alone (not from the engine).
    /// @param stepIndex [in] Index into Show::steps; clamped to 0..steps.size().
    /// @return Ids of the steps whose sounds should be running, oldest start first.
    /// @note Ignores sound length, so a finished non-looping sound still counts.
    ///       Also assumes every start delay has elapsed before the next GO: a
    ///       sound still waiting out its delay survives the next step's
    ///       "everything else" rule in the engine, which this list cannot know.
    ///       Control thread.
    std::vector<StepId> expectedRunningAt(int stepIndex) const;

    /// @brief The timecode of the sound that drives timecode right now.
    /// @return Its Sound::timecodeStart plus its position in the file, or
    ///         nullopt when nothing drives or Settings::timecodeFps is
    ///         unsupported (see snapshotTimecode()).
    /// @note A timecode sound still in its start delay does not drive yet: the
    ///       previous driver (or nothing) is reported until it starts to sound
    ///       (see Snapshot::timecodeKey).
    /// @note Reads the engine snapshot (state at the end of the last block). Control thread.
    std::optional<Timecode> currentTimecode() const;

private:
    const Step* findStep(StepId id) const noexcept;
    double fileTrimDb(FileId id) const noexcept;
    int64_t toFrames(double seconds) const noexcept;
    float toGain(double levelDb, FileId file) const noexcept;
    bool makeStart(StartSpec& out, VoiceKey key, const Sound& sound);

    Engine& engine_;
    SourceProvider& sources_;
    Show show_;
    int next_ = 0;
    std::map<StepId, StepId> levelSetter_;  // sound -> step whose GO last set its level
    std::optional<double> lastGo_;  // time of the last accepted GO, for the guard
    bool paused_ = false;           // set by pause() (or a failed resume at construction), cleared by resume() and fadeOutAll()
};

}  // namespace bigtop
