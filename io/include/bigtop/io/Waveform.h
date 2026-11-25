/// @file Waveform.h
/// @brief A small peak summary of a show audio file, for drawing its waveform.
///
/// The summary is read once per file (on a background thread) and kept in
/// memory only; nothing is written to the show folder. It is frame-indexed,
/// so any start..end range (a step's start and end point, a loop range) can
/// be drawn from it at any width with peakRange().
#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

#include <juce_core/juce_core.h>

namespace bigtop::io {

/// @brief Points in a full-size summary (fewer for a file with fewer frames).
inline constexpr int kWaveformPoints = 4000;

/// @brief Min/max peaks of a whole file in equal frame slices, mono.
/// @note Point i covers frames [i * frames / points, (i + 1) * frames / points),
///       where points = min.size() = max.size(). Values are in [-1, 1]; each
///       point is the min/max over both channels. Empty (frames == 0, no
///       points) means "no summary": the file is missing, unreadable or the
///       read was cancelled.
struct Peaks {
    int64_t frames = 0;      ///< @brief File length in frames at 48 kHz.
    std::vector<float> min;  ///< @brief Lowest sample per point.
    std::vector<float> max;  ///< @brief Highest sample per point.
};

/// @brief Reads a show FLAC once, in blocks, and summarises it.
/// @param flac [in] The 48 kHz show audio file (any format JUCE reads works).
/// @param cancel [in] Checked once per block; when it becomes true the read
///        stops and empty Peaks are returned. Null = never cancelled.
/// @return kWaveformPoints points (or one per frame for a shorter file); empty
///         Peaks when the file is missing, not audio, empty or the read was cancelled.
/// @note Any thread; blocking disk IO (a long file takes a few hundred ms),
///       so never on the message or audio thread.
Peaks readPeaks(const juce::File& flac, const std::atomic<bool>* cancel = nullptr);

/// @brief The min/max of @p columns equal slices of a frame range, for drawing.
/// @param peaks [in] A summary from readPeaks(); empty gives all zeros.
/// @param start [in] First frame of the range (48 kHz).
/// @param end [in] One past the last frame; the range is clamped to the file.
/// @param columns [in] Number of slices (usually the bar's width in px); 0 or less gives empty outputs.
/// @param lo [out] Resized to @p columns: the lowest value per slice.
/// @param hi [out] Resized to @p columns: the highest value per slice.
/// @note Each slice takes every point that overlaps it, at least the nearest
///       one, so a range narrower than one point per column still draws. A
///       slice wholly outside the file (or an empty range) gives 0, 0. Any thread.
void peakRange(const Peaks& peaks, int64_t start, int64_t end, int columns, std::vector<float>& lo,
               std::vector<float>& hi);

}  // namespace bigtop::io
