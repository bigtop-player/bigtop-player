/// @file ModeBar.h
/// @brief The strip across the top of the window: the current mode's name in
///        large letters on the mode's colour, the step list's Details checkbox
///        and the three mode buttons.
///
/// Into any mode from Edit or Rehearsal: one tap. Out of Show mode: the press
/// on another mode's button must last kHoldMs while a bar fills; letting go,
/// dragging off the button or a second touch cancels (design 4). No dialog:
/// nothing may block the operator during a show.
#pragma once

#include <array>
#include <functional>

#include <juce_gui_basics/juce_gui_basics.h>

#include "Mode.h"

namespace bigtop::app {

/// @brief How far a hold-to-leave press is, from 0 to 1.
/// @param elapsedMs Milliseconds since the press began.
/// @param holdMs The hold time that completes it (ModeBar::kHoldMs).
/// @return Fraction done, clamped to [0, 1]; 1 means the mode may change.
constexpr float holdProgress(juce::uint32 elapsedMs, juce::uint32 holdMs) noexcept {
    return elapsedMs >= holdMs ? 1.0f : static_cast<float>(elapsedMs) / static_cast<float>(holdMs);
}

static_assert(holdProgress(0, 2000) == 0.0f && holdProgress(1000, 2000) == 0.5f);
static_assert(holdProgress(1999, 2000) < 1.0f && holdProgress(2000, 2000) == 1.0f && holdProgress(9000, 2000) == 1.0f);

/// @brief The mode strip.
/// @note Message thread. Never takes keyboard focus (the main component keeps
///       Space/Esc/Up/Down).
class ModeBar : public juce::Component, private juce::Timer {
public:
    /// @brief Height the owner should give the strip, in px (buttons are 48 px high).
    static constexpr int kHeight = 56;
    /// @brief How long a press must last to leave Show mode, in ms.
    static constexpr juce::uint32 kHoldMs = 2000;

    /// @brief Builds the strip, in Edit mode.
    ModeBar();

    /// @brief Shows which mode is current.
    /// @param mode The session's mode; repaints in its colour. A hold under way
    ///        is cancelled when the mode changed.
    void setMode(ShowMode mode);

    /// @brief Screenshot/testing hook: draws a hold-to-leave press as if it were @p fraction done.
    /// @param target The button the imagined press is on.
    /// @param fraction 0 (or less) cancels it; otherwise the share of kHoldMs already held.
    /// @note No timer runs, so the hold never completes and never calls onPick; call with 0 to clear it.
    void showHoldProgress(ShowMode target, float fraction);

    /// @brief Called when a mode is picked: at once (on release) for a tap,
    ///        after kHoldMs for a hold out of Show mode. Never called for the
    ///        current mode. The owner may refuse (the strip then keeps
    ///        showing the old mode).
    std::function<void(ShowMode)> onPick;

    /// @brief Called with the new state when the Details checkbox is ticked or unticked (any mode).
    std::function<void(bool)> onDetails;

    /// @brief Shows the Details checkbox ticked or not, without calling onDetails.
    /// @param on True for ticked.
    void setDetails(bool on);

    /// @brief Paints the coloured strip and the mode name (JUCE callback).
    /// @param g Graphics context.
    void paint(juce::Graphics& g) override;

    /// @brief Lays out the three buttons at the right edge and the Details checkbox left of them (JUCE callback).
    void resized() override;

private:
    /// App test hook (app/tests/AppTestSupport.h): the mode buttons, the hold timer and the Details checkbox.
    friend struct AppTestAccess;

    /// One of the three buttons; it only reports presses to the bar.
    class Button : public juce::Component {
    public:
        Button(ModeBar& bar, ShowMode mode) : bar_(bar), mode_(mode) { setWantsKeyboardFocus(false); }
        void paint(juce::Graphics& g) override;
        void mouseDown(const juce::MouseEvent& e) override { bar_.pressed(mode_, e); }
        void mouseDrag(const juce::MouseEvent& e) override { bar_.dragged(mode_, e); }
        void mouseUp(const juce::MouseEvent& e) override { bar_.released(mode_, e); }

    private:
        ModeBar& bar_;
        ShowMode mode_;
    };

    void pressed(ShowMode target, const juce::MouseEvent& e);
    void dragged(ShowMode target, const juce::MouseEvent& e);
    void released(ShowMode target, const juce::MouseEvent& e);
    void cancelHold();
    void timerCallback() override;
    float progressOf(ShowMode target) const;

    ShowMode mode_ = ShowMode::Edit;
    // A hold is under way when holding_ is set: which button, which pointer
    // (mouse or finger) started it, and when. A second pointer cancels it.
    bool holding_ = false;
    ShowMode holdTarget_ = ShowMode::Edit;
    int holdSource_ = 0;
    juce::uint32 holdStartMs_ = 0;
    // A tap (not a hold) picks on release inside the button, like a normal button.
    bool tapPending_ = false;
    juce::ToggleButton details_{"Details"};  // the step list's Details lines; a viewing preference, any mode
    std::array<Button, 3> buttons_{Button(*this, ShowMode::Edit), Button(*this, ShowMode::Rehearsal),
                                   Button(*this, ShowMode::Show)};
};

}  // namespace bigtop::app
