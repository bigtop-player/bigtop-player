// ShowCheckPanel.cpp: see ShowCheckPanel.h.
#include "ShowCheckPanel.h"

namespace bigtop::app {

ShowCheckPanel::ShowCheckPanel() {
    setWantsKeyboardFocus(false);
    setMouseClickGrabsKeyboardFocus(false);
}

void ShowCheckPanel::setLines(const std::vector<juce::String>& lines) {
    if (lines == lines_) return;
    lines_ = lines;
    repaint();
}

juce::String ShowCheckPanel::shownText() const {
    if (lines_.empty()) return "All clear";
    juce::StringArray a;
    for (const auto& l : lines_) a.add(l);
    return a.joinIntoString("\n");
}

void ShowCheckPanel::paint(juce::Graphics& g) {
    g.fillAll(juce::Colour(0xf0202020u));
    auto r = getLocalBounds().reduced(16);
    g.setColour(juce::Colours::white);
    g.setFont(juce::FontOptions(22.0f, juce::Font::bold));
    g.drawText("Show check", r.removeFromTop(32), juce::Justification::centredLeft);
    g.setFont(juce::FontOptions(14.0f));
    g.setColour(juce::Colours::lightgrey);
    g.drawText("Tap to close", r.removeFromBottom(20), juce::Justification::centredRight);
    g.setFont(juce::FontOptions(18.0f));
    if (lines_.empty()) {
        g.setColour(juce::Colour(0xff3fb950u));
        g.drawText("All clear", r.removeFromTop(28), juce::Justification::centredLeft);
        return;
    }
    g.setColour(juce::Colour(0xffff7b72u));
    for (const auto& l : lines_) {
        if (r.getHeight() < 24) break;  // more than fits: the banner and rows still show the rest
        g.drawFittedText(l, r.removeFromTop(26), juce::Justification::centredLeft, 1);
    }
}

void ShowCheckPanel::mouseUp(const juce::MouseEvent&) {
    if (onClose) onClose();
}

}  // namespace bigtop::app
