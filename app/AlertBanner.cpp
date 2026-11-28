// AlertBanner.cpp: see AlertBanner.h.
#include "AlertBanner.h"

namespace bigtop::app {

AlertBanner::AlertBanner() {
    // Space and Esc must keep working while a problem is shown.
    setWantsKeyboardFocus(false);
    setMouseClickGrabsKeyboardFocus(false);
}

void AlertBanner::show(const juce::String& text, const juce::String& hint, bool warning) {
    if (text == text_ && hint == hint_ && warning == warning_) return;
    text_ = text;
    hint_ = hint;
    warning_ = warning;
    repaint();
}

juce::Rectangle<int> AlertBanner::hintArea() const {
    if (hint_.isEmpty()) return {};
    return getLocalBounds().removeFromRight(juce::jmin(220, getWidth() / 3));
}

void AlertBanner::paint(juce::Graphics& g) {
    g.fillAll(juce::Colour(warning_ ? 0xffb36b00u : 0xffb3261eu));
    g.setColour(juce::Colours::white);
    auto r = getLocalBounds().reduced(12, 4);
    const auto hint = hintArea();
    if (!hint.isEmpty()) {
        g.setFont(juce::FontOptions(16.0f));
        g.drawFittedText(hint_, hint.reduced(8, 4), juce::Justification::centredRight, 2);
        r.removeFromRight(hint.getWidth());
    }
    g.setFont(juce::FontOptions(20.0f, juce::Font::bold));
    g.drawFittedText(text_, r, juce::Justification::centredLeft, 2);
}

void AlertBanner::mouseUp(const juce::MouseEvent& e) {
    if (hintArea().contains(e.getPosition()) && onShowCheck) onShowCheck();
    else if (onDismiss) onDismiss();
}

}  // namespace bigtop::app
