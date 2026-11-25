/// @file StepDetails.h
/// @brief The words of the step list's "Details" lines: what a step
///        does, one fact per line, plus the number formats they share with the
///        step editor's boxes.
///
/// Pure functions over the show data; the app only paints what they return.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "bigtop/Show.h"

namespace bigtop {

/// @brief One grey line under a step's name.
struct DetailLine {
    std::string text;          ///< @brief UTF-8 text of the line.
    bool missingFile = false;  ///< @brief The sound's file cannot play: draw the line in red.
    /// @brief Member-wise equality.
    /// @param other Line to compare with.
    /// @return True if both fields are equal.
    bool operator==(const DetailLine& other) const = default;
};

/// @brief A number the way the editor's dB and seconds boxes show it.
/// @param v Value.
/// @return At most two decimals, never fewer than one: "2.5", "3.0", "1.25";
///         a value that rounds to zero is "0.0" (no "-0.0"). Any thread.
std::string formatDecimal(double v);

/// @brief A file position the way the editor's start / end boxes show it.
/// @param seconds Position in seconds (zero or more).
/// @return "m:ss.d" rounded to a tenth ("0:12.0", "2:30.0"); minutes do not wrap
///         at an hour and 59.97 s reads "1:00.0". Any thread.
std::string formatMinSec(double seconds);

/// @brief The Details lines of one step.
/// @param show The show; names change targets and files, and the steps before
///        @p index tell which sounds would be running.
/// @param index The step's index in Show::steps; past the end gives no lines.
/// @param fileMissing True when the caller knows the sound's file cannot play
///        (a file not in Show::files is flagged anyway).
/// @return First, for a sound step, the sound (file name, then only what is not at
///         its default, joined by " · "); then one line per Level or FadeOut change,
///         named by its target step (Keep says nothing); then "everything else fades
///         out ..." when Others fades and a sound would really be left to fade: the list
///         is walked in order up to the step (a sound step adds its sound, a FadeOut
///         change removes its target, an everything-else fade removes all but what that
///         step names and its own sound), and a running sound must be neither named by
///         this step nor its own. 0 s reads "immediately". A fade step with nothing to
///         do gives no lines. Control thread (reads the show).
/// @param runningNow Null: the list walk above. Otherwise the steps whose sounds
///        really run now (sounding or pending, not fading out, no pads), used for the
///        "everything else" line instead of the walk; the other lines do not change.
///        The step list passes it for the NEXT row.
std::vector<DetailLine> describeStep(const Show& show, std::size_t index, bool fileMissing = false,
                                     const std::vector<StepId>* runningNow = nullptr);

}  // namespace bigtop
