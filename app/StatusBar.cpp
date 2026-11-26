// StatusBar.cpp: colours carry the meaning (red = needs attention).
#include "StatusBar.h"

namespace bigtop::app {

namespace {

const juce::Colour kOk = juce::Colours::lightgreen;
const juce::Colour kBad = juce::Colours::red;
const juce::Colour kDim = juce::Colours::grey;
constexpr int kArtNetWidth = 140;  // wide enough for "Art-Net: sending"

void setLabel(juce::Label& l, const juce::String& text, juce::Colour colour) {
    l.setText(text, juce::dontSendNotification);
    l.setColour(juce::Label::textColourId, colour);
}

}  // namespace

StatusBar::StatusBar() {
    for (auto* l : {&audio_, &dropouts_, &save_, &showName_, &message_}) {
        l->setJustificationType(juce::Justification::centredLeft);
        addAndMakeVisible(l);
    }
    clip_.setWantsKeyboardFocus(false);
    clip_.onClick = [this] { if (onClipTapped) onClipTapped(); };
    addAndMakeVisible(clip_);
    settings_.onClick = [this] { if (onSettings) onSettings(); };
    shows_.onClick = [this] { if (onShows) onShows(); };
    showSettings_.onClick = [this] { if (onShowSettings) onShowSettings(); };
    // Focus stays with the main component, which owns Space/Esc/Up/Down.
    for (auto* b : {&settings_, &showSettings_, &shows_}) {
        b->setWantsKeyboardFocus(false);
        addAndMakeVisible(b);
    }
    artNet_.setJustificationType(juce::Justification::centredLeft);
    addAndMakeVisible(artNet_);
    meter_.setInterceptsMouseClicks(false, false);
    addAndMakeVisible(meter_);
}

void StatusBar::update(const ShowSession::Status& s, bool showOpen, ShowMode mode, const io::ArtNetStatus& artNet) {
    setLabel(audio_, s.audioText, s.audioOk ? kOk : kBad);
    // The light says the state; anything unusual in the sender text (an adapter
    // that is missing, a failed send) is the tooltip; the colour comes from the state only.
    using State = io::ArtNetStatus::State;
    setLabel(artNet_,
             artNet.state == State::Sending ? "Art-Net: sending" : artNet.state == State::Error ? "Art-Net: error" : "Art-Net: idle",
             artNet.state == State::Error ? kBad : artNet.state == State::Sending ? kOk : kDim);
    artNet_.setTooltip(artNet.text);
    // CLIP is always there, dim when quiet, so a lit one is noticed.
    clip_.setColour(juce::TextButton::textColourOffId, s.clip ? juce::Colours::white : kDim);
    clip_.setColour(juce::TextButton::buttonColourId, s.clip ? kBad : juce::Colours::transparentBlack);
    // Both are red and never covered by a notice; they share one label.
    juce::StringArray problems;
    if (s.dropouts > 0) problems.add("dropouts: " + juce::String(s.dropouts));
    if (s.droppedStarts > 0) problems.add("a start or seek was dropped (" + juce::String(s.droppedStarts) + ")");
    setLabel(dropouts_, problems.joinIntoString("  "), kBad);
    dropoutsWidth_ = (s.dropouts > 0 ? 120 : 0) + (s.droppedStarts > 0 ? 330 : 0);
    setLabel(save_, s.saveText, s.saveOk ? kOk : kBad);
    setLabel(showName_, s.showName, juce::Colours::white);
    setLabel(message_, s.message, juce::Colours::orange);
    notice_ = s.message.isNotEmpty();
    dropoutsShown_ = !problems.isEmpty();
    saveError_ = !s.saveOk;
    layoutMiddle();
    // Settings and "Shows..." are Edit-mode actions; with no show open there is
    // nothing to protect and the start panel needs the device settings. A mode
    // that refuses one hides it (as Pause and Keep are hidden), so the show
    // screen has nothing to tap that does nothing; resized() keeps the gap.
    settings_.setVisible(!showOpen || allowed(mode, Action::AudioSettings));
    showSettings_.setVisible(showOpen && allowed(mode, Action::ShowSettings));
    shows_.setVisible(showOpen && allowed(mode, Action::SwitchShow));  // with no show the start panel offers New / Open
}

void StatusBar::resized() {
    auto r = getLocalBounds();
    settings_.setBounds(r.removeFromRight(120).reduced(2));
    shows_.setBounds(r.removeFromRight(110).reduced(2));
    showSettings_.setBounds(r.removeFromRight(130).reduced(2));
    audio_.setBounds(r.removeFromLeft(260));
    clip_.setBounds(r.removeFromLeft(64).reduced(4, 2));  // 48 px tall: a touch target
    meter_.setBounds(r.removeFromLeft(260).reduced(4, 0));  // wide enough for segments and a readable dB scale
    artNet_.setBounds(r.removeFromLeft(kArtNetWidth));
    middle_ = r;
    layoutMiddle();
}

void StatusBar::layoutMiddle() {
    // A notice answers what the operator just pressed and takes the space of
    // what can wait 3 s (show name, a calm save time, no dropouts); a red save
    // error and a dropout count are never covered.
    auto r = middle_;
    const bool dropouts = !notice_ || dropoutsShown_;
    const bool save = !notice_ || saveError_;
    dropouts_.setVisible(dropouts);
    if (dropouts) dropouts_.setBounds(r.removeFromLeft(dropoutsWidth_));
    save_.setVisible(save);
    if (save) save_.setBounds(r.removeFromLeft(juce::jmax(0, r.getWidth() / 2)));
    message_.setVisible(notice_);
    showName_.setVisible(!notice_);
    message_.setBounds(r);
    showName_.setBounds(r);
}

}  // namespace bigtop::app
