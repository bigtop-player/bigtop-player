// AudioOutput.cpp - implementation of AudioOutput.h: the output chain
// (FIFO of limited 48 kHz frames + per-channel windowed-sinc converter) and
// the thin AudioDeviceManager wrapper around it.
#include "bigtop/io/AudioOutput.h"

#include <algorithm>
#include <cmath>

namespace bigtop::io {

namespace {
// Extra FIFO room beyond one chunk's worst-case need: the interpolator can
// leave a couple of frames unused per chunk, which carry into the next one.
constexpr int kFifoSlack = 8;
}  // namespace

OutputChain::OutputChain(Engine& engine) : engine_(engine), limiter_(kEngineSampleRate) {
    jassert(engine.sampleRate() == kEngineSampleRate);
}

void OutputChain::prepare(double deviceRate, int maxDeviceBlock) {
    if (!(deviceRate > 0.0)) deviceRate = kEngineSampleRate;
    maxBlock_ = std::max(1, maxDeviceBlock);
    ratio_ = kEngineSampleRate / deviceRate;
    // Worst case one chunk needs ceil(n * ratio) + 2 frames (see renderChunk).
    // Never shrink: resize then keeps every frame already buffered, so no
    // engine audio is dropped on a rate change; only the converter's history restarts.
    const auto capacity = std::max(fifoL_.size(), static_cast<size_t>(std::ceil(maxBlock_ * ratio_)) + 2 + kFifoSlack);
    fifoL_.resize(capacity);
    fifoR_.resize(capacity);
    scratchL_.assign(static_cast<size_t>(maxBlock_), 0.0f);
    scratchR_.assign(static_cast<size_t>(maxBlock_), 0.0f);
    interpL_.reset();
    interpR_.reset();
}

void OutputChain::render(float* const* out, int numChannels, int frames) noexcept {
    if (frames <= 0) return;
    if (maxBlock_ == 0) {  // not prepared yet: silence, and the engine stays put
        for (int c = 0; c < numChannels; ++c)
            if (out[c] != nullptr) std::fill(out[c], out[c] + frames, 0.0f);
        return;
    }
    // Hosts may exceed the announced block size; split rather than overrun.
    for (int done = 0; done < frames; done += maxBlock_)
        renderChunk(out, numChannels, done, std::min(maxBlock_, frames - done));
}

void OutputChain::fill(int needed) noexcept {
    needed = std::min(needed, static_cast<int>(fifoL_.size()));
    while (fifoCount_ < needed) {
        const int n = std::min(kMaxEngineBlock, needed - fifoCount_);
        float* l = fifoL_.data() + fifoCount_;
        float* r = fifoR_.data() + fifoCount_;
        engine_.process(l, r, n);
        limiter_.process(l, r, n);
        fifoCount_ += n;
    }
}

void OutputChain::renderChunk(float* const* out, int numChannels, int offset, int frames) noexcept {
    const bool passthrough = ratio_ == 1.0;
    // The interpolator consumes floor(start position + (frames - 1) * ratio)
    // inputs with a start position below 1 + ratio, so ceil(frames * ratio) + 1
    // always suffices; +2 keeps a margin against rounding in the double sum.
    const int needed = passthrough ? frames : static_cast<int>(std::ceil(frames * ratio_)) + 2;
    fill(needed);

    const float* srcL = fifoL_.data();
    const float* srcR = fifoR_.data();
    int used = frames;
    if (!passthrough) {
        // Same ratio and same input count on both channels, so both
        // interpolators consume the same number of frames.
        used = interpL_.process(ratio_, fifoL_.data(), scratchL_.data(), frames);
        interpR_.process(ratio_, fifoR_.data(), scratchR_.data(), frames);
        srcL = scratchL_.data();
        srcR = scratchR_.data();
    }
    used = std::min(used, fifoCount_);

    for (int c = 0; c < numChannels; ++c) {
        if (out[c] == nullptr) continue;
        float* dst = out[c] + offset;
        if (c == 0)
            std::copy(srcL, srcL + frames, dst);
        else if (c == 1)
            std::copy(srcR, srcR + frames, dst);
        else
            std::fill(dst, dst + frames, 0.0f);
    }

    // Keep the unused frames at the front for the next chunk (a few frames when
    // resampling, none at 48 kHz); a short memmove, realtime safe.
    std::copy(fifoL_.begin() + used, fifoL_.begin() + fifoCount_, fifoL_.begin());
    std::copy(fifoR_.begin() + used, fifoR_.begin() + fifoCount_, fifoR_.begin());
    fifoCount_ -= used;
}

AudioOutput::AudioOutput(Engine& engine, juce::AudioDeviceManager& devices)
    : devices_(devices), chain_(engine) {
    devices_.addAudioCallback(this);
}

AudioOutput::~AudioOutput() {
    // removeAudioCallback waits for a running callback to finish.
    devices_.removeAudioCallback(this);
}

void AudioOutput::setDesiredSetup(const juce::AudioDeviceManager::AudioDeviceSetup& setup,
                                  const juce::String& deviceType) {
    {
        const std::lock_guard<std::mutex> lock(desiredMutex_);
        desired_ = setup;
        desiredType_ = deviceType;
        hasDesired_ = true;
    }
    // The open device may already be the new choice (the settings window opened it).
    auto* device = devices_.getCurrentAudioDevice();
    desiredOpen_.store(device != nullptr && isDesired(device), std::memory_order_release);
    retryIfLost();
}

void AudioOutput::clearDesiredSetup() {
    {
        const std::lock_guard<std::mutex> lock(desiredMutex_);
        hasDesired_ = false;
    }
    desiredOpen_.store(true, std::memory_order_release);  // nothing desired: whatever is open plays
}

bool AudioOutput::isDesired(juce::AudioIODevice* device) const {
    // Type as well as name: the same name under another driver is another device.
    const std::lock_guard<std::mutex> lock(desiredMutex_);
    return !hasDesired_ || (device->getTypeName() == desiredType_ && device->getName() == desired_.outputDeviceName);
}

juce::String AudioOutput::deviceName() const {
    auto* device = devices_.getCurrentAudioDevice();
    return device != nullptr ? device->getName() : juce::String();
}

void AudioOutput::retryIfLost() {
    juce::AudioDeviceManager::AudioDeviceSetup setup;
    juce::String type;
    {
        const std::lock_guard<std::mutex> lock(desiredMutex_);
        if (!hasDesired_) return;
        setup = desired_;
        type = desiredType_;
    }
    if (running()) return;
    // Close first: a fallback or a stopped device must go, and with a device
    // still open JUCE would treat an unchanged setup as nothing to do.
    // The lock is released above: opening calls audioDeviceAboutToStart, which takes it.
    devices_.closeAudioDevice();
    // JUCE switches to another type for its fallback when the desired one had
    // no devices left (e.g. an unplugged USB ASIO driver); switch back first.
    // Not "chosen": the type's default device it may open is replaced at once.
    // Nothing is open here, so JUCE skips its 1.5 s pause between types.
    devices_.setCurrentAudioDeviceType(type, false);
    devices_.setAudioDeviceSetup(setup, true);
}

void AudioOutput::audioDeviceIOCallbackWithContext(const float* const*, int, float* const* outputChannelData,
                                                   int numOutputChannels, int numSamples,
                                                   const juce::AudioIODeviceCallbackContext&) {
    if (!desiredOpen_.load(std::memory_order_acquire)) {
        // A fallback device: no show audio to the wrong speakers, and the
        // engine stands still as if no device ran (nothing is freed meanwhile).
        for (int c = 0; c < numOutputChannels; ++c)
            if (outputChannelData[c] != nullptr) std::fill(outputChannelData[c], outputChannelData[c] + numSamples, 0.0f);
        return;
    }
    chain_.render(outputChannelData, numOutputChannels, numSamples);
}

void AudioOutput::audioDeviceAboutToStart(juce::AudioIODevice* device) {
    // Only the chain is re-prepared, never the engine, so sounds play on
    // across a device restart or rate change.
    chain_.prepare(device->getCurrentSampleRate(), device->getCurrentBufferSizeSamples());
    desiredOpen_.store(isDesired(device), std::memory_order_release);
    running_.store(true, std::memory_order_release);
}

void AudioOutput::audioDeviceStopped() { running_.store(false, std::memory_order_release); }

void AudioOutput::audioDeviceError(const juce::String&) { running_.store(false, std::memory_order_release); }

}  // namespace bigtop::io
