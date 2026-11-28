#pragma once
/// @file AlertBanner.h
/// @brief The banner over the top of the step list for serious problems
///        (missing file, failed import, lost device, Ontime): large enough to
///        be read from a step away, and never in the way of GO or the keys.

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace bigtop::app {

/// @brief A tappable banner: tap the text to dismiss it, tap the hint to open Show check.
/// @note Takes no keyboard focus, so Space and Esc keep reaching the main
///       screen while it shows. Message thread.
class AlertBanner : public juce::Component {
public:
    /// @brief Smallest height that keeps the text readable, in px.
    static constexpr int kHeight = 56;

    /// @brief Sets up a banner that never takes keyboard focus.
    AlertBanner();

    /// @brief Shows a problem.
    /// @param text What went wrong, one line.
    /// @param hint Empty, or the "+N more" line that leads to Show check.
    /// @param warning True draws it amber (a problem that leaves playback alone), else red.
    void show(const juce::String& text, const juce::String& hint, bool warning);

    /// @brief The text shown. @return The last show() text.
    const juce::String& text() const noexcept { return text_; }
    /// @brief The hint shown. @return The last show() hint.
    const juce::String& hint() const noexcept { return hint_; }

    std::function<void()> onDismiss;    ///< @brief A tap on the text.
    std::function<void()> onShowCheck;  ///< @brief A tap on the hint (when one is shown).

    /// @brief Draws the banner.
    /// @param g Graphics context.
    void paint(juce::Graphics& g) override;
    /// @brief A tap: on the hint opens Show check, anywhere else dismisses.
    /// @param e The mouse event.
    void mouseUp(const juce::MouseEvent& e) override;

private:
    juce::Rectangle<int> hintArea() const;

    juce::String text_, hint_;
    bool warning_ = false;
};

}  // namespace bigtop::app
