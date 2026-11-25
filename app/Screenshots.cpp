// Screenshots.cpp: the demo show and the state sequence behind `--screenshots`.
#include "Screenshots.h"

#include <cmath>
#include <cstdio>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <utility>
#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

#include "DemoAudio.h"
#include "MainComponent.h"

// JUCE's own one-message step (juce_Messaging_windows.cpp / _linux.cpp), as the app tests use it: its
// public wrapper, runDispatchLoopUntil(), needs JUCE_MODAL_LOOPS_PERMITTED, which the app leaves off.
namespace juce::detail {
bool dispatchNextMessageOnSystemQueue(bool returnIfNoPendingMessages);
}  // namespace juce::detail

namespace bigtop::app {

namespace {

constexpr int kBlock = 512;  // frames per silent engine block
constexpr int kDemoSteps = 30;
constexpr int kTones = 5;
constexpr int kToneSeconds = 12;

void fail(const juce::String& what) { std::fprintf(stderr, "screenshots: %s\n", what.toRawUTF8()); }

// Step 1 loops, 2 drives timecode, 3 is plain; all keep the others so three sounds can run together.
// Step 4 is a fade step with one change of each kind; 5.. are ordinary songs (default: fade the others).
Show demoShow() {
    Show show;
    show.settings.goGuardSeconds = 0.0;  // GOs come back to back here
    for (int n = 0; n < kTones; ++n)
        show.files.push_back({static_cast<FileId>(n + 1), "audio/tone" + std::to_string(n) + ".flac", 0.0, ""});
    const char* names[] = {"Intro loop", "Opening song", "Announcement"};
    for (StepId id = 1; id <= kDemoSteps; ++id) {
        if (id == 4) {
            Step fade = makeStep(id, show.settings, std::nullopt);
            fade.name = "Fade step";
            fade.changes = {{1, ChangeKind::Keep, 0.0, 0.0}, {2, ChangeKind::Level, -6.0, 2.0}, {3, ChangeKind::FadeOut, 0.0, 3.0}};
            show.steps.push_back(fade);
            continue;
        }
        Sound sound;
        sound.file = static_cast<FileId>((id - 1) % kTones + 1);
        sound.loop = id == 1;
        if (id == 2) sound.timecodeStart = Timecode{1, 0, 0, 0};
        Step step = makeStep(id, show.settings, sound);
        step.name = id <= 3 ? names[id - 1]
                    : id == 7 ? "A very long step name that has to fit its row without being clipped"
                              : "Song " + std::to_string(id);
        if (id <= 3) step.others = Others{};  // Keep
        show.steps.push_back(step);
    }
    show.lastStepId = kDemoSteps;
    show.pads.resize(15);  // enough for the 3x5 grid
    const char* pads[] = {"Applause", "Stinger", "Fanfare", "Drum roll", "Laugh", "Door", "Phone", "Bell"};
    for (size_t i = 0; i < 8; ++i) show.pads[i] = Pad{pads[i], static_cast<FileId>(i % kTones + 1), 0.0};
    return show;
}

bool makeDemoFolder(const juce::File& dir) {
    std::string error;
    if (!io::ShowFolder::createNew(dir, error)) {
        fail("demo show: " + juce::String(error));
        return false;
    }
    for (int n = 0; n < kTones; ++n)
        // Tones swelling up to about -10 dBFS (three together stay below the limiter), each a
        // different pitch and swell: the demo steps are really playable and the running rows'
        // waveforms have a shape (nothing is heard, no device is open).
        if (!writeSineFlac(dir.getChildFile("audio/tone" + juce::String(n) + ".flac"), kToneSeconds, 0.3f, 220.0 * std::pow(1.2, n),
                           2.0 + 0.7 * n)) {
            fail("cannot write the demo audio");
            return false;
        }
    return io::ShowFolder(dir).save(demoShow(), juce::Time::getCurrentTime()) == io::SaveResult::Saved;
}

}  // namespace

/// @brief Walks one MainComponent through the states.
/// @note A friend of MainComponent: it calls the private actions the buttons call and reads the
///       child components, nothing else.
class ScreenshotRig {
public:
    /// @brief Prepares a run at one width.
    /// @param main The component to drive; must outlive the rig.
    /// @param demoDir The demo show folder to open.
    /// @param outDir Folder for the PNGs.
    /// @param width Window width in px.
    /// @param height Window height in px.
    /// @param docs True to write only the curated documentation set, scaled to 1280x720 under stable names.
    ScreenshotRig(MainComponent& main, const juce::File& demoDir, const juce::File& outDir, int width, int height, bool docs)
        : m_(main), demoDir_(demoDir), outDir_(outDir), width_(width), height_(height), docs_(docs) {}

