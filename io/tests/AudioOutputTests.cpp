// AudioOutputTests.cpp: the output chain (engine -> limiter -> rate converter)
// without a sound card. An engine plays a 1 kHz sine at amplitude 0.5 and the
// chain is rendered at device rates of 48, 44.1 and 96 kHz. AudioOutput is
// tested against a fake JUCE device type (unplug, fallback, replug).
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "bigtop/Engine.h"
#include "bigtop/OutputLimiter.h"
#include "bigtop/io/AudioOutput.h"

using namespace bigtop;
using namespace bigtop::io;

namespace {

constexpr VoiceKey kKey = 1;

/// 10 s of a 1 kHz sine at amplitude 0.5, at the engine rate, computed per frame.
class SineSource : public SoundSource {
public:
    int64_t lengthFrames() const noexcept override { return 10 * 48000; }
    void read(int64_t startFrame, float* left, float* right, int frames) noexcept override {
        for (int i = 0; i < frames; ++i) {
            const double t = static_cast<double>(startFrame + i) / kEngineSampleRate;
            left[i] = right[i] = static_cast<float>(0.5 * std::sin(2.0 * 3.141592653589793 * 1000.0 * t));
        }
    }
};

/// An engine (on the heap: it is large) already told to play the sine.
std::unique_ptr<Engine> sineEngine(SineSource& source) {
    auto engine = std::make_unique<Engine>(kEngineSampleRate);
    StepCommand cmd;
    cmd.hasStart = true;
    cmd.start = StartSpec{kKey, &source, 0, source.lengthFrames(), false, 1.0f, 0, 0, false, Timecode{}};
    REQUIRE(engine->post(cmd));
    return engine;
}

/// Renders @p frames device frames through @p chain in device blocks of
/// @p block; returns the left channel (and checks right == left).
std::vector<float> renderLeft(OutputChain& chain, int frames, int block) {
    std::vector<float> left(static_cast<size_t>(frames)), right(left.size());
    for (int done = 0; done < frames; done += block) {
        const int n = std::min(block, frames - done);
        float* out[2] = {left.data() + done, right.data() + done};
        chain.render(out, 2, n);
    }
    REQUIRE(left == right);
    return left;
}

/// Sign changes, counting 0 as positive (the leading silence then adds none).
int zeroCrossings(const std::vector<float>& x) {
    int n = 0;
    for (size_t i = 1; i < x.size(); ++i)
        if ((x[i - 1] < 0.0f) != (x[i] < 0.0f)) ++n;
    return n;
}

const VoiceState* voice(const Snapshot& s) {
    for (int i = 0; i < s.numVoices; ++i)
        if (s.voices[i].key == kKey) return &s.voices[i];
    return nullptr;
}

}  // namespace

TEST_CASE("Output: At 48 kHz the chain is the engine plus the limiter delay") {
    SineSource source;
    auto engine = sineEngine(source);
    auto reference = sineEngine(source);
    OutputChain chain(*engine);
    chain.prepare(48000.0, 256);

    const int frames = 4800;
    const auto out = renderLeft(chain, frames, 256);

    std::vector<float> refL(static_cast<size_t>(frames)), refR(refL.size());
    for (int done = 0; done < frames; done += 256)
        reference->process(refL.data() + done, refR.data() + done, std::min(256, frames - done));

    // Below the ceiling the limiter is bit-transparent, so the chain must be
    // the raw engine output shifted by exactly the limiter's latency.
    const int lat = OutputLimiter(kEngineSampleRate).latencyFrames();
    REQUIRE(lat > 0);
    for (int i = 0; i < lat; ++i) REQUIRE(out[static_cast<size_t>(i)] == 0.0f);
    for (int i = lat; i < frames; ++i) REQUIRE(out[static_cast<size_t>(i)] == refL[static_cast<size_t>(i - lat)]);
    REQUIRE(chain.gainReductionDb() == 0.0f);
}

