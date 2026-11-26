// ModeBar.cpp: the mode strip and the hold-to-leave press.
#include "ModeBar.h"

#include "Pointer.h"
#include "UiClock.h"

namespace bigtop::app {

namespace {
constexpr int kButtonWidth = 150;
constexpr int kButtonHeight = 48;  // touch target minimum
constexpr int kHoldHz = 30;        // repaint rate of the filling bar
constexpr int kDetailsWidth = 120;  // the Details checkbox, its whole height the touch target
constexpr int kDetailsGap = 32;     // between it and the mode buttons
const juce::Colour kDark(0xff1b1b1b);
}  // namespace

ModeBar::ModeBar() {
    for (auto& b : buttons_) addAndMakeVisible(b);
    // Dark text and tick, like the mode name: readable on every mode colour.
    details_.setColour(juce::ToggleButton::textColourId, kDark);
    details_.setColour(juce::ToggleButton::tickColourId, kDark);
    details_.setColour(juce::ToggleButton::tickDisabledColourId, kDark);
    details_.setWantsKeyboardFocus(false);  // the main component keeps Space/Esc/Up/Down
    details_.onClick = [this] {
        if (onDetails) onDetails(details_.getToggleState());
    };
    addAndMakeVisible(details_);
}

void ModeBar::setDetails(bool on) { details_.setToggleState(on, juce::dontSendNotification); }

void ModeBar::setMode(ShowMode mode) {
    if (mode == mode_) return;
    mode_ = mode;
    // The change happened (by this hold or otherwise): nothing is left to hold for.
    cancelHold();
    repaint();
    for (auto& b : buttons_) b.repaint();
}

void ModeBar::showHoldProgress(ShowMode target, float fraction) {
    if (fraction <= 0.0f) {
        cancelHold();
        return;
    }
    holding_ = true;
    holdTarget_ = target;
    holdSource_ = 0;
    // Back-dated start, so progressOf() reads about `fraction` when the strip is painted next.
    holdStartMs_ = uiMs() - static_cast<juce::uint32>(fraction * static_cast<float>(kHoldMs));
    for (auto& b : buttons_) b.repaint();
}

void ModeBar::paint(juce::Graphics& g) {
    // The whole strip is in the mode colour (amber / blue / green): the mode is obvious at a glance.
    g.fillAll(juce::Colour(modeColour(mode_)));
    g.setColour(kDark);
    g.setFont(juce::FontOptions(30.0f, juce::Font::bold));
    g.drawText(modeName(mode_), getLocalBounds().withTrimmedLeft(16), juce::Justification::centredLeft);
}

void ModeBar::resized() {
    auto r = getLocalBounds().withSizeKeepingCentre(getWidth(), kButtonHeight).reduced(8, 0);
    for (int i = 2; i >= 0; --i) buttons_[static_cast<size_t>(i)].setBounds(r.removeFromRight(kButtonWidth).reduced(3, 0));
    // A clear gap before the mode buttons, so a tap on Details never lands on a hold-to-leave button.
    r.removeFromRight(kDetailsGap);
    details_.setBounds(r.removeFromRight(kDetailsWidth));
}

float ModeBar::progressOf(ShowMode target) const {
    if (!holding_ || holdTarget_ != target) return 0.0f;
    return holdProgress(uiMs() - holdStartMs_, kHoldMs);
}

void ModeBar::Button::paint(juce::Graphics& g) {
    const auto area = getLocalBounds().toFloat();
    const juce::Colour colour(modeColour(bar_.mode_));  // the strip's colour: the current mode's
    const bool current = bar_.mode_ == mode_;
    // The strip is in the current mode's colour, so the current button is inverted: dark fill, mode-colour
    // text and border. The other two are tinted darker than the strip with dark text, so it stands out from them.
    g.setColour(current ? kDark : colour.darker(0.25f));
    g.fillRoundedRectangle(area, 8.0f);
    // The filling bar of a hold: grows left to right along the button's bottom edge, the label stays readable.
    const float progress = bar_.progressOf(mode_);
    if (progress > 0.0f) {
        // Clipped to the button's rounded shape, so the bar never pokes out at the corners.
        const juce::Graphics::ScopedSaveState keep(g);
        juce::Path shape;
        shape.addRoundedRectangle(area, 8.0f);
        g.reduceClipRegion(shape);
        g.setColour(current ? colour : kDark);
        g.fillRect(area.withTop(area.getBottom() - 6.0f).withWidth(area.getWidth() * progress));
    }
    g.setColour(current ? colour : kDark);
    g.drawRoundedRectangle(area.reduced(1.0f), 8.0f, 2.0f);
    g.setColour(current ? colour : kDark);
    g.setFont(juce::FontOptions(18.0f, juce::Font::bold));
    // In Show mode the others say how to leave, so the 2 s hold is discoverable.
    const bool needsHold = bar_.mode_ == ShowMode::Show && !current;
    g.drawText(modeName(mode_), getLocalBounds().withTrimmedBottom(needsHold ? 14 : 0), juce::Justification::centred);
    if (needsHold) {
        g.setFont(juce::FontOptions(12.0f));
        g.drawText("hold 2 s", getLocalBounds().withTrimmedTop(26), juce::Justification::centred);
    }
}

void ModeBar::pressed(ShowMode target, const juce::MouseEvent& e) {
    // A second pointer while a hold runs is a stray touch (a palm, a second
    // finger): cancel rather than risk leaving Show mode by accident.
    if (holding_ && pointerIndex(e) != holdSource_) {
        cancelHold();
        return;
    }
    if (target == mode_) return;
    if (mode_ != ShowMode::Show) {
        tapPending_ = true;  // one tap: decided on release inside the button
        return;
    }
    holding_ = true;
    holdTarget_ = target;
    holdSource_ = pointerIndex(e);
    holdStartMs_ = uiMs();
    startTimerHz(kHoldHz);
    for (auto& b : buttons_) b.repaint();
}

void ModeBar::dragged(ShowMode target, const juce::MouseEvent& e) {
    // Dragging off the button it started on cancels (JUCE keeps sending drags to it).
    if (holding_ && target == holdTarget_ && !buttons_[static_cast<size_t>(target)].contains(e.getPosition())) cancelHold();
}

void ModeBar::released(ShowMode target, const juce::MouseEvent& e) {
    if (tapPending_) {
        tapPending_ = false;
        if (buttons_[static_cast<size_t>(target)].contains(e.getPosition()) && target != mode_ && onPick) onPick(target);
        return;
    }
    // Let go before the time is up: nothing happens (a completed hold already cleared holding_).
    if (holding_ && pointerIndex(e) == holdSource_) cancelHold();
}

void ModeBar::cancelHold() {
    tapPending_ = false;
    if (!holding_) return;
    holding_ = false;
    stopTimer();
    for (auto& b : buttons_) b.repaint();
}

void ModeBar::timerCallback() {
    // Also checked while filling, so a lost release cancels at once.
    if (holding_ && !pointerDown(buttons_[static_cast<size_t>(holdTarget_)])) {
        cancelHold();
        return;
    }
    if (progressOf(holdTarget_) < 1.0f) {
        for (auto& b : buttons_) b.repaint();
        return;
    }
    // A lost mouse-up (window deactivated, capture lost) must not leave Show
    // mode: the finger has to still be down on the target button.
    const auto target = holdTarget_;
    if (!pointerDown(buttons_[static_cast<size_t>(target)])) {
        cancelHold();
        return;
    }
    // Held long enough: change the mode now, without waiting for the release.
    cancelHold();
    if (onPick) onPick(target);
}

}  // namespace bigtop::app
