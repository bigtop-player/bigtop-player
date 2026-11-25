/// @file SettingsWindow.h
/// @brief The audio device settings: JUCE's ready-made device selector in a
///        separate, non-modal window. The device chosen when it closes
///        becomes the desired device and is stored in the app settings.
#pragma once

#include <functional>

#include <juce_audio_utils/juce_audio_utils.h>

#include "ShowSession.h"

namespace bigtop::app {

/// @brief Non-modal window with an output-only, stereo device selector.
/// @note Message thread. While it exists the session's device retry is
///       paused; destroying it makes the current device the desired one.
class SettingsWindow : public juce::DocumentWindow {
public:
    /// @brief Opens the window.
    /// @param devices The app's device manager (the selector changes it directly).
    /// @param session Session that adopts the choice; must outlive the window.
    /// @param onClose Called when the user closes the window; the owner then
    ///        deletes it, asynchronously (not from inside this call).
    SettingsWindow(juce::AudioDeviceManager& devices, ShowSession& session, std::function<void()> onClose);

    /// @brief Adopts the final choice and resumes the device retry, unless
    ///        closeButtonPressed() already did.
    ~SettingsWindow() override;

    /// @brief The title bar's close button (JUCE callback): hides the window,
    ///        reports the choice to the session, then calls onClose.
    void closeButtonPressed() override;

private:
    ShowSession& session_;
    std::function<void()> onClose_;
};

}  // namespace bigtop::app
