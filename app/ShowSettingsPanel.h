/// @file ShowSettingsPanel.h
/// @brief The side panel with the show-wide settings (design 6, 8 and 9):
///        default fade time, Art-Net target address, network adapter, timecode
///        frame rate and the pad grid.
///
/// Edit mode only (Action::ShowSettings). Every field commits like the step
/// editor's: on Enter, on focus loss and on each tap, one verified save per
/// change, values that fail the checks put the stored value back. The adapter
/// is machine-specific, so it goes to the app settings, not into the show.
#pragma once

#include <functional>
#include <memory>
#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

#include "NumberField.h"
#include "ShowSession.h"

namespace bigtop::app {

/// @brief The show settings panel.
/// @note Message thread.
class ShowSettingsPanel : public juce::Component {
public:
    /// @brief Builds a closed (hidden) panel.
    /// @param session The session whose open show is edited; must outlive this.
    explicit ShowSettingsPanel(ShowSession& session);

    /// @brief Destroys the fields.
    ~ShowSettingsPanel() override;

    /// @brief Shows the panel; lists the network adapters now.
    void open();

    /// @brief Updates values that are not being typed and the Art-Net status text;
    ///        call from the 30 Hz UI timer.
    void refresh();

    /// @brief Called when Close is tapped.
    std::function<void()> onClose;
    /// @brief Called when Show check is tapped (next to Close).
    std::function<void()> onShowCheck;

    /// @brief Paints the panel background (JUCE callback).
    /// @param g Graphics context.
    void paint(juce::Graphics& g) override;
    /// @brief Lays out the title bar and the scrolling fields (JUCE callback).
    void resized() override;

private:
    /// App test hook (app/tests/AppTestSupport.h): reads the viewport for the scroll check.
    friend struct AppTestAccess;

    void rebuild();
    int contentWidth() const;  // the fields' width: the viewport less a scroll bar, shown or not

    ShowSession& session_;
    juce::Label title_;
    juce::TextButton close_{"Close"};
    juce::TextButton showCheck_{"Show check"};  // the check also runs on its own, so it need not be always visible
    juce::Viewport view_;
    juce::Component content_;
    std::vector<std::unique_ptr<juce::Component>> owned_;
    std::vector<std::function<void(bool)>> pulls_;  // re-read one field; bool = even if being typed
    juce::Label* status_ = nullptr;  // the sender status text, owned by owned_
    int builtWidth_ = -1;
};

}  // namespace bigtop::app
