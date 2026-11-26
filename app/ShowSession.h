/// @file ShowSession.h
/// @brief Everything one running app has open: the engine, the streaming
///        provider, the controller, the show folder and the audio output.
///
/// The UI (start panel, status bar, GO screen) talks only to this class. It
/// keeps the rules that tie the io pieces together: the provider learns every
/// new show version (open and every edit), every edit is saved at once, the
/// next step and the mode go to state.json, another show is only opened
/// once nothing plays any more, and the mode decides which actions are
/// allowed (Mode.h is the one table). Dropped files
/// are hashed on one background thread and converted on another (one file
/// at a time).
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_core/juce_core.h>

#include "bigtop/Engine.h"
#include "bigtop/Show.h"
#include "bigtop/ShowController.h"
#include "bigtop/io/AppSettings.h"
#include "bigtop/io/ArtNetSender.h"
#include "bigtop/io/AudioOutput.h"
#include "bigtop/io/Importer.h"
#include "bigtop/io/ShowArchive.h"
#include "bigtop/io/ShowFolder.h"
#include "bigtop/io/StreamingSource.h"
#include "bigtop/io/Waveform.h"
#include "Mode.h"
#include "UiClock.h"

namespace bigtop::app {

/// @brief The open show and the audio path that plays it.
/// @note Message thread only (all members). Not copyable.
class ShowSession {
public:
    /// @brief What the status bar shows; see status().
    struct Status {
        bool audioOk = false;     ///< @brief The chosen output device is open and playing.
        juce::String audioText;   ///< @brief "Audio OK", "Audio: device stalled" (open but no audio for >= 1 s), "Audio: device lost - retrying" (an em dash on screen) or "Audio: no device".
        bool clip = false;        ///< @brief The output limiter reduced gain by more than kClipThresholdDb since the last resetClip().
        uint32_t dropouts = 0;    ///< @brief Late disk reads of the open show (see StreamingSourceProvider::dropouts()).
        bool saveOk = true;       ///< @brief False after a failed save until the next one succeeds.
        juce::String saveText;    ///< @brief "Saved hh:mm:ss", the save error, or empty before the first save.
        juce::String showName;    ///< @brief The show folder's name; empty when no show is open.
        juce::String message;     ///< @brief A short notice from flash(), empty once kFlashMs has passed.
        uint32_t droppedStarts = 0;  ///< @brief Starts and seeks refused since launch because all voices were busy (Snapshot::droppedStarts); the bar says "a start or seek was dropped".
    };

    /// @brief CLIP lights when the output limiter turns the level down by more than
    ///        this, in dB: where limiting starts to be heard. A master peaking at
    ///        0 dBFS meets the -0.3 dBFS ceiling with 0.3 dB and stays dark.
    static constexpr float kClipThresholdDb = 1.0f;

    /// @brief The notice when Show mode is refused because the sounds are paused (UTF-8).
    static constexpr const char* kShowRefusedPaused = "Resume or press Esc first \xe2\x80\x93 Show mode has no Pause";

    /// @brief How long a flash() message stays in the status bar, in ms.
    static constexpr juce::uint32 kFlashMs = 3000;

    /// @brief Stand-ins that keep a session off the machine: used only by the screenshot mode and the app tests.
    struct Offline {
        std::vector<io::NetAdapter> adapters;  ///< @brief The adapter list shown instead of the real one (invented entries).
        juce::String savedTime;                ///< @brief Non-empty: the status bar says "Saved <this>" instead of the clock, so re-rendered images do not change.
        std::function<io::OutputPeaks()> outputPeaks;  ///< @brief Set: read instead of the output's peaks (as gainReductionDb).
        std::function<float()> gainReductionDb;  ///< @brief Set: read instead of the output limiter's gain reduction (the tests drive the engine directly, so the limiter never sees audio).
    };

    /// @brief Opens the audio device and starts the disk-read thread; no show yet.
    /// @param settings App settings: the stored device is opened from them, and
    ///        the last show and device choice are written back. Must outlive this.
    /// @param devices The app's device manager; must outlive this.
    /// @note The device stored in @p settings becomes the desired one (see
    ///       io::AudioOutput): if it is missing now it is retried, and the
    ///       status bar says so, rather than silently playing elsewhere.
    ///       With nothing stored (first launch) the device JUCE opens becomes
    ///       the desired one and is stored.
    /// @param offline Null in the app. Screenshot/testing hook: when set, no audio device is opened, Art-Net
    ///        goes to a socket that discards everything, and adapters() returns offline->adapters, so
    ///        nothing of the machine is read or sent. Copied; need not outlive the call.
    ShowSession(io::AppSettings& settings, juce::AudioDeviceManager& devices, const Offline* offline = nullptr);

