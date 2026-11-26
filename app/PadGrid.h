/// @file PadGrid.h
/// @brief The pads screen: a grid of one-shot sound buttons (jingles, stingers)
///        with rows x columns taken from the show settings (design 8).
///
/// Tapping a pad plays it from the start over everything else, or fades it out
/// when it is playing (ShowController::triggerPad). A playing pad shows its
/// remaining time. Dropping an audio file on a pad assigns it in Edit mode (same
/// importer and dedup as a step drop). An "Edit pads" toggle (Edit and Rehearsal
/// mode) makes a tap open the pad editor instead of playing, so a stray tap while
/// setting up never fires a sound. Buttons are never smaller than kPadMinPx: when
/// the area is too small the grid scrolls instead.
#pragma once

#include <functional>
#include <memory>

#include <juce_gui_basics/juce_gui_basics.h>

#include "ShowSession.h"

namespace bigtop::app {

/// @brief Smallest pad button edge in px (touch target).
inline constexpr int kPadMinPx = 48;
/// @brief Space between pad buttons and around the grid, px.
inline constexpr int kPadGap = 6;

/// @brief Edge of one pad button along one axis.
/// @param available Room along that axis in px (the viewport's width or height).
/// @param count Pads along that axis (columns for the width, rows for the height); positive.
/// @return The edge that fills @p available exactly, but never below kPadMinPx
///         (the grid then is larger than the room and scrolls).
constexpr int padCellSize(int available, int count) noexcept {
    const int fit = (available - kPadGap * (count + 1)) / count;
    return fit < kPadMinPx ? kPadMinPx : fit;
}

/// @brief Size of the whole grid along one axis, buttons and gaps.
/// @param cell Button edge from padCellSize().
/// @param count Pads along that axis.
/// @return Pixels the content needs; larger than the room means scrolling.
constexpr int padContentSize(int cell, int count) noexcept { return count * cell + (count + 1) * kPadGap; }

/// @brief Which pad is under a point of the grid's content.
/// @param x Point in content coordinates, px (scrolling already added).
/// @param y Point in content coordinates, px.
/// @param cellW Button width from padCellSize().
/// @param cellH Button height from padCellSize().
/// @param rows Grid rows (Settings::padRows).
/// @param cols Grid columns (Settings::padCols).
/// @return Row-major pad index, or -1 on a gap, outside the grid, or for a degenerate grid.
///         Only pads inside rows x cols exist here, so a hidden pad can never be hit.
constexpr int padIndexAt(int x, int y, int cellW, int cellH, int rows, int cols) noexcept {
    if (x < 0 || y < 0 || cellW <= 0 || cellH <= 0 || rows <= 0 || cols <= 0) return -1;
    const int col = x / (cellW + kPadGap);
    const int row = y / (cellH + kPadGap);
    if (col >= cols || row >= rows) return -1;
    // Each slot is a gap (leading) followed by the button; a point in the gap is no pad.
    if (x - col * (cellW + kPadGap) < kPadGap || y - row * (cellH + kPadGap) < kPadGap) return -1;
    return row * cols + col;
}

static_assert(padCellSize(460, 2) == 221);       // 2 columns and 3 gaps fill 460 px
static_assert(padCellSize(100, 6) == kPadMinPx); // too small: the minimum, the grid scrolls
static_assert(padContentSize(48, 6) == 6 * 48 + 7 * kPadGap);
static_assert(padIndexAt(kPadGap, kPadGap, 100, 60, 4, 2) == 0);                       // first button's corner
static_assert(padIndexAt(kPadGap + 100 + kPadGap, kPadGap, 100, 60, 4, 2) == 1);       // second column
static_assert(padIndexAt(kPadGap, kPadGap + 60 + kPadGap, 100, 60, 4, 2) == 2);        // second row
static_assert(padIndexAt(kPadGap + 99, kPadGap + 59, 100, 60, 4, 2) == 0);             // last pixel of pad 0
static_assert(padIndexAt(kPadGap + 100, kPadGap, 100, 60, 4, 2) == -1);                // gap between columns
static_assert(padIndexAt(0, 0, 100, 60, 4, 2) == -1);                                  // outer gap
static_assert(padIndexAt(3 * (100 + kPadGap) + kPadGap, kPadGap, 100, 60, 4, 2) == -1); // beyond the columns
static_assert(padIndexAt(kPadGap, 4 * (60 + kPadGap) + kPadGap, 100, 60, 4, 2) == -1);  // beyond the rows
static_assert(padIndexAt(-1, 10, 100, 60, 4, 2) == -1);

/// @brief The pad grid.
/// @note Message thread. Never takes keyboard focus, so Space, Esc and P keep
///       working on the main window.
class PadGrid : public juce::Component, public juce::FileDragAndDropTarget {
public:
    /// @brief Builds the grid over a session.
    /// @param session Must outlive this; read only while it has a show open.
    explicit PadGrid(ShowSession& session);

    /// @brief Destroys the grid.
    ~PadGrid() override;

    /// @brief Called with a pad index when a pad was tapped while "Edit pads" is on.
    std::function<void(int)> onEditPad;

    /// @brief Takes the engine's state for this tick; repaints when it changed.
    /// @param snap One Engine snapshot for this UI tick (playing pads, remaining time).
    /// @note Only while the session has a show open. Also follows the show's rows and
    ///       columns and turns "Edit pads" off where the mode no longer allows editing.
    void refresh(const Snapshot& snap);

    /// @brief Lays out the header and the scrolling grid (JUCE callback).
    void resized() override;

    /// @brief Accepts any file drag; the session refuses what it cannot import (JUCE callback).
    /// @param files The dragged files (unused).
    /// @return True.
    bool isInterestedInFileDrag(const juce::StringArray& files) override;

    /// @brief Assigns the first dropped file to the pad under the drop point (JUCE callback).
    /// @param files Full paths; only the first is used (a pad holds one sound).
    /// @param x Drop point, relative to this component.
    /// @param y Drop point, relative to this component.
    /// @note A drop on a gap or outside the grid does nothing. Refused with a notice outside Edit mode.
    void filesDropped(const juce::StringArray& files, int x, int y) override;
    /// @brief The visible pad under a point (a step dragged from the list).
    /// @param screenPos The point in screen coordinates.
    /// @return The pad's index, or -1 over a gap, outside the grid or when the grid is hidden.
    int padAtScreen(juce::Point<int> screenPos) const;
    /// @brief Marks the pad a step drag would land on.
    /// @param index Pad index, or -1 for none.
    void setDropHighlight(int index);

private:
    /// App test hook (app/tests/AppTestSupport.h): the painted cells and the Edit pads toggle.
    friend struct AppTestAccess;

    class Cells;
    int padAtContent(int x, int y) const;  // x, y in the scrolled content
    int padAt(int x, int y) const;         // x, y in this component (a drop point)
    int dropHighlight_ = -1;               // the pad a step drag is over, -1 = none
    void paintCells(juce::Graphics& g);
    void tapped(int index);
    void layoutCells();
    juce::String labelOf(int index) const;

    ShowSession& session_;
    juce::Label title_{{}, "Pads"};
    juce::ToggleButton editToggle_{"Edit pads"};
    juce::Viewport view_;
    std::unique_ptr<Cells> cells_;
    Snapshot snap_{};
    int rows_ = 0, cols_ = 0;  // grid size the cells were laid out for
};

}  // namespace bigtop::app