TEST_CASE("Output: At 44.1 and 96 kHz the pitch is unchanged") {
    for (const double rate : {44100.0, 96000.0}) {
        INFO("device rate " << rate);
        SineSource source;
        auto engine = sineEngine(source);
        OutputChain chain(*engine);
        chain.prepare(rate, 480);
        // Skip the start-up silence (limiter 72 frames + sinc 100 frames, ~3.6 ms,
        // ~7 crossings) so the count measures pitch, not latency.
        renderLeft(chain, static_cast<int>(rate / 10), 480);
        const auto out = renderLeft(chain, static_cast<int>(rate), 480);

        const int crossings = zeroCrossings(out);
        CHECK(crossings >= 1996);
        CHECK(crossings <= 2004);
        float peak = 0.0f;
        for (size_t i = out.size() / 2; i < out.size(); ++i) peak = std::max(peak, std::abs(out[i]));
        CHECK(std::abs(peak - 0.5f) <= 0.02f);
    }
}

TEST_CASE("Output: The engine advances 48000 frames per device second at any rate") {
    SineSource source;
    auto engine = sineEngine(source);
    OutputChain chain(*engine);
    chain.prepare(96000.0, 512);
    renderLeft(chain, 96000, 512);
    const int64_t processed = engine->snapshot().framesProcessed;
    CHECK(std::llabs(processed - 48000) <= 512);
}

TEST_CASE("Output: A rate change keeps the engine's position") {
    SineSource source;
    auto engine = sineEngine(source);
    OutputChain chain(*engine);
    chain.prepare(48000.0, 256);
    renderLeft(chain, 24000, 256);
    chain.prepare(96000.0, 256);  // the device restarted at another rate
    renderLeft(chain, 48000, 256);

    const Snapshot s = engine->snapshot();
    const VoiceState* v = voice(s);
    REQUIRE(v != nullptr);  // still playing
    CHECK_FALSE(v->pending);
    CHECK(std::llabs(v->positionFrame - 48000) <= 1024);
}

TEST_CASE("Output: Extra device channels are silent") {
    SineSource source;
    auto engine = sineEngine(source);
    OutputChain chain(*engine);
    chain.prepare(44100.0, 256);
    std::vector<std::vector<float>> ch(4, std::vector<float>(256, 1.0f));
    float* out[4] = {ch[0].data(), ch[1].data(), ch[2].data(), ch[3].data()};
    for (int i = 0; i < 20; ++i) chain.render(out, 4, 256);
    CHECK(std::any_of(ch[0].begin(), ch[0].end(), [](float x) { return x != 0.0f; }));
    for (int c = 2; c < 4; ++c)
        CHECK(std::all_of(ch[static_cast<size_t>(c)].begin(), ch[static_cast<size_t>(c)].end(),
                          [](float x) { return x == 0.0f; }));
}

// ---------------------------------------------------------------------------
// AudioOutput against a fake device type: "Desk" (the chosen device) can be
// unplugged and plugged back; "Speakers" is JUCE's default, i.e. the fallback.
// Devices never call back on their own; pump() delivers one callback by hand.

namespace {

class FakeDevice : public juce::AudioIODevice {
public:
    FakeDevice(const juce::String& name, const juce::String& type) : juce::AudioIODevice(name, type) {}
    ~FakeDevice() override { stop(); }
    juce::StringArray getOutputChannelNames() override { return {"L", "R"}; }
    juce::StringArray getInputChannelNames() override { return {}; }
    juce::Array<double> getAvailableSampleRates() override { return {96000.0}; }
    juce::Array<int> getAvailableBufferSizes() override { return {256}; }
    int getDefaultBufferSize() override { return 256; }
    juce::String open(const juce::BigInteger&, const juce::BigInteger&, double, int) override {
        open_ = true;
        return {};
    }
    void close() override { stop(); open_ = false; }
    bool isOpen() override { return open_; }
    void start(juce::AudioIODeviceCallback* cb) override {
        cb->audioDeviceAboutToStart(this);
        callback_ = cb;
    }
    void stop() override {
        if (auto* cb = std::exchange(callback_, nullptr)) cb->audioDeviceStopped();
    }
    bool isPlaying() override { return callback_ != nullptr; }
    juce::String getLastError() override { return {}; }
    int getCurrentBufferSizeSamples() override { return 256; }
    double getCurrentSampleRate() override { return 96000.0; }
    int getCurrentBitDepth() override { return 32; }
    juce::BigInteger getActiveOutputChannels() const override { juce::BigInteger b; b.setRange(0, 2, true); return b; }
    juce::BigInteger getActiveInputChannels() const override { return {}; }
    int getOutputLatencyInSamples() override { return 0; }
    int getInputLatencyInSamples() override { return 0; }

