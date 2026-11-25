/// @file AppTestSupport.h
/// @brief What every app test builds on: a throwaway show folder, a windowless MainComponent over
///        ShowSession::Offline, direct timer and audio calls, and mouse events with a chosen finger.
///
/// Nothing here opens an audio device, a window or a file chooser, reads the machine's adapters
/// or sends a packet. Time comes from the tests' ManualClock (UiClock.h); only waitHeads() and
/// waitUntil() may wait in real time (at most 2 s each).
#pragma once

#include <functional>
#include <memory>

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "MainComponent.h"
#include "bigtop/Show.h"
#include "bigtop/io/AppSettings.h"

namespace bigtop::app {

/// @brief A friend of MainComponent: hands tests its children and its private actions.
/// @note Message thread. Also a friend of ShowSession, ShowSettingsPanel, StepList, StepEditor, ModeBar,
///       NumberField, PadGrid and PadEditor (their private children and timers).
struct AppTestAccess {
    /// @brief The step list. @param m The main component. @return Its child.
    static StepList& steps(MainComponent& m) { return m.steps_; }
    /// @brief The running sounds. @param m The main component. @return Its child.
    static RunningSounds& running(MainComponent& m) { return m.running_; }
    /// @brief The mode strip. @param m The main component. @return Its child.
    static ModeBar& modeBar(MainComponent& m) { return m.modeBar_; }
    /// @brief The pad grid. @param m The main component. @return Its child.
    static PadGrid& pads(MainComponent& m) { return m.pads_; }
    /// @brief The step editor. @param m The main component. @return Its child.
    static StepEditor& editor(MainComponent& m) { return m.editor_; }
    /// @brief The show settings panel. @param m The main component. @return Its child.
    static ShowSettingsPanel& settingsPanel(MainComponent& m) { return m.settingsPanel_; }
    /// @brief The GO button (for requireHittable()). @param m The main component. @return Its child.
    static juce::TextButton& goButton(MainComponent& m) { return m.goButton_; }
    /// @brief The session. @param m The main component. @return Its session.
    static ShowSession& session(MainComponent& m) { return m.session_; }
    /// @brief Forgets every waveform summary, as if none was read yet (rows draw the plain bar).
    /// @param s The session.
    static void dropPeaks(ShowSession& s) { s.peaks_.clear(); }
    /// @brief One 30 Hz UI tick, as the timer runs it. @param m The main component.
    static void timerCallback(MainComponent& m) { m.timerCallback(); }
    /// @brief What GO (button or Space) does. @param m The main component.
    static void go(MainComponent& m) { m.go(); }
    /// @brief Re-lays the screen after a show was opened or closed. @param m The main component.
    static void showChanged(MainComponent& m) { m.showChanged(); }
    /// @brief Picks a mode directly, as the strip does after its hold. @param m The main component. @param mode The mode.
    static void pickMode(MainComponent& m, ShowMode mode) { m.pickMode(mode); }
    /// @brief Opens the show settings panel. @param m The main component.
    static void openShowSettings(MainComponent& m) { m.openShowSettings(); }
    /// @brief Turns the Details lines on or off, as the strip's checkbox does. @param m The main component. @param on On.
    static void setDetails(MainComponent& m, bool on) { m.setDetails(on); }
    /// @brief The Pause / Resume button. @param m The main component. @return Its child.
    static juce::TextButton& pauseButton(MainComponent& m) { return m.pauseButton_; }
    /// @brief The PAUSED strip over the running area. @param m The main component. @return Its child.
    static juce::Label& pausedBanner(MainComponent& m) { return m.pausedBanner_; }
    /// @brief The show settings' scrolling area. @param p The panel. @return Its viewport.
    static juce::Viewport& viewport(ShowSettingsPanel& p) { return p.view_; }
    /// @brief How often the step list worded its Details lines. @param s The list. @return The count.
    static int describeCount(const StepList& s) { return s.describes_; }
    /// @brief Row @p row's Details lines as last worded. @param s The list. @param row Row index.
    /// @return Them (all empty with Details off).
    static const std::vector<DetailLine>& detailLines(const StepList& s, int row) {
        return s.details_.at(static_cast<size_t>(row));
    }
    /// @brief The painted rows inside the step list's viewport (mouse events on the rows go to it,
    ///        in content coordinates; StepList listens). @param s The list. @return The rows component.
    static juce::Component& rows(StepList& s) { return *s.view_.getViewedComponent(); }
    /// @brief The step list's viewport. @param s The list. @return It.
    static juce::Viewport& view(StepList& s) { return s.view_; }
    /// @brief Row @p i's top edge in content px (rowTops() of the laid-out rows).
    /// @param s The list. @param i Row index, 0..rows (rows = the content height).
    /// @return The offset.
    static int rowTop(const StepList& s, int i) { return s.tops_.at(static_cast<size_t>(i)); }
    /// @brief The centre of row @p i's grab handle, in the rows component's coordinates.
    /// @param s The list. @param i Row index. @return The point.
    static juce::Point<int> handlePoint(const StepList& s, int i) {
        return {StepList::kHandleWidth / 2, rowTop(s, i) + StepList::kRowHeight / 2};
    }
    /// @brief The insertion gap the list draws now. @param s The list. @return The gap, or -1 = no line.
    static int insertionLine(const StepList& s) { return s.gap_; }
    /// @brief One tick of the list's edge auto-scroll timer, as the timer runs it. @param s The list.
    static void scrollTimer(StepList& s) { s.timerCallback(); }
    /// @brief The settings the list stores Details in. @param s The list. @return Them.
    static io::AppSettings& settings(StepList& s) { return s.settings_; }
    /// @brief The mode strip's Details checkbox. @param b The strip. @return Its child.
    static juce::ToggleButton& detailsBox(ModeBar& b) { return b.details_; }
    /// @brief One of the strip's three mode buttons (events go to it in its own coordinates).
    /// @param b The strip. @param mode Which button. @return It.
    static juce::Component& modeButton(ModeBar& b, ShowMode mode) { return b.buttons_.at(static_cast<size_t>(mode)); }
    /// @brief One tick of the strip's hold timer, as the timer runs it. @param b The strip.
    static void holdTimer(ModeBar& b) { b.timerCallback(); }
    /// @brief Opens the step editor, as a row's Edit button does. @param m The main component. @param id The step.
    static void openEditor(MainComponent& m, StepId id) { m.openEditor(id); }
    /// @brief The number field right of a label in the step editor.
    /// @param e The open editor. @param label The label's exact text (e.g. "Fade in").
    /// @return The field; fails the test (REQUIRE) when there is no such label or no field after it.
    static NumberField& field(StepEditor& e, const juce::String& label);
    /// @brief A number field's text box. @param f The field. @return Its child.
    static juce::TextEditor& box(NumberField& f) { return f.box_; }
    /// @brief A number field's [-] button. @param f The field. @return Its child.
    static juce::TextButton& minus(NumberField& f) { return f.minus_; }
    /// @brief A number field's [+] button. @param f The field. @return Its child.
    static juce::TextButton& plus(NumberField& f) { return f.plus_; }

