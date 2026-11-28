// AboutPanel.cpp: see AboutPanel.h.
#include "AboutPanel.h"

namespace bigtop::app {

namespace {
constexpr const char* kProjectUrl = "https://github.com/bigtop-player/bigtop-player";
}  // namespace

AboutPanel::AboutPanel() : link_(kProjectUrl, juce::URL(kProjectUrl)) {
    lines_.setText(juce::String("Bigtop Player, Version ") + BIGTOP_VERSION +
                       "\nGNU Affero General Public License v3\nBuilt with JUCE, r8brain-free-src and nlohmann/json",
                   juce::dontSendNotification);
    lines_.setJustificationType(juce::Justification::topLeft);
    link_.setJustificationType(juce::Justification::centredLeft);
    addAndMakeVisible(lines_);
    addAndMakeVisible(link_);
}

juce::String AboutPanel::text() const { return lines_.getText() + "\n" + link_.getButtonText(); }

void AboutPanel::resized() {
    auto r = getLocalBounds().reduced(8, 4);
    link_.setBounds(r.removeFromBottom(24));
    lines_.setBounds(r);
}

}  // namespace bigtop::app
