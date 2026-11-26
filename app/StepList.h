/// @file StepList.h
/// @brief The GO screen's step list: number, name and state of every step,
///        with the next step highlighted. Tapping a row selects it as next.
///        In Edit mode it is also where the show is built: files dropped on
///        it become steps and rows are reordered by dragging their grab
///        handle; an insertion line shows where a dragged row or file lands.
///        Every row has an Edit button (Edit and Rehearsal mode). With Details
///        on (the checkbox is in the mode bar) grey lines under each name say
///        what the step does. Screen readers read it as a list with
///        one item per row; an item's press makes that row NEXT, like a tap.
#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

#include "ShowSession.h"
#include "bigtop/StepDetails.h"

namespace bigtop::app {

/// @brief What a step row says on the right.
enum class StepRowState {
    None,         ///< @brief After the next step and not playing: nothing to say.
    Next,         ///< @brief The step the next GO runs ("NEXT").
    Starting,     ///< @brief Its sound waits out its start delay ("starting...").
    Playing,      ///< @brief Its sound is in the engine ("playing").
    Done,         ///< @brief Before the next step and not playing ("done").
    Importing,    ///< @brief Its file is still being imported; GO on it is refused ("importing...").
    MissingFile,  ///< @brief Its sound cannot play; GO on it is refused (red "missing file" / "import failed").
};

/// @brief Background of a row whose step has a sound playing (dark green: white text on it
///        stays readable). A row that is also NEXT keeps the NEXT colour and shows this as a
///        bar at its left edge.
inline constexpr juce::uint32 kPlayingTint = 0xff1e4d33;

/// @brief The state a row shows; the one rule behind the list's words.
/// @param row Index into Show::steps.
/// @param next ShowController::nextIndex().
/// @param inSnapshot The step's id is a voice key in the engine snapshot.
/// @param pending That voice is still in its start delay.
/// @param usable False when the step has a sound whose file cannot play.
/// @param importing True while that file is being imported (it is not usable yet).
/// @return Importing, then missing file (the operator must see both even when
///         selected), then NEXT (where GO goes), then starting/playing, then done.
constexpr StepRowState stepRowState(int row, int next, bool inSnapshot, bool pending, bool usable,
                                    bool importing = false) noexcept {
    if (importing) return StepRowState::Importing;
    if (!usable) return StepRowState::MissingFile;
    if (row == next) return StepRowState::Next;
    if (inSnapshot) return pending ? StepRowState::Starting : StepRowState::Playing;
    return row < next ? StepRowState::Done : StepRowState::None;
}

/// @brief Where a dragged row ends up.
/// @param from The dragged row's index.
/// @param gap The insertion gap it was dropped on (0 = before the first row,
///        n = after the last), as insertionGap() gives it.
/// @return The row's index in the list after the move (for ShowSession::moveStep).
constexpr int movedIndex(int from, int gap) noexcept { return gap > from ? gap - 1 : gap; }

/// @brief Height of a step row's name line in px (touch target, at least 48).
inline constexpr int kStepRowHeight = 48;
/// @brief Height of one Details line under the name, in px.
inline constexpr int kDetailLineHeight = 20;

/// @brief The one rule for a row's height.
/// @param detailLines Details lines the row shows (0 with Details off).
/// @return kStepRowHeight for none; each line adds kDetailLineHeight. The lines
///         start 8 px above the name line's bottom, so 8 px stay under the last one.
constexpr int stepRowHeight(int detailLines) noexcept { return kStepRowHeight + detailLines * kDetailLineHeight; }

/// @brief Every row's top edge, from the rows' Details line counts.
/// @param lines [in] Details lines per row.
/// @param tops [out] lines.size() + 1 entries: tops[i] is row i's top in content px
///        (tops[0] = 0), the last entry is the content height.
/// @note The only place offsets are summed; hit tests, the insertion line and the
///       scroll to NEXT all read these offsets.
constexpr void rowTops(std::span<const int> lines, std::span<int> tops) noexcept {
    tops[0] = 0;
    for (std::size_t i = 0; i < lines.size(); ++i) tops[i + 1] = tops[i] + stepRowHeight(lines[i]);
}

/// @brief The row under a content position.
/// @param contentY Vertical position in content px (scroll already added).
/// @param tops Row offsets as rowTops() gives them.
/// @return The row index, or -1 above the first or below the last row.
constexpr int rowAt(int contentY, std::span<const int> tops) noexcept {
    for (std::size_t i = 0; i + 1 < tops.size(); ++i)
        if (contentY >= tops[i] && contentY < tops[i + 1]) return static_cast<int>(i);
    return -1;
}

/// @brief The insertion gap under a point of the list.
/// @param y Vertical position in px, relative to the rows' visible top edge (may be outside it).
/// @param tops Row offsets as rowTops() gives them (numRows + 1 entries; an empty span = no rows).
/// @param scrollY How far the list is scrolled, in px (0 = first row at the top).
/// @return The nearest gap between rows: 0 = before the first row, numRows = after the last.
///         The top half of a row gives the gap above it, the bottom half the gap below it.
///         The one rule behind both the insertion line and the drop.
constexpr int insertionGap(int y, std::span<const int> tops, int scrollY) noexcept {
    const int content = y + scrollY;
    const int numRows = tops.empty() ? 0 : static_cast<int>(tops.size()) - 1;
    for (int i = 0; i < numRows; ++i) {
        const auto row = static_cast<std::size_t>(i);
        if (content < tops[row] + (tops[row + 1] - tops[row]) / 2) return i;
    }
    return numRows;
}

/// @brief Where to scroll so a row is in view, moving as little as possible.
/// @param top The row's top in content px.
/// @param bottom The row's bottom in content px.
/// @param viewY Current scroll position in px.
/// @param viewH Visible height in px.
/// @return The new scroll position: unchanged when the row is in view, its top at the
///         view's top when it is above (or taller than the view), else its bottom at the
///         view's bottom.
constexpr int scrollToShow(int top, int bottom, int viewY, int viewH) noexcept {
    if (top < viewY) return top;
    if (bottom > viewY + viewH) return bottom - viewH > top ? top : bottom - viewH;
    return viewY;
}

/// @brief The list of steps; repaints from the snapshot given to refresh().
/// @note Message thread. The rows are painted by one inner component in a
///       Viewport (with Details on, rows have different heights, which ListBox
///       cannot do). The list never takes keyboard focus, so Space, Esc and
///       Up/Down reach the main component. Row drags start only on the handle
///       and never change NEXT. Drops and moves are passed to the session,
///       which ignores them where the mode forbids them. For screen readers the rows
///       component is a list whose items are one invisible, click-through child per row.
class StepList : public juce::Component, public juce::FileDragAndDropTarget, private juce::Timer {
public:
    /// @brief Height of a row's name line in px; a row without Details lines is this high.
    static constexpr int kRowHeight = kStepRowHeight;
    /// @brief Width of the grab handle at a row's left edge (Edit mode), in px.
    static constexpr int kHandleWidth = 48;
    /// @brief Width of a row's Edit button at its right edge, in px.
    static constexpr int kEditWidth = 96;
    /// @brief Whether a tap lands on a row's Edit button.
    /// @param x Tap position from the row's left edge, in px.
    /// @param rowY Tap position from the row's top edge, in px.
    /// @param width The row's width in px.
    /// @return True in the Edit column of the name line only; below it (the Details
    ///         lines of a tall row) a tap selects like the rest of the row.
    static constexpr bool editHit(int x, int rowY, int width) noexcept {
        return rowY < kRowHeight && x >= width - 12 - kEditWidth;
    }

