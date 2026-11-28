#pragma once
/// @file AboutPanel.h
/// @brief Name, version, licence and project page, at the bottom of Settings,
///        so a bug report can say exactly which build it is about.

#include <juce_gui_basics/juce_gui_basics.h>

namespace bigtop::app {

/// @brief The About lines; the project link opens in the browser.
/// @note Message thread.
class AboutPanel : public juce::Component {
public:
    /// @brief Height the owner should give the panel, in px.
    static constexpr int kHeight = 96;

    /// @brief Builds the text and the link.
    AboutPanel();

    /// @brief Everything the panel says, one line each.
    /// @return Name and version, licence, project page, credits.
    juce::String text() const;

    /// @brief Lays out the text and the link (JUCE callback).
    void resized() override;

private:
    juce::Label lines_;
    juce::HyperlinkButton link_;
};

}  // namespace bigtop::app
