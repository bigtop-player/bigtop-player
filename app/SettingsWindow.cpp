// SettingsWindow.cpp: the selector edits the device manager itself; this
// window only tells the session when to take the result (on close, so a
// device list change while it is open is never mistaken for a choice).
#include "SettingsWindow.h"

namespace bigtop::app {

SettingsWindow::SettingsWindow(juce::AudioDeviceManager& devices, ShowSession& session, std::function<void()> onClose)
    : DocumentWindow("Audio settings", juce::Colours::black, DocumentWindow::closeButton),
      session_(session),
      onClose_(std::move(onClose)) {
    session_.setDeviceSettingsOpen(true);
    // No inputs, exactly two outputs, channels as stereo pairs, no MIDI.
    auto selector = std::make_unique<juce::AudioDeviceSelectorComponent>(devices, 0, 0, 2, 2, false, false, true, false);
    selector->setSize(560, 420);
    setContentOwned(selector.release(), true);
    setUsingNativeTitleBar(true);
    setResizable(true, false);
    centreWithSize(getWidth(), getHeight());
    setVisible(true);
}

SettingsWindow::~SettingsWindow() {
    session_.setDeviceSettingsOpen(false);  // no-op if the close button already did it
}

void SettingsWindow::closeButtonPressed() {
    // Report the choice now, not when the owner deletes us later: a window
    // reopened in between must not be closed out by this one's late report.
    setVisible(false);
    session_.setDeviceSettingsOpen(false);
    onClose_();
}

}  // namespace bigtop::app
