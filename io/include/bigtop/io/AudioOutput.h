/// @file AudioOutput.h
/// @brief The sound-card side of the player: engine -> safety limiter ->
///        realtime rate converter -> device channels 1/2.
///
/// The engine always runs at kEngineSampleRate. OutputChain bridges it to the
/// device rate (e.g. 96 kHz on an SQ-5, 44.1 kHz on some interfaces) and is
/// testable without hardware. AudioOutput plugs the chain into a
/// juce::AudioDeviceManager that the app owns. A device change, restart or
/// unplug only re-prepares the chain; the engine is never reset, so running
/// sounds and their positions carry on.
#pragma once

#include <atomic>
#include <mutex>
#include <vector>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_devices/juce_audio_devices.h>

#include "bigtop/Engine.h"
#include "bigtop/OutputLimiter.h"

namespace bigtop::io {

/// @brief Sample rate the engine always runs at, in frames per second.
/// @note Every import is converted to this rate; when the sound card runs at
///       another rate, one converter at the output bridges the difference.
inline constexpr double kEngineSampleRate = 48000.0;

/// @brief Largest block, in frames, the chain asks the engine for in one
///        Engine::process() call (SoundSource::read handles at most 512).
inline constexpr int kMaxEngineBlock = 512;

/// @brief The device-independent output path: pulls 48 kHz blocks from the
///        engine, limits them, and converts them to the device rate.
///
/// Limited 48 kHz frames wait in a FIFO preallocated by prepare(); render()
/// tops it up from the engine (in blocks of at most kMaxEngineBlock) and then
/// either copies it out (device at 48 kHz) or resamples it with one
/// juce::WindowedSincInterpolator per channel.
/// @note Threading: prepare() on the control or device thread, never
///       concurrently with render(); render() on the audio thread;
///       gainReductionDb() from any thread. Inside AudioOutput this holds
///       because juce::AudioDeviceManager calls audioDeviceAboutToStart and
///       every audio callback under its audioCallbackLock.
/// @note The limiter runs before the converter, so the converted signal can
///       overshoot the -0.3 dBFS ceiling by a resampler's ringing (a fraction
///       of a dB on extreme peaks); the ceiling is a PA guard, not a codec limit.
class OutputChain {
public:
    /// @brief Builds the chain around @p engine and its limiter.
    /// @param engine Engine to pull audio from; must outlive the chain and run
    ///        at kEngineSampleRate (asserted).
    /// @note Control thread; allocates. render() outputs silence until prepare().
    explicit OutputChain(Engine& engine);

    /// @brief Sizes all buffers for a device rate and block size.
    /// @param deviceRate Device sample rate in Hz; <= 0 is treated as kEngineSampleRate.
    /// @param maxDeviceBlock Largest render() block expected, in frames; < 1 is
    ///        treated as 1. Larger render() calls still work (split internally).
    /// @note Control or device thread, never concurrent with render(); may
    ///       allocate. Can be called again with another rate at any time the
    ///       device is stopped: the converter's history restarts (a click at
    ///       most, on a device restart anyway); frames already buffered, the
    ///       engine and the limiter keep their state.
    void prepare(double deviceRate, int maxDeviceBlock);

    /// @brief Renders one device block.
    /// @param out Device channel buffers, @p numChannels of @p frames samples;
    ///        null entries are skipped.
    /// @param numChannels Number of channels in @p out; channel 0 gets left,
    ///        channel 1 right, all others silence (a mono device gets left only).
    /// @param frames Frames to render; <= 0 does nothing.
    /// @note Audio thread only. Never allocates, locks, throws or does I/O.
    void render(float* const* out, int numChannels, int frames) noexcept;

    /// @brief The limiter's current gain reduction.
    /// @return dB, 0 when not limiting, positive otherwise.
    /// @note Any thread.
    float gainReductionDb() const noexcept { return limiter_.gainReductionDb(); }

private:
    void renderChunk(float* const* out, int numChannels, int offset, int frames) noexcept;
    void fill(int needed) noexcept;

