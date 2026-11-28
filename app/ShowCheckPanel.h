#pragma once
/// @file ShowCheckPanel.h
/// @brief The Show check: every problem the show has right now, one line each,
///        so go/no-go is one look instead of a walk through the list.

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <vector>

namespace bigtop::app {

/// @brief A panel over the step list that lists problems, or says "All clear".
/// @note Changes nothing in the show, so every mode may open it. A tap closes
///       it. Takes no keyboard focus, so GO keeps working. Message thread.
class ShowCheckPanel : public juce::Component {
public:
    /// @brief Builds an empty, focus-free panel.
    ShowCheckPanel();

    /// @brief Sets what the panel lists.
    /// @param lines One problem per line (ShowSession::checkReport()); empty shows "All clear".
    void setLines(const std::vector<juce::String>& lines);

    /// @brief The text shown, lines joined by newlines.
    /// @return "All clear" or the problems.
    juce::String shownText() const;

    std::function<void()> onClose;  ///< @brief A tap anywhere on the panel.

    /// @brief Draws the panel.
    /// @param g Graphics context.
    void paint(juce::Graphics& g) override;
    /// @brief A tap closes the panel.
    /// @param e The mouse event.
    void mouseUp(const juce::MouseEvent& e) override;

private:
    std::vector<juce::String> lines_;
};

}  // namespace bigtop::app
