/// @file LevelMeter.h
/// @brief The output level meter in the status bar: two segmented bars (left, right)
///        with a dB scale and a peak hold.
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "bigtop/io/AudioOutput.h"

namespace bigtop::app {

/// @brief A stereo peak meter of the final output (after the limiter), in dBFS from -40 to 0.
///
/// -40 rather than a wider range so a normal show level (around -10 dBFS) sits in
/// the middle of the bar instead of near its end.
/// @note Message thread. It never reads the audio thread itself: the owner feeds it
///       ShowSession::outputPeaks() on its UI timer.
class LevelMeter : public juce::Component {
public:
    /// @brief How long the peak tick stays before it falls to the current level, in ms.
    static constexpr juce::uint32 kHoldMs = 1000;
    /// @brief Segments per bar, 2 dB each from kFloorDb to 0 dBFS.
    static constexpr int kSegments = 20;
    /// @brief The bottom of the scale in dBFS; quieter lights nothing.
    static constexpr float kFloorDb = -40.0f;

    /// @brief How many segments a level lights.
    /// @param level Linear peak level (1 = 0 dBFS).
    /// @return 0..kSegments: a segment is lit once the level is above its lower edge.
    static int litSegments(float level) noexcept;

    /// @brief Shows new peaks.
    /// @param peaks The output peaks since the previous call, linear.
    /// @param nowMs The UI clock (uiMs()), for the peak hold.
    void setPeaks(io::OutputPeaks peaks, juce::uint32 nowMs);

    /// @brief The levels the bars show now, linear.
    /// @return The peaks given by the last setPeaks().
    io::OutputPeaks shown() const noexcept { return shown_; }

    /// @brief The levels the peak ticks show now, linear.
    /// @return Per channel, the highest level of the last kHoldMs, or the current one once that has passed.
    io::OutputPeaks held() const noexcept { return held_; }

    /// @brief Draws the two bars, their peak segments and the dB scale (JUCE callback).
    /// @param g Graphics context.
    void paint(juce::Graphics& g) override;

private:
    io::OutputPeaks shown_{}, held_{};
    juce::uint32 heldSinceL_ = 0, heldSinceR_ = 0;  // when each held peak was set
};

}  // namespace bigtop::app