    /// @brief Builds the list over a session.
    /// @param session Must outlive this; read only while it has a show open.
    /// @param settings Holds the Details choice; must outlive this.
    StepList(ShowSession& session, io::AppSettings& settings);

    /// @brief Releases the rows component.
    ~StepList() override;

    /// @brief Called with a row index when the user taps a row.
    std::function<void(int)> onSelect;

    /// @brief Called with a step's id when the user taps its row's Edit button
    ///        (Edit and Rehearsal mode). NEXT is not changed.
    std::function<void(StepId)> onEdit;

    /// @brief Called while a handle drag moves, with the pointer in screen coordinates,
    ///        and with nullopt when the drag ends. Returns true when the point is over a
    ///        drop target outside the list (a pad): the list then shows no insertion line.
    std::function<bool(std::optional<juce::Point<int>>)> onDragOver;

    /// @brief Called when a handle drag is released outside the list, with the dragged
    ///        step and the point in screen coordinates. Returns true when it took the drop
    ///        (the step went onto a pad): the list does not reorder.
    std::function<bool(StepId, juce::Point<int>)> onDropOutside;

    /// @brief Takes the engine's state for this tick and repaints; scrolls the
    ///        next step into view when it changed.
    /// @param snap One Engine snapshot for this UI tick.
    /// @note Only while the session has a show open.
    void refresh(const Snapshot& snap);

    /// @brief Turns the Details lines on or off and remembers it in the app settings.
    /// @param on True to show them.
    /// @note The mode bar's Details checkbox calls this (through MainComponent).
    void setDetails(bool on);

    /// @brief Whether the Details lines are shown.
    /// @return The last setDetails() value, or the app settings' at construction.
    bool details() const noexcept { return detailsOn_; }

    /// @brief Fills the component with the rows (JUCE callback).
    void resized() override;

    /// @brief Fills the list's background (JUCE callback).
    /// @param g Graphics context.
    void paint(juce::Graphics& g) override;

    /// @brief Accepts any file drag; the session refuses what it cannot import (JUCE callback).
    /// @param files The dragged files.
    /// @return True.
    bool isInterestedInFileDrag(const juce::StringArray& files) override;

