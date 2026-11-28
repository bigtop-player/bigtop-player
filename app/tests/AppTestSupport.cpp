// AppTestSupport.cpp: the temp show folder, the windowless app and the synthetic mouse events.
#include "AppTestSupport.h"

#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "DemoAudio.h"
#include "Pointer.h"
#include "bigtop/io/ShowFolder.h"

#if !JUCE_MAC
// JUCE's own one-message step (juce_Messaging_windows.cpp / _linux.cpp). Its public wrapper,
// runDispatchLoopUntil(), needs JUCE_MODAL_LOOPS_PERMITTED, which only macOS turns on.
namespace juce::detail {
bool dispatchNextMessageOnSystemQueue(bool returnIfNoPendingMessages);
}  // namespace juce::detail
#endif

namespace bigtop::app {

namespace {

constexpr int kBlock = 512;  // frames per engine block, as the screenshot rig renders them

// Every event comes from the main mouse source (index 0); PointerOverride makes it the wanted finger.
juce::MouseEvent event(juce::Component& c, juce::Point<int> downAt, juce::Point<int> at, bool dragged,
                       juce::ModifierKeys mods = juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier)) {
    const auto now = juce::Time::getCurrentTime();  // event times are not the UI clock; nothing reads them for timing
    return {juce::Desktop::getInstance().getMainMouseSource(),
            at.toFloat(),
            mods,
            juce::MouseInputSource::defaultPressure,
            juce::MouseInputSource::defaultOrientation,
            juce::MouseInputSource::defaultRotation,
            juce::MouseInputSource::defaultTiltX,
            juce::MouseInputSource::defaultTiltY,
            &c,
            &c,
            now,
            downAt.toFloat(),
            now,
            1,
            dragged};
}

// A named part of a running row (RunningSounds gives each part a component ID).
template <typename T>
T& part(juce::Component& row, const char* id) {
    INFO("row part: " << id);
    auto* c = dynamic_cast<T*>(row.findChildWithID(id));
    REQUIRE(c != nullptr);
    return *c;
}

}  // namespace

juce::Component& AppTestAccess::row(MainComponent& m, StepId id) {
    INFO("running row of step " << id);
    auto* r = m.running_.findChildWithID(juce::String(id));
    REQUIRE(r != nullptr);
    return *r;
}

juce::Slider& AppTestAccess::faderOf(juce::Component& row) { return part<juce::Slider>(row, "fader"); }

juce::TextButton& AppTestAccess::keepOf(juce::Component& row) { return part<juce::TextButton>(row, "keep"); }

juce::TextButton& AppTestAccess::fadeOf(juce::Component& row) { return part<juce::TextButton>(row, "fade"); }

juce::Component& AppTestAccess::barOf(juce::Component& row) { return part<juce::Component>(row, "bar"); }

juce::Label& AppTestAccess::nameOf(juce::Component& row) { return part<juce::Label>(row, "name"); }

juce::String AppTestAccess::partText(juce::Component& row, const char* id) { return part<juce::Label>(row, id).getText(); }

NumberField& AppTestAccess::field(StepEditor& e, const juce::String& label) {
    INFO("editor field: " << label);
    // Fields are built right after their label (addNumber), so the next NumberField in build order is it.
    bool seen = false;
    for (const auto& c : e.owned_) {
        if (auto* l = dynamic_cast<juce::Label*>(c.get()); l != nullptr && l->getText() == label) seen = true;
        if (auto* f = dynamic_cast<NumberField*>(c.get()); f != nullptr && seen) return *f;
    }
    REQUIRE(false);
    std::abort();  // not reached: REQUIRE throws
}

TempDir::TempDir()
    : dir(juce::File::getSpecialLocation(juce::File::tempDirectory)
              .getChildFile("bigtop-app-test-" + juce::String::toHexString(juce::Random::getSystemRandom().nextInt64()))) {
    REQUIRE(dir.createDirectory().wasOk());
}

TempDir::~TempDir() { dir.deleteRecursively(); }

juce::File writeShow(const TempDir& temp, const Show& show, double soundSeconds) {
    const auto folder = temp.dir.getChildFile("Test show");
    std::string error;
    REQUIRE(io::ShowFolder::createNew(folder, error));
    for (const auto& f : show.files) {
        const auto file = folder.getChildFile(juce::String::fromUTF8(f.path.c_str()));
        REQUIRE(file.getParentDirectory().createDirectory().wasOk());
        REQUIRE(writeSineFlac(file, soundSeconds, 0.1f));
    }
    REQUIRE(io::ShowFolder(folder).save(show, juce::Time::getCurrentTime()) == io::SaveResult::Saved);
    return folder;
}

Show threeSoundSteps() {
    Show show;
    const char* names[] = {"Intro", "Song A", "Song B"};
    for (StepId id = 1; id <= 3; ++id) {
        const auto file = static_cast<FileId>(id);
        show.files.push_back({file, "audio/sound" + std::to_string(id) + ".flac", 0.0, ""});
        Sound sound;
        sound.file = file;
        Step step = makeStep(id, show.settings, sound);
        step.name = names[id - 1];
        show.steps.push_back(step);
    }
    show.lastStepId = 3;
    return show;
}