    /// One device callback of 256 frames; returns the peak of both channels.
    float pump() {
        REQUIRE(callback_ != nullptr);
        std::vector<float> l(256, 1.0f), r(256, 1.0f);
        float* out[2] = {l.data(), r.data()};
        callback_->audioDeviceIOCallbackWithContext(nullptr, 0, out, 2, 256, juce::AudioIODeviceCallbackContext{});
        float peak = 0.0f;
        for (size_t i = 0; i < l.size(); ++i) peak = std::max({peak, std::abs(l[i]), std::abs(r[i])});
        return peak;
    }

private:
    bool open_ = false;
    juce::AudioIODeviceCallback* callback_ = nullptr;
};

class FakeType : public juce::AudioIODeviceType {
public:
    explicit FakeType(const juce::String& typeName) : juce::AudioIODeviceType(typeName) {}
    juce::StringArray names;
    void scanForDevices() override {}
    juce::StringArray getDeviceNames(bool wantInputNames) const override {
        return wantInputNames ? juce::StringArray() : names;
    }
    int getDefaultDeviceIndex(bool) const override { return 0; }
    int getIndexOfDevice(juce::AudioIODevice* d, bool asInput) const override {
        return asInput || d == nullptr ? -1 : names.indexOf(d->getName());
    }
    bool hasSeparateInputsAndOutputs() const override { return true; }
    juce::AudioIODevice* createDevice(const juce::String& out, const juce::String&) override {
        return names.contains(out) ? new FakeDevice(out, getTypeName()) : nullptr;
    }
    void unplug(const juce::String& n) { names.removeString(n); callDeviceChangeListeners(); }
    void plug(const juce::String& n) { names.add(n); callDeviceChangeListeners(); }
};

/// A device manager whose device types are FakeTypes: "Fake" with @p names,
/// plus any added with addType(). JUCE then adds no real device types.
struct FakeDevices {
    juce::ScopedJuceInitialiser_GUI juceInit;  // the manager needs a message manager
    juce::AudioDeviceManager manager;
    FakeType* type = nullptr;
    explicit FakeDevices(juce::StringArray names) {
        type = addType("Fake", std::move(names));
        manager.setCurrentAudioDeviceType("Fake", false);
    }
    FakeType* addType(const juce::String& typeName, juce::StringArray names) {
        auto t = std::make_unique<FakeType>(typeName);
        t->names = std::move(names);
        FakeType* raw = t.get();
        manager.addAudioDeviceType(std::move(t));
        return raw;
    }
    FakeDevice* current() { return dynamic_cast<FakeDevice*>(manager.getCurrentAudioDevice()); }
};

juce::AudioDeviceManager::AudioDeviceSetup deskSetup() {
    juce::AudioDeviceManager::AudioDeviceSetup s;
    s.outputDeviceName = "Desk";
    return s;
}

}  // namespace

TEST_CASE("Output: an unplugged desk never falls back to other speakers and comes back when replugged") {
    FakeDevices devices({"Speakers", "Desk"});
    SineSource source;
    auto engine = sineEngine(source);
    AudioOutput out(*engine, devices.manager);
    out.setDesiredSetup(deskSetup(), "Fake");
    REQUIRE(out.running());
    REQUIRE(out.deviceName() == "Desk");
    for (int i = 0; i < 4; ++i) devices.current()->pump();
    CHECK(devices.current()->pump() > 0.1f);

    devices.type->unplug("Desk");
    REQUIRE(out.deviceName() == "Speakers");  // JUCE opened its default as fallback
    CHECK_FALSE(out.running());                // so the app shows its banner
    const int64_t frozen = engine->snapshot().framesProcessed;
    CHECK(devices.current()->pump() == 0.0f);  // nothing reaches the wrong speakers
    CHECK(engine->snapshot().framesProcessed == frozen);

    out.retryIfLost();  // closes the fallback; the desk is still missing
    CHECK(devices.manager.getCurrentAudioDevice() == nullptr);
    CHECK_FALSE(out.running());

    devices.type->plug("Desk");
    out.retryIfLost();
    CHECK(out.running());
    CHECK(out.deviceName() == "Desk");
    CHECK(devices.current()->pump() > 0.1f);  // the sound plays on
    const VoiceState* v = voice(engine->snapshot());
    REQUIRE(v != nullptr);
}

