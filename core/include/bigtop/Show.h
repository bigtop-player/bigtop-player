/// @file Show.h
/// @brief The plain data model of a show: what the engine plays and what the
///        JSON file (ShowJson.h) stores. No behaviour lives here, only data.
///
/// The step model: a show is an ordered list of steps. One GO runs exactly one
/// step. A step may start a sound and/or change sounds that are already running
/// (set a level, fade out), and says what happens to all the other sounds
/// ("Others"). A running sound is identified by the id of the step that started
/// it, which is why step ids must be unique and never reused. Changes refer to
/// their targets by that id.
///
/// All structs are plain aggregates with default member values and a defaulted
/// operator==, so a show can be compared and round-tripped through JSON in
/// tests.
///
/// @note Show data is edited and loaded on the control thread; the audio thread
///       never touches these types directly.
#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace bigtop {

/// @brief Identifies a step within a show.
///
/// Never 0 (0 means "no step"), unique within a show, and never reused after a
/// step is deleted, so a Change can't silently start pointing at a different
/// step.
using StepId = uint32_t;

/// @brief Highest valid step id: step ids are 1..kMaxStepId.
///
/// The upper half of the 32-bit range is reserved for the engine's pad voice
/// keys (Engine.h kPadKeyBase), so a step can never pose as a pad and escape
/// fade out all. fromJson() rejects larger step ids and change targets.
inline constexpr StepId kMaxStepId = 0x7FFFFFFFu;

/// @brief Identifies an AudioFile entry in Show::files.
using FileId = uint32_t;

/// @brief A SMPTE-style timecode position.
///
/// Bigtop sends timecode, it never receives it: Sound::timecodeStart is the
/// value sent when a timecode sound is at its start point. The frame rate is
/// Settings::timecodeFps.
struct Timecode {
    int h = 0;  ///< @brief Hours.
    int m = 0;  ///< @brief Minutes.
    int s = 0;  ///< @brief Seconds.
    int f = 0;  ///< @brief Frames within the second.
    /// @brief Member-wise equality.
    /// @param other Timecode to compare with.
    /// @return True if all fields are equal.
    bool operator==(const Timecode& other) const = default;
};

/// @brief One audio file used by the show.
struct AudioFile {
    FileId id = 0;     ///< @brief Referenced by Sound::file and Pad::file.
    std::string path;  ///< @brief Relative to the show folder, so the folder can move.
    /// @brief Permanent per-file correction in dB (e.g. to even out loud/quiet files).
    ///
    /// Applied always, underneath any Sound::levelDb, which is the per-use level.
    double trimDb = 0.0;
    /// @brief SHA-256 (hex) of the imported original file, so a re-import can
    ///        be recognised; empty = unknown.
    std::string sourceHash{};  // explicit {} lets brace initializers omit it without a GCC/Clang warning
    /// @brief Version of the import pipeline that made the file (missing in JSON = 1).
    ///
    /// A re-import of the same original is only merged with an entry whose
    /// version is current, so an improved importer replaces older conversions.
    int importVersion = 1;
    /// @brief The original dropped file kept in the show folder, relative to it
    ///        (e.g. `sources/<sha256>.mp3`); empty = none kept (files imported before it existed).
    ///
    /// Lets an export carry the smallest lossless form and lets the app
    /// rebuild a missing FLAC. Optional in JSON (omitted when empty).
    std::string sourcePath{};
    /// @brief Member-wise equality.
    /// @param other File to compare with.
    /// @return True if all fields are equal.
    bool operator==(const AudioFile& other) const = default;
};

/// @brief What a Change (or Others) does to a running sound.
enum class ChangeKind {
    Keep,     ///< @brief Leave the sound exactly as it is.
    Level,    ///< @brief Move it to a new level (Change::levelDb) over Change::seconds.
    FadeOut,  ///< @brief Fade it to silence over the given seconds, then stop it.
};

/// @brief A change applied to a running sound when its step is GO'd.
struct Change {
    StepId target = 0;                   ///< @brief Id of the step that started the sound.
    ChangeKind kind = ChangeKind::Keep;  ///< @brief What to do to the sound.
    double levelDb = 0.0;                ///< @brief New level in dB; only used for Level.
    double seconds = 0.0;                ///< @brief Duration of the level move/fade, in seconds.
    /// @brief Member-wise equality.
    /// @param other Change to compare with.
    /// @return True if all fields are equal.
    bool operator==(const Change& other) const = default;
};

/// @brief What happens to every running sound that is not named by a Change and
///        is not the sound this step starts.
///
/// Typically a new song fades the previous one out.
struct Others {
    ChangeKind kind = ChangeKind::Keep;  ///< @brief Only Keep or FadeOut are valid (not Level).
    double seconds = 0.0;                ///< @brief Fade duration in seconds (FadeOut only).
    /// @brief Member-wise equality.
    /// @param other Others to compare with.
    /// @return True if all fields are equal.
    bool operator==(const Others& other) const = default;
};

/// @brief A sound that a step starts.
struct Sound {
    FileId file = 0;  ///< @brief Which AudioFile to play.
    /// @brief Per-use level in dB (0 = unity), on top of the file's trimDb.
    double levelDb = 0.0;
    double fadeInSeconds = 0.0;  ///< @brief Fade-in length in seconds; 0 = start at full level immediately.
    double startSeconds = 0.0;   ///< @brief Position in the file to start from, in seconds.
    std::optional<double> endSeconds{};  ///< @brief Position to stop at, in seconds; unset = play to the end.
    bool loop = false;                 ///< @brief Repeat start..end until stopped.
    double startDelaySeconds = 0.0;    ///< @brief Wait this long (seconds) after GO before starting.
    /// @brief If set, this sound drives the Art-Net timecode sent to lighting.
    ///
    /// The sound still starts at GO like any other; while it plays, the
    /// timecode sent is this start value plus the position in the file.
    std::optional<Timecode> timecodeStart{};
    /// @brief Name of an OnTime cue to trigger when the sound starts; empty = none.
    ///
    /// The explicit `{}` on this and the other optional members lets designated
    /// initializers such as `Sound{.file = 1}` omit them; GCC and Clang warn otherwise.
    std::string ontimeCue{};
    /// @brief Member-wise equality.
    /// @param other Sound to compare with.
    /// @return True if all fields are equal.
    bool operator==(const Sound& other) const = default;
};