    /// @brief The network adapters to offer for Art-Net.
    /// @return io::listAdapters(), or the invented list of an Offline session.
    std::vector<io::NetAdapter> adapters() const;

    /// @brief Waits for a running hash and a running conversion to finish
    ///        (queued ones are dropped: their steps stay red, as after a
    ///        crash), stops audio, then frees the show.
    /// @warning Quit waits for the running conversion however long it takes
    ///          (a long file: seconds; a hung decoder would hang quit). A
    ///          finite timeout is not safe: the job would outlive the session.
    ~ShowSession();

    ShowSession(const ShowSession&) = delete;
    ShowSession& operator=(const ShowSession&) = delete;

    /// @brief Opens a show folder, with next step and mode from state.json.
    /// @param dir The show folder.
    /// @param error Receives a human-readable reason on failure.
    /// @param backupsOnFailure If not null, receives the loadable backups
    ///        (newest first) when show.json itself failed to load.
    /// @return True when the show is open; nothing plays. On failure the
    ///         previously open show (if any) stays open.
    /// @note Every audio file whose FLAC is missing while its kept original
    ///       (AudioFile::sourcePath) exists is converted again at once (see
    ///       io::filesToRebuild()): its rows show "importing..." until done.
    /// @note Refused while any sound of the current show may still be read
    ///       (design 8.4), including about 1 s after the last one ended.
    bool open(const juce::File& dir, std::string& error, std::vector<juce::File>* backupsOnFailure);

    /// @brief Creates an empty show in @p dir and opens it.
    /// @param dir Folder that does not exist yet or is empty.
    /// @param error Receives a human-readable reason on failure.
    /// @return True when the new show is open. Refused while sounds play, like open().
    bool createNew(const juce::File& dir, std::string& error);

    /// @brief Makes a backup the folder's show.json and opens it.
    /// @param dir The show folder whose show.json failed.
    /// @param backup One of the backups open() returned.
    /// @param error Receives a human-readable reason on failure.
    /// @return True when the show is open. Refused while sounds play, like open().
    bool restoreBackupAndOpen(const juce::File& dir, const juce::File& backup, std::string& error);

    /// @brief The open show's folder.
    /// @return The folder, or an empty File while no show is open.
    juce::File showDir() const;

    /// @brief Whether a show is open.
    /// @return True after a successful open(), createNew() or restoreBackupAndOpen().
    bool isOpen() const noexcept;

    /// @brief The open show's controller (GO, fade out, selection, faders).
    /// @return The controller.
    /// @warning Only while isOpen().
    ShowController& controller();

    /// @brief Takes an edited show: updates the controller and the provider,
    ///        then saves show.json and state.json.
    /// @param edited The whole show after the edit.
    /// @note A failed save keeps the show in memory and shows a red error;
    ///       the next commit saves everything again. Only while isOpen().
    void commit(Show edited);

    /// @brief One edit of the show: checks the mode, applies @p change to a copy, commits it.
    /// @param action The permission the edit needs (EditValue for levels and times,
    ///        EditStructure / ShowSettings for the rest); when the current mode
    ///        forbids it, allowed() flashes the notice and nothing changes.
    /// @param change Mutates the copy of the show; returns false to drop the edit
    ///        (nothing is committed or saved). Runs only when allowed.
    /// @return True when the edit was committed; false when refused, dropped or no show is open.
    /// @note Running sounds are never touched: the controller keeps what they
    ///       started with. Message thread.
    bool edit(Action action, const std::function<bool(Show&)>& change);

    /// @brief The Keep button: stores a running sound's live fader in its step's
    ///        level (or into the Level change that set the level heard, see keepIntoShow())
    ///        and sets the fader back to 0 dB, with no audible change.
    /// @param id The step whose running sound to keep.
    /// @return False when refused (mode forbids it, the step or its sound is
    ///         gone, the sound is not running, the fader is at its -inf stop, or the
    ///         engine queue is full: "Engine busy"); then nothing changed.
    /// @note The fader value is the engine's trim (snapshot), so it is exactly
    ///       what is folded. New level = keptLevelDb(level, trim dB), committed
    ///       through edit(Action::KeepFader); the engine folds the same trim into
    ///       the running sound in one frame (ShowController::keepTrim). The
    ///       engine command goes first: if it is refused nothing was stored, so
    ///       the level can never be counted twice. Message thread.
    bool keepFader(StepId id);