    /// @brief Runs every state and writes its PNG.
    /// @return False when anything failed (already reported on stderr).
    bool run() {
        m_.setSize(width_, height_);  // the session is Offline: no audio device, only we drive the engine
        settle();
        shot("start-panel");

        std::string error;
        if (!m_.session_.open(demoDir_, error, nullptr)) {
            fail("cannot open the demo show: " + juce::String(error));
            return false;
        }
        m_.showChanged();
        if (!waitForPeaks()) return false;
        settle();
        shot("edit-30-steps");

        // Details on: the fade step's two actions and the sound steps' facts under their names.
        m_.setDetails(true);
        settle();
        shot("step-details");
        m_.setDetails(false);  // the other states show the list as it starts
        settle();

        // A file drag shows the same insertion line a row drag does (StepList's one rule).
        m_.steps_.fileDragEnter({}, 40, 4 * StepList::kRowHeight + 10);
        shot("insertion-line");
        m_.steps_.fileDragExit({});

        m_.chooseShow();
        settle();
        shot("start-panel-open-show");
        m_.choosing_ = false;
        m_.showChanged();

        open([&] { m_.openEditor(5); }, "step-editor-sound");
        open([&] { m_.openEditor(4); }, "step-editor-fade-changes");
        open([&] { m_.openPadEditor(0); }, "pad-editor");
        open([&] { m_.openShowSettings(); }, "show-settings");
        m_.closeSide();
        setPadGrid(3, 5);
        shot("pad-grid-3x5");

        m_.pickMode(ShowMode::Rehearsal);
        settle();
        shot("rehearsal");
        open([&] { m_.openEditor(5); }, "step-editor-rehearsal");
        m_.closeSide();

        for (int i = 0; i < 3; ++i) {  // three sounds: a loop, the timecode one, a plain one
            m_.go();
            audio(20);
        }
        audio(150);
        // Away from 0 dB, so Keep shows lit (the app test "the fader does not move when Keep lights up" checks the layout).
        m_.running_.moveFaderForScreenshot(2, -4.5);
        settle();
        shot("running-three-sounds");
        open([&] { m_.openEditor(2); }, "running-with-editor-open");
        m_.closeSide();

        m_.togglePause();
        audio(120);
        shot("paused");
        m_.togglePause();
        audio(30);

        m_.pickMode(ShowMode::Show);
        settle();
        shot("show-mode");
        m_.modeBar_.showHoldProgress(ShowMode::Edit, 0.5f);
        shot("show-leaving-hold");
        m_.modeBar_.showHoldProgress(ShowMode::Edit, 0.0f);

        m_.pickMode(ShowMode::Edit);  // direct pick: the 2 s hold is only the UI's guard
        m_.fadeOut();
        for (int i = 0; i < 600 && m_.session_.snapshot().numVoices > 0; ++i) audio(1);
        setPadGrid(4, 2);
        m_.session_.controller().triggerPad(0);
        audio(40);
        shot("pad-grid-4x2-playing");
        // A renamed or removed state would silently leave a stale image: fail so CI notices.
        if (docs_ && shots_ != static_cast<int>(std::size(kDocsNames))) {
            fail("docs render produced " + juce::String(shots_) + " of " + juce::String(static_cast<int>(std::size(kDocsNames))) + " images");
            return false;
        }
        return ok_;
    }

private:
    // Runs posted messages until every demo file's waveform is read (at most 5 s), so the
    // running rows are drawn with it in every image.
    bool waitForPeaks() {
        const auto ready = [this] {
            for (const auto& f : m_.session_.controller().show().files)
                if (m_.session_.peaks(f.id) == nullptr) return false;
            return true;
        };
        const auto until = juce::Time::getMillisecondCounter() + 5000;
        while (!ready() && juce::Time::getMillisecondCounter() < until)
            if (!juce::detail::dispatchNextMessageOnSystemQueue(true)) juce::Thread::sleep(5);
        if (!ready()) fail("the demo waveforms were not read within 5 s");
        return ready();
    }

    // Lets the UI catch up with the model: the same refresh the 30 Hz timer does.
    void settle() {
        m_.timerCallback();
        m_.timerCallback();
    }

    // Renders blocks of silence through the engine (no device is open), with a tiny sleep so the disk-read thread keeps up.
    void audio(int blocks) {
        std::vector<float> left(kBlock), right(kBlock);
        for (int i = 0; i < blocks; ++i) {
            m_.session_.engineForScreenshots().process(left.data(), right.data(), kBlock);
            juce::Thread::sleep(2);
        }
        settle();
    }

    void open(const std::function<void()>& action, const char* name) {
        action();
        settle();
        shot(name);
    }