TEST_CASE("Output: a desk missing at launch is retried; a deliberate none is not") {
    FakeDevices devices({"Speakers"});
    SineSource source;
    auto engine = sineEngine(source);
    AudioOutput out(*engine, devices.manager);
    out.setDesiredSetup(deskSetup(), "Fake");
    CHECK_FALSE(out.running());
    CHECK(devices.manager.getCurrentAudioDevice() == nullptr);

    devices.type->plug("Desk");
    out.retryIfLost();
    CHECK(out.running());
    CHECK(out.deviceName() == "Desk");

    out.clearDesiredSetup();
    devices.manager.closeAudioDevice();  // the user chose "none"
    out.retryIfLost();
    CHECK(devices.manager.getCurrentAudioDevice() == nullptr);
    CHECK_FALSE(out.running());
}

TEST_CASE("Output: a desk on its own driver type comes back after JUCE fell back to another type") {
    // "Asio" lists only the desk, so unplugging it leaves that type empty and
    // JUCE switches the manager to "Fake" to open the speakers.
    FakeDevices devices({"Speakers"});
    FakeType* asio = devices.addType("Asio", {"Desk"});
    SineSource source;
    auto engine = sineEngine(source);
    AudioOutput out(*engine, devices.manager);
    out.setDesiredSetup(deskSetup(), "Asio");
    REQUIRE(out.running());
    REQUIRE(devices.manager.getCurrentAudioDeviceType() == "Asio");
    REQUIRE(out.deviceName() == "Desk");

    asio->unplug("Desk");
    REQUIRE(devices.manager.getCurrentAudioDeviceType() == "Fake");
    REQUIRE(out.deviceName() == "Speakers");
    CHECK_FALSE(out.running());
    CHECK(devices.current()->pump() == 0.0f);

    out.retryIfLost();  // the desk is still missing: nothing stays open
    CHECK(devices.manager.getCurrentAudioDevice() == nullptr);

    asio->plug("Desk");
    out.retryIfLost();
    CHECK(out.running());
    CHECK(devices.manager.getCurrentAudioDeviceType() == "Asio");
    CHECK(out.deviceName() == "Desk");
}

TEST_CASE("Output: a device of the same name under another type is not the desired one") {
    FakeDevices devices({"Desk"});
    devices.addType("Asio", {"Desk"});
    SineSource source;
    auto engine = sineEngine(source);
    AudioOutput out(*engine, devices.manager);
    REQUIRE(out.deviceName() == "Desk");  // "Fake" Desk, opened as JUCE's default
    out.setDesiredSetup(deskSetup(), "Asio");
    CHECK(out.running());
    CHECK(devices.manager.getCurrentAudioDeviceType() == "Asio");
}

// The meter's data: the peak since the last read, after the limiter, then reset.
TEST_CASE("Output: takePeaks reports the peak since the last call, then starts again") {
    SineSource source;
    auto engine = sineEngine(source);
    OutputChain chain(*engine);
    chain.prepare(48000.0, 256);
    renderLeft(chain, 4800, 256);
    const OutputPeaks peaks = chain.takePeaks();
    CHECK(std::abs(peaks.left - 0.5f) < 1e-3f);
    CHECK(std::abs(peaks.right - 0.5f) < 1e-3f);
    const OutputPeaks again = chain.takePeaks();  // nothing rendered since
    CHECK(again.left == 0.0f);
    CHECK(again.right == 0.0f);
}
