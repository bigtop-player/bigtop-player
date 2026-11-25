/// @file ShowArchive.h
/// @brief Moves a whole show between machines as one `.bigtop.zip`, and keeps
///        the originals that make that small and lossless.
///
/// Every dropped file is also kept unchanged in the show folder's sources/
/// (keepOriginal()). An export carries, per audio file the show uses, that
/// original (the smallest lossless form: the file as it came) or, when there
/// is none, the FLAC from audio/. An import unpacks it into an empty folder
/// and checks it; the FLACs that were left out are rebuilt by the app through
/// the normal importer (filesToRebuild()). Entries are stored uncompressed
/// because audio does not deflate.
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <juce_core/juce_core.h>

#include "bigtop/Show.h"

namespace bigtop::io {

/// @brief Outcome of exportShow() and importShow().
struct ArchiveResult {
    bool ok = false;    ///< @brief True when the archive was written / the folder now holds a valid show.
    std::string error;  ///< @brief Human-readable reason when ok is false.
    /// @brief exportShow() only, when ok: AudioFile::path of each used file that had neither
    ///        original nor FLAC on disk and so is not in the zip. The caller should warn.
    std::vector<std::string> omitted{};
};

/// @brief Where an original of a given content is kept, relative to the show folder.
/// @param hash SHA-256 hex of the file (see hashFile()).
/// @param original The dropped file; only its extension is used.
/// @return "sources/<hash><extension in lower case>", e.g. "sources/ab12.mp3".
/// @note Pure; any thread.
std::string sourcePathFor(std::string_view hash, const juce::File& original);

/// @brief Copies a dropped file, unchanged, to its place in sources/.
/// @param source The dropped file.
/// @param showDir The show folder.
/// @param sourcePath Relative target, from sourcePathFor(); one escaping the show folder is refused.
/// @return True when the copy is in place (also when it already was: the name
///         holds the content hash, so an existing file is the same content).
///         False when the copy failed or the path is unsafe; nothing partial is left.
/// @note Written as .tmp and renamed, so a crash never leaves a half copy
///       under the real name. Blocking; any thread except the audio thread.
bool keepOriginal(const juce::File& source, const juce::File& showDir, const std::string& sourcePath);

/// @brief The file name an export gets.
/// @param showName The show folder's name.
/// @param when Local time of the export.
/// @return "<showName>-YYYYMMDD-HHMM.bigtop.zip" (characters illegal in file names become '_').
juce::String archiveName(const juce::String& showName, juce::Time when);

/// @brief Writes a show as a .bigtop.zip.
///
/// Contains show.json (written from @p show), state.json when the folder has
/// one, and for every AudioFile that a step or a pad uses: its original from
/// sources/ when present, otherwise its FLAC from audio/. No backups, no
/// unused files. Entries are stored uncompressed.
/// @param showDir The show folder the files are read from.
/// @param show The show to export (the open show, so unsaved edits are not lost).
/// @param zipTarget The file to create; replaced when it exists.
/// @return ok, or why not. The zip is first written to `<zipTarget>.tmp`, read
///         back (entry list and sizes) and only then renamed, so a failure
///         leaves no zip and no .tmp, and an older zipTarget stays intact.
/// @note A used file with neither original nor FLAC on disk is left out (its
///       step is red in the app as well) and listed in ArchiveResult::omitted; it
///       does not fail the export. A file path that escapes the show folder, or
///       entries adding up to more than 4 GB (zip sizes are 32-bit, no zip64),
///       fail it before anything is written.
/// @note The zip's state.json keeps the next step but always says mode "edit", so
///       an imported show never opens locked in Show mode.
/// @note Blocking and possibly long (gigabytes); call from a worker thread.
ArchiveResult exportShow(const juce::File& showDir, const Show& show, const juce::File& zipTarget);

/// @brief Unpacks a .bigtop.zip into an empty folder and validates it.
///
/// Refused (with the folder left empty again): a damaged or unreadable zip, an
/// entry that would land outside the folder (absolute path, "..", drive
/// letter, backslash), two entries with the same name ignoring case, a zip
/// without show.json, a show.json that the real
/// loader (fromJson) refuses, and a show.json naming an audio or source path
/// outside the folder. A state.json is rewritten with mode "edit" (the next
/// step kept), so the show never opens locked in Show mode.
/// Opening the show and rebuilding missing FLACs are the caller's job.
/// @param zipFile The archive.
/// @param emptyDir A folder that does not exist yet or is empty. A folder with
///        content is refused and left exactly as it was.
/// @return ok, or the reason. On success audio/ and backups/ exist too.
/// @note Blocking and possibly long; call from a worker thread.
ArchiveResult importShow(const juce::File& zipFile, const juce::File& emptyDir);

/// @brief The audio files an opened show has to convert again.
/// @param showDir The show folder.
/// @param show Its show.
/// @return Ids of the AudioFiles whose FLAC (path) is missing while their original
///         (sourcePath) exists, in show order. Files without an original are not
///         listed (nothing to rebuild from); paths escaping the show folder are ignored.
/// @note Blocking (checks the disk); any thread except the audio thread.
std::vector<FileId> filesToRebuild(const juce::File& showDir, const Show& show);

}  // namespace bigtop::io