    Engine& engine_;
    OutputLimiter limiter_;
    double ratio_ = 1.0;       ///< @brief Engine frames per device frame (48000 / deviceRate).
    int maxBlock_ = 0;         ///< @brief Device frames per renderChunk(); 0 = not prepared.
    std::vector<float> fifoL_, fifoR_;  ///< @brief Limited 48 kHz frames not yet used; valid in [0, fifoCount_).
    int fifoCount_ = 0;
    std::vector<float> scratchL_, scratchR_;  ///< @brief Converter output, maxBlock_ frames each.
    juce::WindowedSincInterpolator interpL_, interpR_;
};


/// @brief Connects an OutputChain to the app's juce::AudioDeviceManager and
///        keeps the output on the device the user chose.
///
/// Registers itself as audio callback on construction. The app owns the
/// manager and opens devices; this class only ever reopens the *desired*
/// setup, which the app sets explicitly (setDesiredSetup / clearDesiredSetup)
/// and which nothing else changes.
///
/// Device lost (design 7.1): when the desired device disappears, JUCE closes
/// it and may open a fallback (e.g. the laptop speakers). A fallback never
/// plays: its callbacks output silence without running the engine (sounds
/// hold their positions, as when no device runs), running() is false so the
/// app shows its banner, and the next retryIfLost() closes it and tries the
/// desired setup again, every 2 s until it is back. A desired device that is
/// missing at launch is retried the same way.
/// @note With no desired setup set (first launch, or after clearDesiredSetup)
///       any device the app opens plays and nothing is retried.
/// @note The desired setup is reopened on its own device type (driver, e.g.
///       ASIO), even when JUCE switched to another type for the fallback
///       because the desired type had no devices left.
class AudioOutput : private juce::AudioIODeviceCallback {
public:
    /// @brief Hooks @p engine up to @p devices; no desired setup yet.
    /// @param engine Engine to play; must outlive this object.
    /// @param devices The app's device manager; must outlive this object.
    /// @note Message thread. If a device is already running, JUCE calls
    ///       audioDeviceAboutToStart at once.
    AudioOutput(Engine& engine, juce::AudioDeviceManager& devices);

    /// @brief Unregisters from the device manager (a running callback finishes first).
    ~AudioOutput() override;

    /// @brief Sets the device the output must play on and opens it if needed.
    /// @param setup The chosen setup (its outputDeviceName names the device;
    ///        rate and buffer size are what a reopen restores).
    /// @param deviceType Name of the device type the device belongs to, as
    ///        juce::AudioDeviceManager::getCurrentAudioDeviceType() gives it.
    /// @note Message thread. The app calls it at start-up from the stored
    ///       device state (AppSettings), and for the settings window as
    ///       follows: JUCE's AudioDeviceSelectorComponent changes the manager
    ///       itself, so the app pauses its retryIfLost() timer while the window
    ///       is open (otherwise the user's new pick looks like a fallback and
    ///       is closed) and, when it closes (or on each change broadcast while
    ///       open), calls setDesiredSetup(devices.getAudioDeviceSetup(),
    ///       devices.getCurrentAudioDeviceType()).
    /// @note Rate or buffer-size changes on the same device must be passed in
    ///       too, or the next reopen restores the old ones.
    /// @note Calls retryIfLost() at once, so a missing device is retried from now on.
    void setDesiredSetup(const juce::AudioDeviceManager::AudioDeviceSetup& setup, const juce::String& deviceType);

    /// @brief Forgets the desired setup (the user deliberately chose "none").
    /// @note Message thread. retryIfLost() then does nothing.
    void clearDesiredSetup();

    /// @brief Whether the desired output device is open and calling back.
    /// @return True between audioDeviceAboutToStart and stop/error of the
    ///         desired device (of any device when none is desired); false
    ///         while a fallback device is open.
    /// @note Any thread.
    bool running() const noexcept {
        return running_.load(std::memory_order_acquire) && desiredOpen_.load(std::memory_order_acquire);
    }

    /// @brief Name of the open device (which may be a muted fallback).
    /// @return The device name, or empty when none is open.
    /// @note Message thread.
    juce::String deviceName() const;

    /// @brief Reopens the desired setup unless it is already running.
    /// @note Message (control) thread; the app calls it from a timer every 2 s.
    ///       Does nothing without a desired setup or while running(). Otherwise
    ///       closes whatever is open (a fallback or a stopped device), switches
    ///       the manager back to the desired device type and opens the desired
    ///       setup; if it is still missing, no device stays open.
    void retryIfLost();

    /// @brief The output limiter's current gain reduction (for the CLIP light).
    /// @return dB, 0 when not limiting.
    /// @note Any thread.
    float gainReductionDb() const noexcept { return chain_.gainReductionDb(); }

private:
    void audioDeviceIOCallbackWithContext(const float* const* inputChannelData, int numInputChannels,
                                          float* const* outputChannelData, int numOutputChannels,
                                          int numSamples,
                                          const juce::AudioIODeviceCallbackContext& context) override;
    void audioDeviceAboutToStart(juce::AudioIODevice* device) override;
    void audioDeviceStopped() override;
    void audioDeviceError(const juce::String& errorMessage) override;
    bool isDesired(juce::AudioIODevice* device) const;

    juce::AudioDeviceManager& devices_;
    OutputChain chain_;
    std::atomic<bool> running_{false};      ///< @brief A device is open and calling back.
    std::atomic<bool> desiredOpen_{true};   ///< @brief That device is the desired one (or none is desired).
    // Written on the message thread, read in audioDeviceAboutToStart, which
    // some drivers call from their own thread; never touched by the audio callback.
    mutable std::mutex desiredMutex_;
    juce::AudioDeviceManager::AudioDeviceSetup desired_;
    juce::String desiredType_;
    bool hasDesired_ = false;
};

}  // namespace bigtop::io
