/// @file RunningSounds.h
/// @brief The GO screen's "running" area: one row per playing step sound
///        (newest on top) with its name, a position bar (seek; the sound's
///        waveform behind it once read), elapsed and
///        remaining time, the timecode of the driving sound, a live fader and
///        the Fade, Keep and Edit buttons.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

#include "ShowSession.h"

namespace bigtop::app {

/// @brief The file frame a tap or release at pixel @p x on the position bar means.
/// @param x Pixel across the bar; anything left of it (x <= 0) means the start,
///        right of it (x >= width) the last frame, so a drag past the ends clamps.
/// @param width Bar width in px; 0 or less returns @p startFrame.
/// @param startFrame First frame of the bar's range (the sound's start point).
/// @param endFrame One past the last frame (end point, or the file's end);
///        at or before @p startFrame returns @p startFrame.
/// @return A frame in [startFrame, endFrame - 1], linear in x. Any thread.
constexpr int64_t seekFrameForX(int x, int width, int64_t startFrame, int64_t endFrame) noexcept {
    if (width <= 0 || endFrame <= startFrame || x <= 0) return startFrame;
    if (x >= width) return endFrame - 1;
    // 64-bit product: a 3 h file at 48 kHz (5e8 frames) times a few thousand px fits easily.
    return startFrame + (endFrame - startFrame) * x / width;
}

/// @brief Whole minutes and seconds of a remaining time.
struct MinSec {
    int64_t minutes;  ///< @brief Whole minutes (not wrapped at an hour).
    int seconds;      ///< @brief 0..59.
};

/// @brief Splits a remaining time for the large "-m:ss" readout.
/// @param frames Remaining frames at 48 kHz; zero or less gives 0:00.
/// @return Minutes and seconds with the time rounded UP to whole seconds, so
///         "-0:00" shows only when nothing remains. Any thread.
constexpr MinSec splitTime(int64_t frames) noexcept {
    if (frames <= 0) return {0, 0};
    // Round up so a sound with one frame left still reads 0:01, not 0:00.
    constexpr int64_t kRate = static_cast<int64_t>(io::kEngineSampleRate);
    const int64_t total = (frames + kRate - 1) / kRate;
    return {total / 60, static_cast<int>(total % 60)};
}

static_assert(io::kEngineSampleRate == 48000.0, "the splitTime asserts below count in 48 kHz frames");
static_assert(seekFrameForX(0, 100, 0, 1000) == 0);
static_assert(seekFrameForX(-30, 100, 500, 1500) == 500);
static_assert(seekFrameForX(100, 100, 0, 1000) == 999);
static_assert(seekFrameForX(250, 100, 500, 1500) == 1499);
static_assert(seekFrameForX(50, 100, 0, 1000) == 500);
static_assert(seekFrameForX(50, 100, 1000, 2000) == 1500);
static_assert(seekFrameForX(10, 0, 40, 90) == 40);
static_assert(seekFrameForX(10, 100, 90, 90) == 90);
static_assert(splitTime(0).minutes == 0 && splitTime(0).seconds == 0);
static_assert(splitTime(-5).minutes == 0 && splitTime(-5).seconds == 0);
static_assert(splitTime(1).minutes == 0 && splitTime(1).seconds == 1);        // rounds up
static_assert(splitTime(48000).seconds == 1 && splitTime(48001).seconds == 2);
static_assert(splitTime(48000 * 84).minutes == 1 && splitTime(48000 * 84).seconds == 24);
static_assert(splitTime(48000 * 3600).minutes == 60);

/// @brief Formats a position as m:ss for the running-sounds rows.
/// @param frames Frames at 48 kHz (io::kEngineSampleRate); negative shows as 0:00.
/// @return Whole minutes, then seconds rounded down, e.g. "3:07"; minutes are
///         not wrapped at an hour ("75:00").
juce::String formatMinutesSeconds(int64_t frames);

/// @brief Height of the running area's row strip when nothing plays, in px (a slim "Nothing playing" line).
inline constexpr int kRunningEmptyHeight = 32;
/// @brief Most rows shown at once; more scroll.
inline constexpr int kRunningRowsShown = 4;

/// @brief Height of the area that holds the rows (not the scrolled content).
/// @param rows Number of running rows.
/// @param rowHeight One row's height in px (RunningSounds::kRowHeight).
/// @return kRunningEmptyHeight for no rows, else the rows' height capped at kRunningRowsShown rows.
constexpr int runningAreaHeight(int rows, int rowHeight) noexcept {
    return rows <= 0 ? kRunningEmptyHeight : (rows < kRunningRowsShown ? rows : kRunningRowsShown) * rowHeight;
}

/// @brief How one running row divides its width, left to right: name, time column, bar, fader, buttons.
struct RowParts {
    int nameW;       ///< @brief Name column, px.
    int timeW;       ///< @brief Remaining/elapsed column, px.
    int faderW;      ///< @brief Fader including its dB readout, px; always kRowFaderWidth.
    int barW;        ///< @brief Position bar, px; never below kRowBarFloor from 682 px row width on.
    bool secondary;  ///< @brief Whether the TC and elapsed lines are shown (dropped first when narrow).
};

/// @brief Smallest position bar, px.
inline constexpr int kRowBarFloor = 150;
/// @brief The fader's draggable track, px (never below this): the slider's width less the thumb at both ends.
inline constexpr int kRowFaderTrack = 150;
/// @brief Thumb radius of the horizontal fader, px: juce::LookAndFeel_V4 caps it at 7 (a 56 px row is tall enough to reach the cap).
inline constexpr int kRowFaderThumbRadius = 7;
/// @brief The fader's dB readout box beside the track, px.
inline constexpr int kRowFaderBox = 64;
/// @brief The fader with its readout, px (always shown).
inline constexpr int kRowFaderWidth = kRowFaderTrack + 2 * kRowFaderThumbRadius + kRowFaderBox;
/// @brief Width of the Fade, Keep and Edit buttons, px each.
inline constexpr int kRowButtonWidth = 56;

/// @brief The one rule for a row's horizontal layout.
/// @param width The row's width in px.
/// @param buttons How many of Fade / Keep / Edit are shown (1..3).
/// @return Name and time columns shrink first and the secondary text lines go below 560 px;
///         the fader and the bar keep kRowFaderWidth and kRowBarFloor whenever @p width is at
///         least 682 px, whichever buttons are shown.
constexpr RowParts rowParts(int width, int buttons) noexcept {
    constexpr int kMinName = 60, kMinTime = 64, kPad = 12;
    const int inner = width - kPad - buttons * kRowButtonWidth;
    const bool compact = width < 560;
    int nameW = compact ? 90 : 130, timeW = compact ? 70 : 84;
    int excess = nameW + timeW + kRowFaderWidth + kRowBarFloor - inner;
    if (excess > 0) {
        const int fromName = excess < nameW - kMinName ? excess : nameW - kMinName;
        nameW -= fromName;
        excess -= fromName;
        timeW -= excess < timeW - kMinTime ? excess : timeW - kMinTime;
    }
    const int bar = inner - nameW - timeW - kRowFaderWidth;
    return {nameW, timeW, kRowFaderWidth, bar > 0 ? bar : 0, !compact};
}

// The running area spans the window, so a row is at least 1024 px: the fader and the bar never drop out.
static_assert(rowParts(1024, 3).faderW - kRowFaderBox - 2 * kRowFaderThumbRadius >= 150 && rowParts(1024, 3).barW >= kRowBarFloor);
static_assert(rowParts(1024, 1).faderW - kRowFaderBox - 2 * kRowFaderThumbRadius >= 150 && rowParts(1024, 1).barW >= kRowBarFloor);
static_assert(rowParts(682, 3).faderW - kRowFaderBox - 2 * kRowFaderThumbRadius >= 150 && rowParts(682, 3).barW >= kRowBarFloor);
static_assert(rowParts(1024, 3).secondary && !rowParts(500, 3).secondary);
static_assert(runningAreaHeight(0, 56) == 32 && runningAreaHeight(1, 56) == 56 && runningAreaHeight(9, 56) == 224);

/// @brief Rows for the step voices of each snapshot; the faders send live trim.
/// @note Message thread. Sized by refresh() to its rows (put it in a Viewport).
///       No child takes keyboard focus. Each row's component ID is its step id, and its
///       position bar, fader and Fade / Keep / Edit buttons have the IDs "bar", "fader", "fade",
///       "keep" and "edit" (the app tests find them with findChildWithID()).
class RunningSounds : public juce::Component {
public:
    /// @brief Height of one row in px (touch target, at least 48).
    static constexpr int kRowHeight = 56;