    /// @brief The session's engine, so the screenshot mode can render audio blocks itself.
    /// @return The engine.
    /// @warning Screenshot/testing hook (Screenshots.cpp). Only valid to call Engine::process() on it
    ///          while no audio device is open: process() is the audio thread's call.
    Engine& engineForScreenshots() noexcept { return *engine_; }

    /// @brief The Art-Net timecode sender (one per session, thread already running).
    /// @return The sender; its status() feeds the status bar light.
    /// @note Configured from the show's target and frame rate and the app
    ///       settings' adapter on open and after every commit that changes them.
    io::ArtNetSender& sender() noexcept { return *sender_; }

    /// @brief Chooses the network adapter Art-Net goes out of and applies it at once.
    /// @param address Adapter IPv4 address, or "" for Default. Stored in the app
    ///        settings (machine-specific), not in the show.
    void setArtNetAdapter(const juce::String& address);

    /// @brief The stored adapter choice.
    /// @return Adapter address, or "" for Default.
    juce::String artNetAdapter() const { return settings_.artNetAdapter(); }

    /// @brief Saves state.json (next index, mode); call after the next
    ///        step changed (GO, selection). Only while isOpen().
    void stateChanged();

    /// @brief The current mode.
    /// @return The mode of the open show; Edit while no show is open.
    ShowMode mode() const noexcept;

    /// @brief Switches the mode and saves state.json.
    /// @param mode The new mode. Ignored (false) while no show is open.
    /// @return False when refused: entering Show while the show is paused
    ///         (by the controller's record or the engine snapshot) flashes kShowRefusedPaused and changes nothing, so Show mode can
    ///         never be stuck paused with its Resume button hidden.
    /// @note Dropped files that were hashed while drops were not allowed are
    ///       added (in drop order) as soon as the new mode allows drops.
    bool setMode(ShowMode mode);

    /// @brief Asks the permission table whether an action may happen now.
    /// @param action What the operator tries.
    /// @return True when the current mode allows it. When not, a notice that
    ///         names the mode is flashed, so a refused tap is never silent.
    ///         False without a notice while no show is open.
    bool allowed(Action action);

    /// @brief Adds dropped files as steps (Edit mode only).
    /// @param paths Full paths of the dropped files, in drop order.
    /// @param index Step list position of the first new step (clamped).
    /// @note Message thread. Not WAV/MP3/FLAC (or not a file): refused with a
    ///       notice (a folder with its own wording). The rest are hashed on
    ///       the hash thread; then one step per
    ///       file (named by the file without extension) is inserted and
    ///       committed at once. Content the show already has reuses its file;
    ///       new content gets a file entry and is converted on the import
    ///       thread, and its steps show "importing..." until the provider has
    ///       loaded it. A file entry whose audio is missing (an import that
    ///       failed or was cut by a crash) is imported again from this drop.
    ///       Ignored with a notice outside Edit mode or with no show. A drop whose
    ///       hashes arrive after the mode left Edit waits, with a notice
    ///       (\"1 dropped file waits\"), until Edit mode returns: only Edit changes the show.
    void importFiles(const juce::StringArray& paths, int index);

    /// @brief Assigns a dropped audio file to a pad (Edit mode only).
    /// @param file The dropped file (WAV, MP3 or FLAC); anything else is refused with a notice.
    /// @param index Pad index, 0..padRows*padCols-1 (a pad hidden by the grid is refused).
    /// @note Message thread. Same path as importFiles(): hashed on the hash thread,
    ///       the show's existing file is reused for identical content (same dedup and
    ///       importVersion), new content is converted on the import thread and the pad
    ///       shows "importing..." until the provider has loaded it. The pad's name
    ///       becomes the file name without extension; its level stays. Pads missing
    ///       from the list (a short pad vector) are created. Unlike step drops, a
    ///       drop whose hash arrives after Edit mode ended is dropped with a notice
    ///       (nothing waits), because a pad cannot be "inserted later".
    void importToPad(const juce::File& file, int index);

    /// @brief Deletes a step (Edit mode only); its audio file stays.
    /// @param index Index into Show::steps; out of range does nothing.
    /// @note A playing sound of the step plays on (design 8.3). Ignored with
    ///       a notice outside Edit mode. Message thread.
    void deleteStep(int index);

