/// @file MainComponent.h
/// @brief The main window's content: the mode strip, the start panel or the GO screen (running
///        sounds, step list, GO and Fade out), the status bar below, the 30 Hz
///        UI timer that also drives the 4 Hz housekeeping, the side area (the pad
///        grid, replaced by the step editor, the pad editor or the show settings), the operator keys
///        (Delete removes the next step in Edit mode), the settings window, and
///        "Shows..." (the start panel over an open show, to switch shows).
#pragma once

#include <memory>

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "AlertBanner.h"
#include "ShowCheckPanel.h"
#include "ModeBar.h"
#include "ModeFrame.h"
#include "PadEditor.h"
#include "PadGrid.h"
#include "RunningSounds.h"
#include "SettingsWindow.h"
#include "ShowSession.h"
#include "ShowSettingsPanel.h"
#include "StartPanel.h"
#include "StatusBar.h"
#include "StepEditor.h"
#include "StepList.h"

namespace bigtop::app {

/// @brief Owns the session and switches between the start panel and the GO screen.
/// @note Message thread. While a show is open this component keeps the
///       keyboard focus (no child wants it), so Space = GO, Esc = Fade out and
///       Up/Down = select next work whatever was clicked last. It is the
///       drag container for the step list's row drags.
class MainComponent : public juce::Component, public juce::DragAndDropContainer, private juce::Timer {
public:
    /// @brief Builds the session and reopens the last show if it still opens.
    /// @param settings App settings; must outlive this component.
    /// @param devices The app's device manager; must outlive this component.
    /// @note Otherwise the start panel is shown, with the error and restore
    ///       offer when the last show's show.json failed. Nothing plays.
    /// @param offline Null in the app; the screenshot mode passes invented data (see ShowSession::Offline).
    MainComponent(io::AppSettings& settings, juce::AudioDeviceManager& devices, const ShowSession::Offline* offline = nullptr);

    /// @brief Closes the settings window before the session goes.
    ~MainComponent() override;

    /// @brief The window's close button or Alt+F4.
    /// @return True when the app may quit now. In Show mode it returns false and
    ///         asks "Quit Bigtop Player during the show?" (Cancel is the default);
    ///         Quit then calls the quit handler (JUCEApplication::quit in the app).
    bool quitRequested();

    /// @brief The window title: "Bigtop Player – <show name>", or "Bigtop Player" with no show.
    /// @return The title.
    juce::String windowTitle() const;

    /// @brief Lays out the screen and the status bar (JUCE callback).
    void resized() override;

    /// @brief The operator keys: Space, Esc, P (pause / resume), Up, Down, Delete (JUCE callback).
    /// @param key The key pressed.
    /// @return True when the key was one of them and a show is open.
    /// @note A held Space or Esc acts once: key repeat must not run steps or
    ///       turn a fade into a stop; P likewise acts once per press.
    bool keyPressed(const juce::KeyPress& key) override;

    /// @brief Ends a held Space or Esc on release (JUCE callback).
    /// @param isKeyDown Whether a key went down (ignored; the keys are polled).
    /// @return False, so the event goes on to other listeners.
    bool keyStateChanged(bool isKeyDown) override;

    /// @brief Draws the GO button's border in the current mode's colour (JUCE callback).
    /// @param g Graphics context; painted over the children so the border is never hidden.

private:
    /// Screenshot/testing hook (Screenshots.cpp): drives the same private UI actions the buttons call
    /// (go, pickMode, openEditor, ...) and reads the child components to render them. Not used in normal runs.
    friend class ScreenshotRig;
    /// App test hook (app/tests/AppTestSupport.h): hands tests the children and the private actions.
    friend struct AppTestAccess;

    void timerCallback() override;
    void go();
    void fadeOut();
    void togglePause();
    void select(int index);
    void releaseKeys();
    void showChanged();
    void refreshStatus();
    void refreshAlert();
    void refreshKeepAwake();  // awake in Rehearsal and Show mode, so a show is never interrupted by sleep
    void openShowCheck();  // runs a file check now and shows every problem  // shows or hides the banner from ShowSession::currentAlert()
    void toggleSettings();
    void chooseShow();
    void pickMode(ShowMode mode);
    void openEditor(StepId id);
    void openPadEditor(int index);
    void openShowSettings();
    void closeSide();
    void setDetails(bool on);
    void flushState(bool force);

    juce::AudioDeviceManager& devices_;
    ShowSession session_;
    StartPanel start_;
    RunningSounds running_;
    juce::Viewport runningView_;
    StepList steps_;
    StepEditor editor_;  // side panels, one at a time, right of the lists (design 6)
    ShowSettingsPanel settingsPanel_;
    PadEditor padEditor_;  // same slot; the pad grid fills the slot while none of the three is open
    PadGrid pads_;
    ModeFrame goFrame_;  // behind GO, in the mode colour (added before GO)
    juce::TextButton goButton_{"GO (Space)"}, fadeButton_{"Fade out (Esc)"};
    juce::TextButton pauseButton_{"Pause (P)"};  // "Resume (P)" while paused; hidden in Show mode (Action::Pause)
    std::function<void(bool)> keepAwake_;   // setKeepAwake, or the Offline stand-in
    std::optional<bool> awake_;             // what keepAwake_ was last told; unset = never
    std::function<void()> onQuitConfirmed_ = [] { if (auto* a = juce::JUCEApplicationBase::getInstance()) a->quit(); };
    ShowCheckPanel showCheck_;                    // over the step list while open; see openShowCheck()
    AlertBanner alertBanner_;                     // over the top of the step list; see ShowSession::alert
    juce::Label pausedBanner_{{}, "PAUSED"};      // over the running area while the snapshot says paused
    juce::TextButton addFadeButton_{"+ Fade"};  // Edit mode only (Action::EditStructure)
    ModeBar modeBar_;
    StatusBar status_;
    juce::TooltipWindow tooltips_{this};  // shows a failed import's reason on its row
    std::unique_ptr<SettingsWindow> settingsWindow_;
    std::unique_ptr<SettingsWindow> closingWindow_;  // closed, waiting for its async delete
    juce::uint32 lastTickMs_ = 0;  // last session_.tick(), for the 4 Hz housekeeping
    bool spaceHeld_ = false, escHeld_ = false, pHeld_ = false;
    // Held Up/Down: one row, then after kRepeatDelayMs one row per kRepeatIntervalMs at most,
    // so a held key walks the list at a pace the eye can follow.
    static constexpr juce::uint32 kRepeatDelayMs = 400, kRepeatIntervalMs = 125;
    bool arrowHeld_ = false;
    juce::uint32 arrowFirstMs_ = 0, arrowLastMs_ = 0;
    void arrow(int delta);
    void deleteAt(int index);  // asks first when the step is playing  // pressed and not released yet: repeats ignored
    // Up/Down repeat selects many steps a second; state.json is written at most
    // every kStateSaveMs while they do, and once when the keys are released.
    ShowMode shownMode_ = ShowMode::Edit;  // last mode the layout was made for
    int shownRunningRows_ = 0;  // row count the running area was sized for
    bool stateDirty_ = false;
    juce::uint32 lastStateSaveMs_ = 0;
    bool choosing_ = false;  // the start panel covers an open show ("Shows..."); operator keys are off
};

}  // namespace bigtop::app