/// @brief One line of the show: what a single GO does.
struct Step {
    StepId id = 0;     ///< @brief Unique, nonzero, never reused (see StepId).
    std::string name;  ///< @brief Name shown in the step list.
    std::string note;  ///< @brief Free-text note for the operator.
    std::optional<Sound> sound;   ///< @brief Sound to start; unset = this step only changes things.
    std::vector<Change> changes;  ///< @brief Changes to specific running sounds.
    Others others;                ///< @brief What happens to all remaining running sounds.
    /// @brief Member-wise equality.
    /// @param other Step to compare with.
    /// @return True if all fields are equal.
    bool operator==(const Step& other) const = default;
};

/// @brief A one-shot sound button (sound effects, fanfares) played outside the
///        step list.
struct Pad {
    std::string name;            ///< @brief Button label; empty = unused pad.
    std::optional<FileId> file;  ///< @brief File to play; unset = nothing assigned.
    double levelDb = 0.0;        ///< @brief Level in dB (0 = unity).
    double startSeconds = 0.0;   ///< @brief Where a tap starts playing, in seconds from the file's start.
    std::optional<double> endSeconds{};  ///< @brief Where it stops, in seconds; unset = the file's end.
    /// @brief Member-wise equality.
    /// @param other Pad to compare with.
    /// @return True if all fields are equal.
    bool operator==(const Pad& other) const = default;
};

/// @brief Show-wide settings.
struct Settings {
    /// @brief Fade length in seconds used by makeStep() for "Others" when a
    ///        sound starts, and by "fade out all" (Esc).
    double defaultFadeSeconds = 2.0;
    /// @brief Minimum time in seconds between two accepted GO presses, so a
    ///        double tap on the button doesn't skip a step.
    double goGuardSeconds = 0.3;
    /// @brief True: Pause (button and P) is also available in Show mode, and
    ///        Show mode may be entered while paused. False (the default, and
    ///        what an older show file without the key gets): Show mode has no Pause.
    bool pauseInShow = false;
    /// @brief Frame rate of the timecode Bigtop sends, in frames per second.
    ///
    /// @warning Allowed values are 24, 25 or 30 (fromJson() rejects anything
    ///          else); code divides by this value.
    int timecodeFps = 25;
    double loudnessTargetLufs = -16.0;  ///< @brief Target loudness for normalisation, in LUFS.
    /// @brief Art-Net destination address (broadcast by default).
    std::string artnetTarget = "255.255.255.255";
    std::string ontimeHost;  ///< @brief OnTime server host; empty = integration off.
    int ontimePort = 4001;   ///< @brief OnTime server port.
    /// @brief Visible pad grid rows; kMinPadRows..kMaxPadRows, and padRows * padCols <= kMaxPads.
    int padRows = 4;
    int padCols = 2;  ///< @brief Visible pad grid columns; kMinPadCols..kMaxPadCols.
    /// @brief Member-wise equality.
    /// @param other Settings to compare with.
    /// @return True if all fields are equal.
    bool operator==(const Settings& other) const = default;
};

/// @brief Version written to and required in the JSON file (see ShowJson.h).
inline constexpr int kFormatVersion = 1;

/// @brief Most pads a show can hold.
inline constexpr int kMaxPads = 24;
inline constexpr int kMinPadRows = 1;  ///< @brief Fewest pad grid rows.
inline constexpr int kMaxPadRows = 6;  ///< @brief Most pad grid rows.
inline constexpr int kMinPadCols = 1;  ///< @brief Fewest pad grid columns.
inline constexpr int kMaxPadCols = 6;  ///< @brief Most pad grid columns.

/// @brief The whole show.
struct Show {
    int formatVersion = kFormatVersion;  ///< @brief File format version; see kFormatVersion.
    Settings settings;                   ///< @brief Show-wide settings.
    std::vector<AudioFile> files;        ///< @brief Every audio file the show uses.
    std::vector<Step> steps;             ///< @brief Played in order, one per GO.
    /// @brief The one-shot pad buttons: 8 empty ones by default, at most kMaxPads.
    ///
    /// May be longer than padRows * padCols: pads hidden by shrinking the grid
    /// keep their data but do not play (ShowController::triggerPad).
    std::vector<Pad> pads = std::vector<Pad>(8);
    /// @brief Highest step id ever handed out, so ids stay unique and unused
    ///        even after the newest step is deleted; 0 = none yet.
    StepId lastStepId = 0;
    /// @brief Member-wise equality.
    /// @param other Show to compare with.
    /// @return True if all fields are equal.
    bool operator==(const Show& other) const = default;
};

/// @brief Builds a step with the default "Others" behaviour.
///
/// If the step starts a sound, everything else fades out over
/// settings.defaultFadeSeconds; if it starts nothing, everything else is kept.
/// @param id Id for the new step; must be nonzero and unique within the show.
/// @param settings Show settings; supplies defaultFadeSeconds (seconds).
/// @param sound Sound the step starts, or empty if the step only changes things.
/// @return The new step.
/// @note Control thread only.
Step makeStep(StepId id, const Settings& settings, std::optional<Sound> sound);

}  // namespace bigtop
