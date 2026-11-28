// Main.cpp - entry point of the JUCE desktop app: owns the app settings, the
// audio device manager and the main window. All show logic is in
// ShowSession; the engine lives in core/ (no JUCE).
//
// Command line: `--settings <file>` uses another settings file than the one
// under %APPDATA% (to try the app without touching the real settings).
// `--screenshots <out-dir>` renders the main states to PNG files and quits
// (see Screenshots.h); no window opens and no settings are written.
// `--docs-screenshots <out-dir>` does the same for the 1280x720 images in docs/images.
#include <cstdio>

#include <juce_gui_basics/juce_gui_basics.h>

#include "MainComponent.h"
#include "Screenshots.h"

#if JUCE_WINDOWS
#include <windows.h>
#include <timeapi.h>  // timeBeginPeriod (winmm, already linked by JUCE)
#endif

namespace {

juce::File settingsFile() {
    const auto args = juce::JUCEApplicationBase::getCommandLineParameterArray();
    const auto at = args.indexOf("--settings");
    if (at >= 0 && at + 1 < args.size()) return juce::File::getCurrentWorkingDirectory().getChildFile(args[at + 1].unquoted());
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("Bigtop Player")
        .getChildFile("settings.xml");
}

class MainWindow : public juce::DocumentWindow {
public:
    MainWindow(bigtop::io::AppSettings& settings, juce::AudioDeviceManager& devices)
        : DocumentWindow("Bigtop Player",
                         juce::LookAndFeel::getDefaultLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId),
                         DocumentWindow::allButtons) {
        setUsingNativeTitleBar(true);
        setContentOwned(new bigtop::app::MainComponent(settings, devices), true);
        setResizable(true, true);
        centreWithSize(getWidth(), getHeight());
        setVisible(true);
    }

    void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }
};

class BigtopApp : public juce::JUCEApplication {
public:
    const juce::String getApplicationName() override { return "Bigtop Player"; }
    const juce::String getApplicationVersion() override { return BIGTOP_VERSION; }  // from CMake, with the tag's suffix
    void initialise(const juce::String&) override {
#if JUCE_WINDOWS
        // The Art-Net sender sleeps ~1 ms between passes; Windows would round that up to ~15 ms,
        // which is jitter of almost half a frame at 30 fps. Held for the whole run.
        timeBeginPeriod(1);
#endif
        const auto args = juce::JUCEApplicationBase::getCommandLineParameterArray();
        if (const auto at = args.indexOf("--screenshots"); at >= 0) {
            // Render and leave: the exit code says whether every PNG was written.
            const bool hasDir = at + 1 < args.size();
            if (!hasDir) std::fputs("--screenshots needs an output folder\n", stderr);
            setApplicationReturnValue(
                hasDir ? bigtop::app::renderScreenshots(juce::File::getCurrentWorkingDirectory().getChildFile(args[at + 1].unquoted()))
                       : 2);
            quit();
            return;
        }
        if (const auto at = args.indexOf("--docs-screenshots"); at >= 0) {
            const bool hasDir = at + 1 < args.size();
            if (!hasDir) std::fputs("--docs-screenshots needs an output folder\n", stderr);
            setApplicationReturnValue(
                hasDir ? bigtop::app::renderDocsScreenshots(juce::File::getCurrentWorkingDirectory().getChildFile(args[at + 1].unquoted()))
                       : 2);
            quit();
            return;
        }
        settings_ = std::make_unique<bigtop::io::AppSettings>(settingsFile());
        window_ = std::make_unique<MainWindow>(*settings_, devices_);
    }
    // In Show mode the window asks first; Quit in its question calls quit().
    void systemRequestedQuit() override {
        auto* main = window_ != nullptr ? dynamic_cast<bigtop::app::MainComponent*>(window_->getContentComponent()) : nullptr;
        if (main == nullptr || main->quitRequested()) quit();
    }
    // The window (and with it the session and its audio output) goes before
    // the device manager it is registered with.
    void shutdown() override {
        window_.reset();
        devices_.closeAudioDevice();
        settings_.reset();
#if JUCE_WINDOWS
        timeEndPeriod(1);
#endif
    }

private:
    juce::AudioDeviceManager devices_;
    std::unique_ptr<bigtop::io::AppSettings> settings_;
    std::unique_ptr<MainWindow> window_;
};

}  // namespace

START_JUCE_APPLICATION(BigtopApp)