    /// @brief The pad editor. @param m The main component. @return Its child.
    static PadEditor& padEditor(MainComponent& m) { return m.padEditor_; }
    /// @brief What the start panel does after it opened a show: leaves choosing, re-lays the screen.
    /// @param m The main component.
    static void showOpened(MainComponent& m) {
        m.choosing_ = false;
        m.showChanged();
    }
    /// @brief The painted pad buttons (events go to it in content coordinates). @param g The grid. @return It.
    static juce::Component& padCells(PadGrid& g) { return *g.view_.getViewedComponent(); }
    /// @brief The "Edit pads" toggle. @param g The grid. @return Its child.
    static juce::ToggleButton& editPads(PadGrid& g) { return g.editToggle_; }
    /// @brief The label pad @p index paints (its name, else its file's name). @param g The grid.
    /// @param index Pad index. @return The label; empty for an empty pad.
    static juce::String padLabel(const PadGrid& g, int index) { return g.labelOf(index); }
    /// @brief The pad editor's name box. @param e The editor. @return Its child.
    static juce::TextEditor& padName(PadEditor& e) { return e.name_; }
    /// @brief The pad editor's level field. @param e The editor. @return Its child.
    static NumberField& padLevel(PadEditor& e) { return e.level_; }
    /// @brief The pad editor's file trim field. @param e The editor. @return Its child.
    static NumberField& padTrim(PadEditor& e) { return e.trim_; }

    /// @brief The running row of a step (rows carry their step id as component ID).
    /// @param m The main component. @param id The step id.
    /// @return The row; fails the test (REQUIRE) when the step has no row.
    static juce::Component& row(MainComponent& m, StepId id);
    /// @brief A row's fader. @param row A row from row(). @return It; fails the test when missing.
    static juce::Slider& faderOf(juce::Component& row);
    /// @brief A row's Keep button. @param row A row from row(). @return It; fails the test when missing.
    static juce::TextButton& keepOf(juce::Component& row);
    /// @brief A row's Fade / Stop button. @param row A row from row(). @return It; fails the test when missing.
    static juce::TextButton& fadeOf(juce::Component& row);
    /// @brief A row's position bar (events go to it in its own coordinates). @param row A row from row().
    /// @return It; fails the test when missing.
    static juce::Component& barOf(juce::Component& row);
};

/// @brief A fresh, empty folder under the system temp folder, deleted with everything in it.
struct TempDir {
    /// @brief Creates the folder (a random name, so parallel runs never meet).
    TempDir();
    /// @brief Deletes the folder recursively.
    ~TempDir();
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    juce::File dir;  ///< @brief The folder.
};

/// @brief Writes @p show as a show folder inside @p temp, each Show::files entry a sine FLAC.
/// @param temp Where the folder goes (as "Test show").
/// @param show The show; every AudioFile::path is relative to the folder (e.g. "audio/a.flac").
/// @param soundSeconds Length of every FLAC in seconds.
/// @return The show folder. Fails the running test (REQUIRE) when anything cannot be written.
juce::File writeShow(const TempDir& temp, const Show& show, double soundSeconds = 8.0);

/// @brief Three plain sound steps "Intro", "Song A", "Song B", each on its own file.
/// @return The show (default settings, so every GO fades the others).
Show threeSoundSteps();

/// @brief One windowless app over an invented machine, with @p show open.
/// @note Owns, in order, its temp folder, a settings file there, a device manager that is never
///       opened, the Offline stand-ins (two invented adapters, a fixed "Saved" time) and the
///       MainComponent. Message thread.
class TestApp {
public:
    /// @brief Writes the show, builds the main component at the given size and opens the show.
    /// @param show The show to open (see writeShow()).
    /// @param width Main component width in px.
    /// @param height Main component height in px.
    explicit TestApp(const Show& show, int width = 1366, int height = 768);