    /// @brief Moves a step (Edit mode only).
    /// @param from Index of the step to move; out of range does nothing.
    /// @param to The index it has afterwards (clamped).
    /// @note Ignored with a notice outside Edit mode. Message thread.
    void moveStep(int from, int to);

    /// @brief Gives a pad a step's sound (a step dragged onto the pad).
    /// @param step The step whose file, name, start and end the pad takes; the pad keeps its level.
    /// @param pad The pad's index.
    /// @return True if the pad changed. Refused with a notice outside Edit mode (Action::AssignPad)
    ///         and for a step without a sound (a fade step).
    bool assignStepToPad(StepId step, int pad);

    /// @brief Inserts a fade step (a step without a sound) right after NEXT.
    /// @return The new step's id; nullopt when the mode forbids structure edits
    ///         (a notice is flashed), no show is open, or step ids ran out.
    /// @note Named "Fade", with makeStep()'s default for a step without a sound
    ///       ("everything else: keep"). NEXT stays on its step. Message thread.
    std::optional<StepId> addFadeStep();

    /// @brief Whether a file's import is still running or queued.
    /// @param file AudioFile::id.
    /// @return True from the start of its conversion (queued included)
    ///         until the converted file is loaded.
    bool importing(FileId file) const;

    /// @brief Whether any import or re-conversion is queued or running.
    /// @return True while importing(file) is true for some file.
    bool importsRunning() const { return !importing_.empty(); }

    /// @brief Why a file's import failed in this session.
    /// @param file AudioFile::id.
    /// @return The error, or empty (none, or failed before this launch).
    juce::String importError(FileId file) const;

    /// @brief A file's waveform summary, for the running rows' position bars.
    /// @param file AudioFile::id.
    /// @return The summary, or null until it is read (or when the file cannot be read).
    /// @note Read on the waveform thread once per file after a show opens and after a file's
    ///       import finishes; handed over here on the message thread. Another show drops them all.
    std::shared_ptr<const io::Peaks> peaks(FileId file) const;

    /// @brief The output peaks since the last call, for the level meter.
    /// @return Per channel, the highest sample after the limiter (linear).
    io::OutputPeaks outputPeaks();

    /// @brief Turns the CLIP light off (a tap on it). If the limiter is still
    ///        working, the next tick() lights it again.
    void resetClip() noexcept { clipLatched_ = false; }

    /// @brief The 4 Hz housekeeping: frees unused sources, samples the CLIP
    ///        light, watches the engine's frame count for a stalled device
    ///        (status text only), and every 2 s retries a lost output device.
    void tick();

    /// @brief The status bar's content.
    /// @return The current status.
    Status status() const;

    /// @brief What the engine plays right now (the GO screen's 30 Hz view).
    /// @return Engine::snapshot(); look voices up by key, their order is not stable.
    Snapshot snapshot() const noexcept;

    /// @brief Shows a short notice in the status bar for kFlashMs (no dialog:
    ///        nothing may block the operator during a show).
    /// @param text The notice; replaces any notice still showing.
    void flash(const juce::String& text);

    /// @brief The open show's audio provider (to show unplayable steps red).
    /// @return The provider.
    /// @warning Only while isOpen().
    const io::StreamingSourceProvider& sources() const;

    /// @brief Tells the session that the device settings window opened or closed.
    /// @param open True while the window is open: the device retry pauses so
    ///        the user's new pick is never taken for a fallback and closed.
    ///        On false, if the user picked a device while the window was open
    ///        (JUCE's explicit-choice state XML changed), the device the
    ///        manager has open now becomes the user's choice: the desired
    ///        setup (none if "no device" was picked), stored in the app
    ///        settings. No pick, or a pick that failed to open, keeps the
    ///        previous choice, so a default JUCE opened after a device loss
    ///        (a muted fallback) is never adopted.
    /// @warning Relies on JUCE changing createStateXml() only for picks made in
    ///          the selector, not for its own fallback after a loss; checked by
    ///          the rehearsal checklist ("Unplug while Settings is open").
    /// @note Repeated calls with the same value do nothing.
    void setDeviceSettingsOpen(bool open);

    /// @brief Whether another show may be opened now (design 8.4).
    /// @param error Receives the refusal notice when not.
    /// @return True when no show is open or nothing of it can play any more
    ///         (about 1 s after the last sound ended). Frees unused sources.
    bool quiet(std::string& error);

private:
    friend struct AppTestAccess;  // the app tests drop the waveforms to compare a bar with and without
    void adoptDevice();
    void configureSender();
    std::unique_ptr<juce::XmlElement> stateXml() const;
    void saved(io::SaveResult result);
    void hashed(const std::vector<juce::File>& files, const std::vector<std::string>& hashes, int index);
    void padHashed(const juce::File& file, const std::string& hash, int index);
    void convert(const juce::File& source, FileId file, const juce::File& target, const std::string& keepAs);
    void keepOnly(const juce::File& source, const std::string& keepAs);
    void converted(FileId file, const io::ImportResult& result);
    void readPeaksOf(const AudioFile& file);

