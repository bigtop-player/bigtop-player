/// @file StatusBar.h
/// @brief The bottom bar: audio device state, CLIP light, dropouts, save
///        state, show name, a notice when a start or seek was dropped, the
///        Art-Net light, the settings button, "Show settings" and "Shows..."
///        (switch to another show). The mode buttons live in the ModeBar.
#pragma once

#include <functional>

#include <juce_gui_basics/juce_gui_basics.h>

#include "bigtop/io/ArtNetSender.h"
#include "LevelMeter.h"
#include "ShowSession.h"

namespace bigtop::app {

/// @brief Displays a ShowSession::Status; the buttons report through callbacks.
/// @note Message thread.
class StatusBar : public juce::Component {
    friend struct AppTestAccess;

public:
    /// @brief Height the owner should give the bar, in px: 48 px buttons plus a 2 px margin.
    static constexpr int kHeight = 52;

    /// @brief Builds the bar, empty until the first update().
    StatusBar();

    /// @brief Shows a new status.
    /// @param status From ShowSession::status().
    /// @param showOpen Whether a show is open ("Shows..." is hidden otherwise).
    /// @param mode The current mode; Settings, "Show settings" and "Shows..." are
    ///        shown only where the permission table allows them (Edit mode), or
    ///        with no show (Settings); elsewhere they are hidden and leave a gap.
    /// @param artNet The Art-Net sender status: green "Art-Net: sending", grey
    ///        "Art-Net: idle", red "Art-Net: error"; the full text (the reason) is the tooltip.
    /// @param ontime The Ontime link: hidden when Off, grey "Ontime: checking", green "Ontime: connected", red "Ontime: error".
    /// @param ontimeError The last Ontime error, shown as the light's tooltip.
    void update(const ShowSession::Status& status, bool showOpen, ShowMode mode, const io::ArtNetStatus& artNet,
                io::OntimeClient::State ontime = io::OntimeClient::State::Off, const juce::String& ontimeError = {});

    /// @brief Called when the settings button is pressed (only in Edit mode or with no show).
    std::function<void()> onSettings;
    /// @brief Called when "Show settings" is pressed (shown only with a show open, in Edit mode).
    std::function<void()> onShowSettings;
    /// @brief Called when "Shows..." is pressed (shown only with a show open, in Edit mode).
    std::function<void()> onShows;

    /// @brief Feeds the level meter (call on the UI timer).
    /// @param peaks Output peaks since the previous call (ShowSession::outputPeaks()).
    /// @param nowMs The UI clock, for the meter's peak hold.
    void setPeaks(io::OutputPeaks peaks, juce::uint32 nowMs) { meter_.setPeaks(peaks, nowMs); }
    /// @brief Called when the CLIP light is tapped (any mode): the owner resets it.
    std::function<void()> onClipTapped;
    /// @brief Lays out the fields (JUCE callback).
    void resized() override;

private:
    void layoutMiddle();

    juce::Label audio_, artNet_, ontime_, dropouts_, save_, showName_, message_;
    juce::TextButton clip_{"CLIP"};  // a light that a tap resets, as in a DAW
    LevelMeter meter_;               // next to CLIP: how close the output is to it
    juce::Rectangle<int> middle_;  // between the Art-Net light and the buttons
    bool notice_ = false, dropoutsShown_ = false, saveError_ = false;
    int dropoutsWidth_ = 0;  // wide enough for the dropouts count and/or the dropped-start text
    juce::TextButton settings_{"Settings"}, showSettings_{"Show settings"}, shows_{juce::CharPointer_UTF8("Shows\xe2\x80\xa6")};
};

}  // namespace bigtop::app