    /// @brief Builds an empty area over a session.
    /// @param session Must outlive this; read only while it has a show open.
    explicit RunningSounds(ShowSession& session);

    /// @brief Destroys the rows.
    ~RunningSounds() override;

    /// @brief Called with a step's id when its row's Edit button is tapped
    ///        (only shown where the mode allows EditValue). Set by the owner.
    std::function<void(StepId)> onEdit;

    /// @brief Adds rows for new step voices (on top), removes rows whose key
    ///        left the snapshot (their fader with them, so a sound that comes
    ///        back starts at 0 dB like the engine's trim), and updates times.
    /// @param snap One Engine snapshot for this UI tick.
    /// @note Only while the session has a show open. Sends a trim only to
    ///       retry a fader move the full engine queue refused.
    void refresh(const Snapshot& snap);

    /// @brief A GO just restarted @p key (or started it): the engine set its
    ///        trim to 0 dB, so the row's fader goes to 0 dB and the row to the top.
    /// @param key The started step's id.
    /// @note Also ends the row's own Fade state and any position-bar drag: the
    ///       restarted sound is not the one that was faded or being dragged.
    void restarted(VoiceKey key);

    /// @brief Number of rows now shown.
    /// @return The row count (the owner sizes the area from it with runningAreaHeight()).
    int rows() const noexcept { return static_cast<int>(rows_.size()); }

    /// @brief Paints "Nothing playing" while there are no rows (JUCE callback).
    /// @param g Graphics context.
    void paint(juce::Graphics& g) override;

    /// @brief Screenshot/testing hook: moves a row's fader the way a finger would (sends the trim).
    /// @param key The step whose row to move; no such row does nothing.
    /// @param db New fader position in dB.
    void moveFaderForScreenshot(VoiceKey key, double db);

    /// @brief Lays out the rows (JUCE callback).
    void resized() override;

private:
    class Row;
    ShowSession& session_;
    std::vector<std::unique_ptr<Row>> rows_;  // newest first
};

}  // namespace bigtop::app
