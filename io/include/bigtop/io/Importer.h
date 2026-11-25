/// @file Importer.h
/// @brief Turns a dropped WAV, FLAC or MP3 into the show folder's audio format:
///        48 kHz, stereo, 24-bit FLAC, written atomically. The original is
///        not kept, so the conversion is high quality (r8brain resampling).
///        Also: the SHA-256 used to spot a file imported twice, and the
///        choice of a free file name in audio/. MP3s with a LAME/Xing tag are
///        imported gapless (encoder delay and padding removed).
#pragma once

#include <string>

#include <juce_core/juce_core.h>

#include "bigtop/Show.h"

namespace bigtop::io {

/// @brief Version of the import pipeline, stored as AudioFile::importVersion.
///
/// Raise it whenever the importer's output for the same source changes, so
/// a re-drop of an old file is converted again instead of merged with the
/// old conversion. 1 = first importer (MP3 with its encoder delay); 2 = gapless MP3 and FLAC.
inline constexpr int kImportVersion = 2;

/// @brief Whether a file can be imported, judged by its name only.
/// @param file Any file (it need not exist).
/// @return True for extension .wav, .flac or .mp3, in any letter case.
bool isImportable(const juce::File& file);

/// @brief SHA-256 of a file's bytes, for de-duplicating imports.
/// @param file File to hash. Blocking I/O; not for the audio thread.
/// @return 64 lowercase hex digits, or "" when the file cannot be read
///         (missing, a directory, no permission).
std::string hashFile(const juce::File& file);

/// @brief Picks the file an import should be written to.
/// @param audioDir The show's audio/ folder (its parent is the show folder,
///        which AudioFile::path is relative to).
/// @param stem Name without extension, already legal as a file name.
/// @param show The show, whose AudioFile paths count as taken even when the
///        file is not on disk (e.g. missing after a copy).
/// @return audioDir/`stem`.flac, else "`stem` (2).flac", "`stem` (3).flac", ...:
///         the first that neither exists nor is named by the show.
/// @note Names are compared ignoring letter case on every platform, so a
///       show folder copied to a case-insensitive file system still works.
/// @note Blocking (checks the disk); call from any thread except the audio thread.
juce::File chooseAudioTarget(const juce::File& audioDir, const juce::String& stem, const Show& show);

/// @brief Outcome of convertToShowAudio().
struct ImportResult {
    bool ok = false;    ///< @brief True when the target now holds the converted audio.
    std::string error;  ///< @brief Human-readable reason when ok is false.
};

/// @brief Converts a WAV, FLAC or MP3 into the show's audio format.
///
/// Decodes the source in chunks of at most 1 s, resamples to 48 kHz with
/// r8brain (skipped when the source already is 48 kHz), copies mono to both
/// channels, keeps only the first two of more than two channels, and writes
/// 24-bit FLAC to target.withFileExtension(".flac.tmp"), which is renamed to
/// target only when complete. The output has the source's duration.
///
/// @param source A WAV, FLAC or MP3 file.
/// @param target Where the FLAC goes; must not exist (see chooseAudioTarget()).
/// @return ok, or the reason it failed. On failure nothing of ours is left at
///         target or the .flac.tmp name; an already existing target fails at
///         once and is left untouched.
/// @note Blocking; call from any thread except the audio thread.
/// @note A WAV read error fails the import rather than importing silence. An
///       MP3 that ends before its estimated length is not an error: what
///       decodes is imported (a playable file is never refused), unless it
///       decodes under 25 % of the estimate and under 2 s: that is junk that
///       happened to look like a few MP3 frames, and fails.
/// @note An MP3 whose first frame is a Xing/Info header with a LAME-style
///       extension (written by LAME and ffmpeg) is imported gapless: the
///       encoder delay + 529 decoder frames at the start and padding - 529
///       (not below 0) at the end are dropped before resampling, so the
///       output has the length of the original audio. Without that extension
///       nothing is trimmed.
/// @note The finished FLAC is read back (length, rate, channels) before the
///       rename, since JUCE cannot report errors when the file is closed.
ImportResult convertToShowAudio(const juce::File& source, const juce::File& target);

}  // namespace bigtop::io
