/// @file ShowFolder.h
/// @brief A show lives in a folder: show.json (the show), state.json (where
///        the owner left off), audio/ (imported files) and backups/. This
///        class makes saving safe: a save that cannot be read back never
///        replaces the good file, and timed backups are kept.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include <juce_core/juce_core.h>

#include "bigtop/Show.h"

namespace bigtop::io {

/// @brief The app's three modes (design 4); the permission table lives in
///        app/Mode.h, io only stores the choice.
enum class ShowMode {
    Edit,       ///< @brief Everything may change.
    Rehearsal,  ///< @brief Only levels and times may change.
    Show        ///< @brief Nothing changes; hold 2 s to leave.
};

/// @brief Where the owner left off; stored in state.json, outside the show
///        so that moving through the show never rewrites show.json.
struct ShowState {
    int nextIndex = 0;      ///< @brief Index of the step the next GO plays.
    ShowMode mode = ShowMode::Edit;  ///< @brief The mode the app was in; Edit when unknown.
};

/// @brief Outcome of a save.
enum class SaveResult {
    Saved,           ///< @brief The file on disk is the new content.
    ReadBackFailed,  ///< @brief The text could not be parsed back; the old file is untouched.
    WriteFailed      ///< @brief The disk refused the write; the old file is untouched.
};

/// @brief Outcome of ShowFolder::open().
struct OpenResult {
    std::optional<Show> show;                 ///< @brief Set when show.json loaded.
    std::string error;                        ///< @brief Human-readable reason when it did not.
    std::vector<juce::File> loadableBackups;  ///< @brief Newest first; filled only when show.json failed.
};

/// @brief One show folder on disk.
/// @note All calls are blocking file I/O: call them from the message thread
///       or a worker, never from the audio thread.
class ShowFolder {
public:
    /// @brief Wraps a folder; touches nothing on disk.
    /// @param dir The show folder (it need not exist yet).
    explicit ShowFolder(juce::File dir);

    /// @brief The show folder.
    /// @return The folder given at construction.
    const juce::File& dir() const noexcept;

    /// @brief Folder holding the imported audio files.
    /// @return dir/audio (may not exist for a folder never created by createNew).
    juce::File audioDir() const;

    /// @brief Starts a new, empty show in a folder.
    /// @param dir Folder that does not exist yet or is empty.
    /// @param error Receives a human-readable reason on failure.
    /// @return True when audio/, backups/ and an empty show.json now exist.
    static bool createNew(const juce::File& dir, std::string& error);

    /// @brief Loads show.json. Removes leftover *.tmp files of a crashed
    ///        save first; on success takes the on-open backup if due.
    /// @param now Current time (injected so tests control the clock).
    /// @return The show, or an error plus the backups that would load.
    /// @note Never modifies show.json; the owner decides about backups.
    OpenResult open(juce::Time now);

    /// @brief Saves the show: serialise, parse the text back, write
    ///        show.json.tmp, flush, then swap it over show.json.
    /// @param show Show to save.
    /// @param now Current time, used for the backup name and schedule.
    /// @return Saved, or why the old file was kept.
    /// @note A backup is taken afterwards when the newest is 5+ minutes old.
    SaveResult save(const Show& show, juce::Time now);

    /// @brief Saves state.json by the same tmp-and-swap path, without backup.
    /// @param state State to store.
    /// @return Saved or WriteFailed.
    SaveResult saveState(const ShowState& state);

    /// @brief Reads state.json.
    /// @return The state, or defaults when the file is missing or unreadable.
    ShowState loadState() const;

    /// @brief Makes a backup the current show.
    /// @param backup A file from OpenResult::loadableBackups.
    /// @return True when show.json now holds the backup's content.
    bool restoreBackup(const juce::File& backup);

private:
    juce::File dir_;
    void backupIfDue(juce::Time now);
};

}  // namespace bigtop::io