    io::AppSettings& settings_;
    juce::AudioDeviceManager& devices_;
    // Owned in this order (design): a show's provider and controller are
    // replaced together on open; output_ is destroyed first so no audio
    // callback can read a source while the rest goes.
    std::unique_ptr<Engine> engine_;
    // Declared right after the engine and destroyed before it: its thread reads engine snapshots.
    std::unique_ptr<io::ArtNetSender> sender_;
    juce::String fixedSavedTime_;  // Offline only: a fixed "Saved" time for repeatable screenshots
    std::optional<std::vector<io::NetAdapter>> fakeAdapters_;  // set only for an Offline session
    juce::String senderConfig_;  // last configure() arguments, so an unrelated edit does not rebind the socket
    juce::TimeSliceThread readThread_{"Bigtop disk reads"};
    std::unique_ptr<io::StreamingSourceProvider> provider_;
    std::unique_ptr<ShowController> controller_;
    std::unique_ptr<io::ShowFolder> folder_;  // null while no show is open
    std::unique_ptr<io::AudioOutput> output_;

    ShowMode mode_ = ShowMode::Edit;
    bool hasDesired_ = false;     // a device is chosen, so "not running" means lost
    bool settingsOpen_ = false;
    // JUCE's explicit-choice XML when the settings window opened (empty = none);
    // closing it with the same XML adopts nothing.
    juce::String choiceAtOpen_;
    int ticks_ = 0;
    // Last framesProcessed seen by tick() and when it last moved, so a device
    // that runs but delivers no callbacks shows as stalled.
    int64_t lastFrames_ = 0;
    juce::uint32 lastAdvanceMs_ = uiMs();
    bool clipLatched_ = false;  // CLIP is lit: set by tick(), cleared by resetClip()
    std::function<float()> fakeGainReductionDb_;  // Offline only, see Offline::gainReductionDb
    std::function<io::OutputPeaks()> fakeOutputPeaks_;  // Offline only, see Offline::outputPeaks
    bool saveOk_ = true;
    juce::String saveText_;
    juce::String flashText_;
    juce::uint32 flashMs_ = 0;  // uiMs() when flashText_ was set

    // Import results arrive by callAsync; they act only while this token is the
    // one they captured. It is replaced when another show opens and dies with
    // the session, so a late result for an old show (or after quit) does nothing.
    std::shared_ptr<int> importToken_ = std::make_shared<int>(0);
    std::set<FileId> importing_;                 // queued or converting, for the current show
    std::map<FileId, std::string> importErrors_;  // per file, so every step using it shows it
    // Hashed drops that arrived in Show mode, applied in order when it ends.
    struct PendingDrop {
        std::vector<juce::File> files;
        std::vector<std::string> hashes;
        int index = 0;
    };
    std::vector<PendingDrop> pendingDrops_;
    // Waveform summaries of the open show. The cancel flag belongs to one show: open()
    // sets it (the old show's reads stop within one block) and makes a new one.
    std::map<FileId, std::shared_ptr<const io::Peaks>> peaks_;
    std::shared_ptr<std::atomic<bool>> peaksCancel_ = std::make_shared<std::atomic<bool>>(false);
    // Last members: destroyed first, but ~ShowSession already joined them.
    // Hashing has its own thread so a drop's steps appear within seconds even
    // while earlier conversions run.
    juce::ThreadPool hashPool_{juce::ThreadPoolOptions{}.withThreadName("Bigtop hash").withNumberOfThreads(1)};
    juce::ThreadPool importPool_{juce::ThreadPoolOptions{}.withThreadName("Bigtop import").withNumberOfThreads(1)};
    // Waveform reads get their own low-priority thread: a show's worth of files must never
    // delay an import, and the disk-read thread (tails) and GO never wait for it.
    juce::ThreadPool peakPool_{juce::ThreadPoolOptions{}
                                   .withThreadName("Bigtop waveforms")
                                   .withNumberOfThreads(1)
                                   .withDesiredThreadPriority(juce::Thread::Priority::low)};
};

}  // namespace bigtop::app