    /// @brief Destroys the main component before the rest.
    ~TestApp();

    TestApp(const TestApp&) = delete;
    TestApp& operator=(const TestApp&) = delete;

    /// @brief The main component. @return It.
    MainComponent& main();

    /// @brief The session behind it. @return It.
    ShowSession& session();

    /// @brief Runs the 30 Hz UI tick @p n times (the clock does not move; tests advance their ManualClock).
    /// @param n Number of ticks.
    void tick(int n = 1);

    /// @brief Renders @p blocks blocks of 512 frames through the engine, then one UI tick.
    /// @param blocks Number of blocks (512 frames each, about 10.7 ms of audio).
    /// @note No device is open, so this is the audio thread's call made by the test.
    void audio(int blocks);

    /// @brief Waits (real time, at most 2 s) until no import runs and every file of the show can play.
    /// @note One of the two real-time waits the tests allow, with waitUntil() (disk reads run on
    ///       their own thread). Fails the test (REQUIRE) when the time runs out.
    void waitHeads();

    /// @brief Runs the message queue (posted callbacks, timers) until @p done is true, at most 2 s
    ///        of real time.
    /// @param done Checked before each message.
    /// @note For work on the hash and import threads, which report back with callAsync: real time
    ///       is allowed here as in waitHeads(). Fails the test (REQUIRE) when the time runs out.
    void waitUntil(const std::function<bool()>& done);

private:
    TempDir temp_;
    std::unique_ptr<io::AppSettings> settings_;
    juce::AudioDeviceManager devices_;
    ShowSession::Offline offline_;
    std::unique_ptr<MainComponent> main_;
};

/// @brief Sends a press to a component's mouseDown().
/// @param c The component (it receives the event; @p at is in its coordinates).
/// @param at Press position in @p c's coordinates.
/// @param finger The finger index pointerIndex() reports for the event.
/// @param to Who gets the event instead of @p c: a mouse listener of @p c (as StepList is of its
///        rows); nullptr = @p c itself. The same for drag(), release() and leave().
void press(juce::Component& c, juce::Point<int> at, int finger = 0, juce::MouseListener* to = nullptr);

/// @brief Sends a drag to a component's mouseDrag().
/// @param c The component the press went to.
/// @param from Where the press was, in @p c's coordinates.
/// @param to Where the finger is now.
/// @param finger The finger index.
/// @param listener See press().
void drag(juce::Component& c, juce::Point<int> from, juce::Point<int> to, int finger = 0,
          juce::MouseListener* listener = nullptr);

/// @brief Sends a release to a component's mouseUp().
/// @param c The component the press went to.
/// @param from Where the press was, in @p c's coordinates.
/// @param at Where the finger lifts.
/// @param finger The finger index.
/// @param to See press().
void release(juce::Component& c, juce::Point<int> from, juce::Point<int> at, int finger = 0,
             juce::MouseListener* to = nullptr);

/// @brief Sends a mouseExit(): the pointer left @p c.
/// @param c The component the pointer leaves.
/// @param at Last position, in @p c's coordinates.
/// @param finger The finger index.
/// @param to See press().
/// @param mods The buttons held as it leaves: none (the default) = no release will come;
///        juce::ModifierKeys::leftButtonModifier = a drag that left the component and goes on.
void leave(juce::Component& c, juce::Point<int> at, int finger = 0, juce::MouseListener* to = nullptr,
           juce::ModifierKeys mods = {});

/// @brief The first button under @p root (any depth) whose text is @p text.
/// @param root Where to look. @param text The exact button text (e.g. "Show settings").
/// @return It; fails the test (REQUIRE) when there is none.
juce::Button& button(juce::Component& root, const juce::String& text);

/// @brief A press and a release at the same spot.
/// @param c The component. @param at Position in its coordinates. @param finger The finger index.
void tap(juce::Component& c, juce::Point<int> at, int finger = 0);

/// @brief Fails the test unless a user could reach @p c: it and every parent are visible and it
///        has non-empty bounds. Call before every simulated interaction, so a test can never pass
///        against a hidden or 0x0 component.
/// @param c The component.
void requireHittable(const juce::Component& c);

}  // namespace bigtop::app
