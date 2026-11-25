// DemoAudio.cpp: the sine FLAC writer shared by the screenshot demo and the app tests.
#include "DemoAudio.h"

#include <cmath>
#include <memory>

#include <juce_audio_formats/juce_audio_formats.h>

namespace bigtop::app {

bool writeSineFlac(const juce::File& file, double seconds, float level, double hz, double swellSeconds) {
    constexpr double kRate = 48000.0;
    constexpr double kPi = juce::MathConstants<double>::pi;
    juce::AudioBuffer<float> buf(2, static_cast<int>(seconds * kRate));
    for (int i = 0; i < buf.getNumSamples(); ++i) {
        // A swell rises and falls from 15 % to full level, so a waveform has a shape to show.
        const double swell = swellSeconds > 0.0 ? 0.15 + 0.85 * std::abs(std::sin(kPi * i / (swellSeconds * kRate))) : 1.0;
        const auto v = static_cast<float>(level * swell * std::sin(2.0 * kPi * hz * i / kRate));
        buf.setSample(0, i, v);
        buf.setSample(1, i, v);
    }
    // FileOutputStream appends to an existing file; start from nothing instead.
    if (file.existsAsFile() && !file.deleteFile()) return false;
    std::unique_ptr<juce::OutputStream> out = std::make_unique<juce::FileOutputStream>(file);
    auto w = juce::FlacAudioFormat().createWriterFor(
        out, juce::AudioFormatWriterOptions{}.withSampleRate(kRate).withNumChannels(2).withBitsPerSample(24));
    return w != nullptr && w->writeFromAudioSampleBuffer(buf, 0, buf.getNumSamples());
}

}  // namespace bigtop::app