    /// @brief Imports dropped files at the gap under the drop point (JUCE callback).
    /// @param files Full paths, in drop order.
    /// @param x Drop point, relative to this component.
    /// @param y Drop point, relative to this component.
    void filesDropped(const juce::StringArray& files, int x, int y) override;

    /// @brief Starts showing the insertion line for a file drag (JUCE callback).
    /// @param files The dragged files (unused).
    /// @param x Pointer position, relative to this component.
    /// @param y Pointer position, relative to this component.
    void fileDragEnter(const juce::StringArray& files, int x, int y) override;

    /// @brief Moves the insertion line with the pointer (JUCE callback).
    /// @param files The dragged files (unused).
    /// @param x Pointer position, relative to this component.
    /// @param y Pointer position, relative to this component.
    void fileDragMove(const juce::StringArray& files, int x, int y) override;

    /// @brief Hides the insertion line when the file drag leaves (JUCE callback).
    /// @param files The dragged files (unused).
    void fileDragExit(const juce::StringArray& files) override;

    /// @brief Draws the insertion line over the rows (JUCE callback).
    /// @param g Graphics context.
    void paintOverChildren(juce::Graphics& g) override;

    /// @brief Watches presses on the rows, to start a drag from a handle (JUCE callback). A press
    ///        of the finger whose drag is still set first drops that drag (its release was lost).
    /// @param e The press, from the rows component (this listens to it).
    void mouseDown(const juce::MouseEvent& e) override;

    /// @brief Moves the insertion line while a row is dragged (JUCE callback).
    /// @param e The drag.
    void mouseDrag(const juce::MouseEvent& e) override;

    /// @brief Drops a row drag whose release never came: the pointer left the rows with no
    ///        button held (JUCE callback). Only the dragging finger's exit counts.
    /// @param e The exit, from the rows component.
    void mouseExit(const juce::MouseEvent& e) override;

    /// @brief Drops the dragged row at the insertion line, or reports a tap (JUCE callback).
    /// @param e The release.
    void mouseUp(const juce::MouseEvent& e) override;

private:
    /// App test hook (app/tests/AppTestSupport.h): Details rewording count, row offsets, the
    /// insertion line, the scroll timer.
    friend struct AppTestAccess;

    class Rows;
    class RowItem;

    int numRows() const;
    void layoutRows();
    void showRow(int row);
    void paintRows(juce::Graphics& g);
    void paintRow(int row, juce::Graphics& g, int width, int height);
    void tapped(int row, int x, int contentY);
    int contentWidth() const;
    juce::String tooltipAt(int contentY) const;
    juce::String rowTitle(int row) const;  // what a screen reader reads for the row
    void syncItems();                      // one RowItem per laid-out row, at the row's bounds
    void notifyTitles();                   // tells screen readers about rows whose words changed
    void timerCallback() override;
    void dragTo(int y);
    void cancelDrag();  // forgets the row drag: no line, no carried row, the Viewport may scroll again
    bool handleShown() const;

    ShowSession& session_;
    io::AppSettings& settings_;
    bool detailsOn_ = false;  // Details lines shown (AppSettings::stepDetails)
    juce::Viewport view_;
    std::unique_ptr<Rows> rows_;
    std::vector<std::unique_ptr<RowItem>> items_;  // after rows_: destroyed first
    std::vector<int> tops_{0};  // rowTops() of the current rows: offsets for paint, hits, gaps and scrolling
    // Per row; all empty with Details off. A step whose everything-else rule fades always has that
    // line, with empty text while nothing is left to fade: row heights depend only on the show.
    std::vector<std::vector<DetailLine>> details_;
    // What details_ was worded from: compared with == each tick, so the words are rebuilt only
    // when the steps or files change (no allocation otherwise).
    bool described_ = false;
    std::vector<Step> describedSteps_;
    std::vector<AudioFile> describedFiles_;
    std::vector<bool> unknownFile_;  // per row: describeStep flagged the file itself (not in Show::files)
    // The NEXT row's everything-else line follows what really plays: the step voices of the snapshot
    // that are not fading out (sorted keys), and the NEXT row it was worded for.
    std::vector<StepId> runningNow_;
    std::vector<StepId> describedRunning_;
    int describedNext_ = -1;
    int describes_ = 0;  // times rows were worded; app tests check it grows only on a show edit or a NEXT / running change
    Snapshot snap_{};
    int shownNext_ = -1;  // next index at the last refresh, to scroll only on change
    int dragFrom_ = -1;   // row whose handle is pressed, -1 = none
    bool dragActive_ = false;  // that press has moved: the line shows and the timer runs
    int pressSource_ = 0;  // input source index of that press; others are ignored
    bool fileDrag_ = false;  // a file drag is over the list
    int gap_ = -1;        // insertion gap to draw while either drag runs, -1 = none
    int pointerY_ = 0;    // last drag position (relative to the rows' viewport), for the edge auto-scroll
};

}  // namespace bigtop::app
