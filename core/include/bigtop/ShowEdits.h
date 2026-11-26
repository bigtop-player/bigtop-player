/// @file ShowEdits.h
/// @brief Plain edit operations on the Show model: what the app calls when the
///        operator drops files in, deletes a step or drags one to a new place.
///
/// They only change the model; swapping an edited show into the running
/// controller is ShowController::updateShow(). Step ids are never reused (a
/// deleted step's sound may still be playing under its id), which is why
/// allocateStepId() looks at Show::lastStepId as well as the existing steps.
///
/// @note Control thread only; none of these is thread-safe.
#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "bigtop/Show.h"

namespace bigtop {

inline constexpr double kEditLevelMinDb = -60.0;  ///< @brief Lowest level an editor field accepts, dB.
inline constexpr double kEditLevelMaxDb = 12.0;   ///< @brief Highest level an editor field accepts, dB.
inline constexpr double kEditMaxPositionSeconds = 24.0 * 3600.0;  ///< @brief Latest start or end point inside a file an editor field accepts, s.
inline constexpr double kEditTrimMinDb = -24.0;   ///< @brief Lowest file trim an editor field accepts, dB.
inline constexpr double kEditTrimMaxDb = 12.0;    ///< @brief Highest file trim an editor field accepts, dB.
inline constexpr double kEditMaxSeconds = 600.0;  ///< @brief Longest time an editor field accepts, seconds.

/// @brief Validates an edited number.
/// @param value [in] The typed value.
/// @param lo [in] Lower limit.
/// @param hi [in] Upper limit.
/// @return value clamped to lo..hi; nullopt for NaN or infinity (including text like 1e999).
std::optional<double> checkedEdit(double value, double lo, double hi) noexcept;

/// @brief Sets the change a step applies to one running sound.
/// @param step [in,out] The step.
/// @param change [in] Replaces the existing change with the same target, else is appended.
/// @return False, step unchanged, when it would exceed kMaxChanges, the target is 0
///         or above kMaxStepId, or levelDb/seconds are not finite.
bool setChange(Step& step, Change change);

/// @brief Removes the step's change for a target (back to the default behaviour).
/// @param step [in,out] The step.
/// @param target [in] Target step id; no change for it = no-op.
void removeChange(Step& step, StepId target);

/// @brief Sets the visible pad grid size.
/// @param show [in,out] The show.
/// @param rows [in] Clamped to kMinPadRows..kMaxPadRows.
/// @param cols [in] Clamped to kMinPadCols..kMaxPadCols, then reduced until rows * cols <= kMaxPads.
/// @note Show::pads grows with empty pads to rows * cols but never shrinks, so
///       pads hidden by a smaller grid keep their data.
void resizePadGrid(Show& show, int rows, int cols);

/// @brief The level a "keep" of a live trim results in.
/// @param levelDb [in] Current Sound::levelDb.
/// @param liveTrimDb [in] Live trim in dB.
/// @return level + trim clamped to the editor range; levelDb unchanged if the trim is not finite.
double keptLevelDb(double levelDb, double liveTrimDb) noexcept;

/// @brief Stores a kept live trim where the level the operator hears was set.
/// @param show [in,out] The show.
/// @param sound [in] Id of the step that starts the sound.
/// @param setter [in] Id of the step whose GO last set the sound's level
///        (ShowController::levelSetter(); 0 = unknown).
/// @param trimDb [in] The live trim being kept, dB.
/// @return True when stored; false, show unchanged, for an unknown step, a step
///         without a sound or a non-finite trim.
/// @note The setter's ChangeKind::Level change for the sound gets
///       keptLevelDb(); when setter is the sound itself, 0, a deleted step, or
///       a step whose change is gone or no longer a Level change, Sound::levelDb
///       of the start step is updated instead. keptLevelDb() clamps to
///       kEditLevelMinDb..kEditLevelMaxDb while the engine folds the whole trim.
bool keepIntoShow(Show& show, StepId sound, StepId setter, double trimDb) noexcept;

/// @brief Hands out a fresh step id.
/// @param show [in,out] The show; its lastStepId is set to the returned id.
/// @return max(lastStepId, highest existing step id) + 1, or nullopt when that
///         would exceed kMaxStepId (nothing is changed then).
/// @note The id is consumed even if the caller never adds the step, which is
///       harmless: ids only need to be unique, not dense.
std::optional<StepId> allocateStepId(Show& show) noexcept;

/// @brief Finds a file by the hash of its imported original.
/// @param show [in] The show to search.
/// @param sourceHash [in] Hex hash to look for.
/// @param minImportVersion [in] Files imported with an older AudioFile::importVersion are skipped.
/// @return Id of the first matching file; nullopt if none. An empty hash
///         (unknown) never matches, so unknown files are never merged.
std::optional<FileId> findFileByHash(const Show& show, std::string_view sourceHash, int minImportVersion) noexcept;

/// @brief Adds an audio file entry.
/// @param show [in,out] The show.
/// @param path [in] Path relative to the show folder.
/// @param sourceHash [in] Hash of the imported original; may be empty.
/// @param importVersion [in] Importer version that makes the file (AudioFile::importVersion); default 1.
/// @return The new id: highest existing file id + 1 (1 for the first); trimDb is 0.
FileId addFile(Show& show, std::string path, std::string sourceHash, int importVersion = 1);

/// @brief Inserts steps into the list, keeping their order.
/// @param show [in,out] The show.
/// @param index [in] Position of the first inserted step; clamped to 0..steps.size().
/// @param steps [in] Steps to insert; their ids must already be unique (see allocateStepId()).
void insertSteps(Show& show, int index, std::vector<Step> steps);

/// @brief Deletes a step.
/// @param show [in,out] The show.
/// @param id [in] Step to delete.
/// @return False if no such step. Files and lastStepId are left alone.
bool removeStep(Show& show, StepId id);

/// @brief Moves a step to another position.
/// @param show [in,out] The show.
/// @param id [in] Step to move.
/// @param newIndex [in] Index the step has in the resulting list; clamped to 0..steps.size()-1.
/// @return False if no such step.
bool moveStep(Show& show, StepId id, int newIndex);

/// @brief One file the operator dropped onto the step list.
struct DroppedFile {
    std::string name;        ///< @brief Name for its step: the file name without extension (UTF-8).
    std::string sourceHash;  ///< @brief Hash of the file's bytes; empty = unknown (never merged).
};

/// @brief What addDroppedFiles() made of one DroppedFile.
struct DroppedStep {
    StepId step = 0;       ///< @brief The new step's id; 0 when no step id was left (nothing added).
    FileId file = 0;       ///< @brief The file its sound plays; 0 when step is 0.
    bool newFile = false;  ///< @brief True when this call added the file entry, so it must be imported.
};

/// @brief Turns dropped files into steps: one makeStep() per file, in drop order.
/// @param show [in,out] The show; gets the steps and any new file entries.
/// @param index [in] Position of the first new step; clamped like insertSteps().
/// @param files [in] The dropped files, in drop order.
/// @param importVersion [in] The current importer version (core cannot see io's
///        constant): new file entries are stamped with it, and only known files
///        imported with at least this version are reused.
/// @param newPath [in] Called once per file whose hash the show does not know
///        yet (or knows only from an older import); returns its AudioFile::path.
///        It sees the show with the files added before it, so two new files
///        never get the same path.
/// @return One entry per file, in the same order.
/// @note A hash already in the show (also from earlier in the same drop)
///       reuses that file, so the same content is imported once. When step ids
///       run out (allocateStepId() returns nullopt) that file and every later
///       one are not added; their entries keep step 0. Control thread.
std::vector<DroppedStep> addDroppedFiles(Show& show, int index, const std::vector<DroppedFile>& files,
                                         int importVersion, const std::function<std::string(const DroppedFile&, const Show&)>& newPath);

}  // namespace bigtop
