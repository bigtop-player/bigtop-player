/// @file ModeFrame.h
/// @brief The mode-coloured frame around the GO button.
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace bigtop::app {

/// @brief A rounded frame in the mode's colour, laid out kWidth px around GO.
///
/// A child of its own rather than painted by the window: when a mode change
/// moves GO, JUCE repaints the frame's old and new place, so no old stroke is
/// left on the buttons that take the space.
/// @note Message thread. It takes no clicks.
class ModeFrame : public juce::Component {
public:
    /// @brief How far the frame reaches outside GO, in px; less than the 8 px gap to its neighbours.
    static constexpr int kWidth = 4;

    ModeFrame() { setInterceptsMouseClicks(false, false); }

    /// @brief Sets the frame's colour, repainting only when it changes.
    /// @param c The mode's colour.
    void setColour(juce::Colour c) {
        if (c == colour_) return;
        colour_ = c;
        repaint();
    }

    /// @brief The colour the frame is drawn in.
    /// @return The last colour given to setColour().
    juce::Colour colour() const noexcept { return colour_; }

    /// @brief Draws the frame (JUCE callback).
    /// @param g Graphics context.
    void paint(juce::Graphics& g) override {
        g.setColour(colour_);
        const float half = static_cast<float>(kWidth) / 2.0f;
        g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(half), 6.0f, static_cast<float>(kWidth));
    }

private:
    juce::Colour colour_;
};

}  // namespace bigtop::app