    void setPadGrid(int rows, int cols) {
        m_.session_.edit(Action::ShowSettings, [=](Show& s) {
            s.settings.padRows = rows;
            s.settings.padCols = cols;
            return true;
        });
        settle();
    }

    // The states the docs use, by the stable file name they get in docs/images/. Anything else is skipped in docs mode.
    static constexpr std::pair<const char*, const char*> kDocsNames[] = {
            {"start-panel", "start-panel"},          {"start-panel-open-show", "start-panel-open-show"},
            {"edit-30-steps", "edit-mode"},          {"step-editor-sound", "step-editor"},
            {"step-editor-fade-changes", "fade-step"}, {"show-settings", "show-settings"},
            {"rehearsal", "rehearsal"},              {"running-three-sounds", "running-sounds"},
            {"paused", "paused"},                    {"show-mode", "show-mode"},
            {"pad-grid-4x2-playing", "pads"},          {"step-details", "step-details"}};

    static const char* docsName(const juce::String& state) {
        for (const auto& n : kDocsNames)
            if (state == n.first) return n.second;
        return nullptr;
    }

    void shot(const char* name) {
        const char* docName = docsName(name);
        if (docs_ && docName == nullptr) return;
        ++shots_;
        const auto file = docs_ ? outDir_.getChildFile(juce::String(docName) + ".png")
                                : outDir_.getChildFile(juce::String(++count_).paddedLeft('0', 2) + "-" + name + "-" + juce::String(width_) + ".png");  // not formatted(): %s is wide there
        // The snapshot is transparent where the component paints nothing; the real window's
        // background shows there, so it is put behind it (else white text lands on white).
        const auto snapshot = m_.createComponentSnapshot(m_.getLocalBounds(), true, 1.0f);
        juce::Image image(juce::Image::RGB, snapshot.getWidth(), snapshot.getHeight(), true);
        {
            juce::Graphics g(image);
            g.fillAll(juce::LookAndFeel::getDefaultLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId));
            g.drawImageAt(snapshot, 0, 0);
        }
        // Docs images are rendered at the real 1920x1080 layout, then shrunk so the pages load fast.
        if (docs_) image = image.rescaled(1280, 720, juce::Graphics::highResamplingQuality);
        juce::FileOutputStream out(file);
        if (!image.isValid() || out.failedToOpen() || !out.setPosition(0) || !out.truncate().wasOk() ||
            !juce::PNGImageFormat().writeImageToStream(image, out)) {
            fail("cannot write " + file.getFullPathName());
            ok_ = false;
        }
    }

    MainComponent& m_;
    juce::File demoDir_, outDir_;
    int width_, height_;
    bool docs_;
    int shots_ = 0;  // docs images written (docs mode only)
    int count_ = 0;
    bool ok_ = true;
};

namespace {

// Shared by both entry points: renders the widths in @p widths over the invented demo rig.
int render(const juce::File& outDir, std::initializer_list<int> widths, bool docs) {
    if (!outDir.createDirectory().wasOk()) {
        fail("cannot create " + outDir.getFullPathName());
        return 2;
    }
    const auto work = juce::File::getSpecialLocation(juce::File::tempDirectory)
                          .getChildFile("bigtop-screenshots-" + juce::String::toHexString(juce::Random::getSystemRandom().nextInt()));
    int result = 0;
    // Invented adapters from the documentation ranges (RFC 5737): nothing of this machine is ever shown.
    ShowSession::Offline offline;
    offline.savedTime = "20:00:00";  // the clock would make every re-render differ
    offline.adapters = {{juce::IPAddress("198.51.100.20"), juce::IPAddress("198.51.100.255"), "Ethernet"},
                        {juce::IPAddress("192.0.2.15"), juce::IPAddress("192.0.2.255"), "Wi-Fi"}};
    for (const int width : widths) {
        const int height = width == 1920 ? 1080 : 768;
        // A fresh show and settings file per width: the states change the show (mode, pad grid, steps run).
        const auto dir = work.getChildFile("w" + juce::String(width));
        const auto demo = dir.getChildFile("Demo show");
        if (!dir.createDirectory().wasOk() || !makeDemoFolder(demo)) {
            result = 3;
            break;
        }
        io::AppSettings settings(dir.getChildFile("settings.xml"));
        juce::AudioDeviceManager devices;
        MainComponent main(settings, devices, &offline);
        if (!ScreenshotRig(main, demo, outDir, width, height, docs).run()) result = 4;
    }
    work.deleteRecursively();
    return result;
}

}  // namespace

int renderScreenshots(const juce::File& outDir) { return render(outDir, {1024, 1366, 1920}, false); }

int renderDocsScreenshots(const juce::File& outDir) { return render(outDir, {1920}, true); }

}  // namespace bigtop::app
