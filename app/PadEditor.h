/// @file PadEditor.h
/// @brief The side panel that edits one pad: its name, level and the file's
///        trim, plus Clear pad (design 8).
///
/// The name and Clear pad are Edit mode only (Action::AssignPad); level and
/// trim are also editable in Rehearsal mode (Action::EditValue). In Show mode
/// the panel never opens. Every change is one ShowSession::edit() (one
/// verified save), also while the pad plays: a new name does not touch the
/// sound; a new level or trim ramps the running sound to it over the engine's
/// declick (ShowController::levelPad); Clear pad fades the running sound out
/// exactly as a second tap does, so no voice is left that no tap can fade.
#pragma once

#include <functional>

#include <juce_gui_basics/juce_gui_basics.h>

#include "NumberField.h"
#include "ShowSession.h"

namespace bigtop::app {

/// @brief The pad editor panel.
/// @note Message thread. It follows the pad by index; a pad index that the grid
///       no longer shows (grid shrunk, show closed) closes the panel.
class PadEditor : public juce::Component {
public:
    /// @brief Builds a closed (hidden) panel.
    /// @param session The session whose open show is edited; must outlive this.
    explicit PadEditor(ShowSession& session);

    /// @brief Destroys the fields.
    ~PadEditor() override;

    /// @brief Shows the panel for a pad.
    /// @param index Pad index, 0..padRows*padCols-1. Out of range does nothing.
    void open(int index);

    /// @brief Brings the fields up to date; call from the 30 Hz UI timer.
    /// @note Does not overwrite a field that is being typed in. Closes (onClose)
    ///       when the pad is no longer visible in the grid. Cheap when closed.
    void refresh();

    /// @brief Called when the panel wants to go away: Close was tapped, or the pad is gone.
    std::function<void()> onClose;

    /// @brief Paints the panel background (JUCE callback).
    /// @param g Graphics context.
    void paint(juce::Graphics& g) override;
    /// @brief Lays out the title bar and the fields (JUCE callback).
    void resized() override;

private:
    /// App test hook (app/tests/AppTestSupport.h): the name box, the level and trim fields.
    friend struct AppTestAccess;

    const Pad* pad() const;
    bool edit(Action action, const std::function<bool(Show&, Pad&)>& change);
    void commitName();

    ShowSession& session_;
    int index_ = -1;
    juce::Label title_, fileLabel_, nameLabel_{{}, "Name"}, levelLabel_{{}, "Level"}, trimLabel_{{}, "File trim"}, startLabel_{{}, "Start"}, endLabel_{{}, "End"}, hint_;
    juce::TextButton close_{"Close"}, clear_{"Clear pad"};
    juce::TextEditor name_;
    NumberField level_, trim_, start_, end_;  // start/end: the pad's range in the file, Edit mode only
};

}  // namespace bigtop::app