TestApp::TestApp(const Show& show, int width, int height) {
    const auto folder = writeShow(temp_, show);
    settings_ = std::make_unique<io::AppSettings>(temp_.dir.getChildFile("settings.xml"));
    // Invented adapters from the documentation ranges (RFC 5737), as the screenshot mode uses.
    offline_.savedTime = "20:00:00";
    offline_.gainReductionDb = [this] { return gainReductionDb_; };
    offline_.outputPeaks = [this] { return outputPeaks_; };
    offline_.keepAwake = [this](bool on) { keepAwakeCalls_.push_back(on); };
    // Ontime: records each URL and answers 200, so no request leaves the machine.
    offline_.http = [urls = ontimeUrls_] {
        struct Recording : io::HttpGet {
            explicit Recording(decltype(urls) u) : log(std::move(u)) {}
            int get(const juce::String& url, int) override {
                std::lock_guard<std::mutex> lock(log->first);
                log->second.push_back(url);
                return 200;
            }
            decltype(urls) log;
        };
        return std::unique_ptr<io::HttpGet>(std::make_unique<Recording>(urls));
    };
    offline_.checkFiles = [this](const std::vector<juce::File>& files) {
        ++fileChecks_;
        while (holdChecks_) juce::Thread::sleep(2);  // a slow disk, released by the test
        return ShowSession::statFiles(files);
    };
    offline_.adapters = {{juce::IPAddress("198.51.100.20"), juce::IPAddress("198.51.100.255"), "Ethernet"},
                         {juce::IPAddress("192.0.2.15"), juce::IPAddress("192.0.2.255"), "Wi-Fi"}};
    main_ = std::make_unique<MainComponent>(*settings_, devices_, &offline_);
    main_->setSize(width, height);
    main_->setVisible(true);  // as the window shows it; no peer is created (requireHittable() checks the flags)
    std::string error;
    REQUIRE(session().open(folder, error, nullptr));
    AppTestAccess::showChanged(*main_);
    tick(2);
}

TestApp::~TestApp() { main_.reset(); }

MainComponent& TestApp::main() { return *main_; }

ShowSession& TestApp::session() { return AppTestAccess::session(*main_); }

void TestApp::tick(int n) {
    for (int i = 0; i < n; ++i) AppTestAccess::timerCallback(*main_);
}

void TestApp::audio(int blocks) {
    std::vector<float> left(kBlock), right(kBlock);
    for (int i = 0; i < blocks; ++i) session().engineForScreenshots().process(left.data(), right.data(), kBlock);
    tick();
}

void TestApp::waitHeads() {
    // Heads load while the show opens; a converting import makes its file usable only once done.
    waitUntil([this] {
        if (session().importsRunning()) return false;
        for (const auto& f : session().controller().show().files)
            if (!session().sources().fileUsable(f.id)) return false;
        return true;
    });
}

namespace {
// One step of the message loop, or a short sleep when it is empty.
void pumpOnce() {
#if JUCE_MAC
    juce::MessageManager::getInstance()->runDispatchLoopUntil(5);  // macOS has no one-message step
#else
    if (!juce::detail::dispatchNextMessageOnSystemQueue(true)) juce::Thread::sleep(5);  // queue empty: wait a little
#endif
}
}  // namespace

std::vector<juce::String> TestApp::ontimeUrls() {
    waitUntil([this] { return !session().ontime().busy(); });
    std::lock_guard<std::mutex> lock(ontimeUrls_->first);
    std::vector<juce::String> out;
    for (const auto& u : ontimeUrls_->second)
        if (!u.endsWith("/api/version")) out.push_back(u);  // the health check is not what the tests look at
    return out;
}

void TestApp::pumpMessages(int ms) {
    const auto until = juce::Time::getMillisecondCounter() + static_cast<juce::uint32>(ms);
    while (juce::Time::getMillisecondCounter() < until) pumpOnce();
}

void TestApp::waitUntil(const std::function<bool()>& done) {
    const auto until = juce::Time::getMillisecondCounter() + 2000;  // real time: disk threads only
    while (!done() && juce::Time::getMillisecondCounter() < until) pumpOnce();
    REQUIRE(done());
}

void press(juce::Component& c, juce::Point<int> at, int finger, juce::MouseListener* to) {
    const PointerOverride pointer(finger);
    (to != nullptr ? *to : c).mouseDown(event(c, at, at, false));
}

void drag(juce::Component& c, juce::Point<int> from, juce::Point<int> to, int finger, juce::MouseListener* listener) {
    const PointerOverride pointer(finger);
    (listener != nullptr ? *listener : c).mouseDrag(event(c, from, to, from != to));
}

void release(juce::Component& c, juce::Point<int> from, juce::Point<int> at, int finger, juce::MouseListener* to) {
    const PointerOverride pointer(finger);
    (to != nullptr ? *to : c).mouseUp(event(c, from, at, from != at));
}

void leave(juce::Component& c, juce::Point<int> at, int finger, juce::MouseListener* to, juce::ModifierKeys mods) {
    const PointerOverride pointer(finger);
    (to != nullptr ? *to : c).mouseExit(event(c, at, at, false, mods));
}

juce::Button& button(juce::Component& root, const juce::String& text) {
    INFO("button: " << text);
    // Depth first in child order, so the first match is the one a reader of the layout expects.
    std::function<juce::Button*(juce::Component&)> find = [&](juce::Component& c) -> juce::Button* {
        if (auto* b = dynamic_cast<juce::Button*>(&c); b != nullptr && b->getButtonText() == text) return b;
        for (auto* child : c.getChildren())
            if (auto* hit = find(*child)) return hit;
        return nullptr;
    };
    auto* b = find(root);
    REQUIRE(b != nullptr);
    return *b;
}

void tap(juce::Component& c, juce::Point<int> at, int finger) {
    press(c, at, finger);
    release(c, at, at, finger);
}

void requireHittable(const juce::Component& c) {
    INFO("component: " << c.getName() << " " << c.getBounds().toString());
    REQUIRE(!c.getBounds().isEmpty());
    for (const juce::Component* p = &c; p != nullptr; p = p->getParentComponent()) REQUIRE(p->isVisible());
}

}  // namespace bigtop::app
