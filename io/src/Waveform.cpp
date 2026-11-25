// Waveform.cpp: one pass over a file in blocks into min/max points, and the
// column slices the position bar draws.
#include "bigtop/io/Waveform.h"

#include <algorithm>
#include <memory>

#include <juce_audio_formats/juce_audio_formats.h>

namespace bigtop::io {

namespace {
constexpr int kBlockFrames = 1 << 16;  // 1.4 s at 48 kHz: few reads, and the cancel flag is seen quickly
}  // namespace

Peaks readPeaks(const juce::File& flac, const std::atomic<bool>* cancel) {
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    const std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(flac));
    if (reader == nullptr || reader->lengthInSamples <= 0 || reader->numChannels <= 0) return {};
    const int64_t frames = reader->lengthInSamples;
    const int points = static_cast<int>(std::min<int64_t>(kWaveformPoints, frames));
    Peaks out;
    out.frames = frames;
    out.min.assign(static_cast<size_t>(points), 1.0f);
    out.max.assign(static_cast<size_t>(points), -1.0f);

    // Frame f belongs to point floor(f * points / frames); every point gets at least one frame
    // because points <= frames. `next` is the first frame of point p + 1.
    const auto firstFrameOf = [&](int64_t p) { return (p * frames + points - 1) / points; };
    int64_t p = 0, next = firstFrameOf(1);
    juce::AudioBuffer<float> buf(static_cast<int>(reader->numChannels), kBlockFrames);
    for (int64_t pos = 0; pos < frames; pos += kBlockFrames) {
        if (cancel != nullptr && cancel->load()) return {};
        const int n = static_cast<int>(std::min<int64_t>(kBlockFrames, frames - pos));
        if (!reader->read(&buf, 0, n, pos, true, true)) return {};
        for (int i = 0; i < n; ++i) {
            while (pos + i >= next) next = firstFrameOf(++p + 1);
            float& lo = out.min[static_cast<size_t>(p)];
            float& hi = out.max[static_cast<size_t>(p)];
            for (int ch = 0; ch < buf.getNumChannels(); ++ch) {
                const float s = buf.getSample(ch, i);
                lo = std::min(lo, s);
                hi = std::max(hi, s);
            }
        }
    }
    // A float file may hold overs; the summary promises [-1, 1].
    for (auto& v : out.min) v = std::clamp(v, -1.0f, 1.0f);
    for (auto& v : out.max) v = std::clamp(v, -1.0f, 1.0f);
    return out;
}

void peakRange(const Peaks& peaks, int64_t start, int64_t end, int columns, std::vector<float>& lo,
               std::vector<float>& hi) {
    const int cols = std::max(columns, 0);
    lo.assign(static_cast<size_t>(cols), 0.0f);
    hi.assign(static_cast<size_t>(cols), 0.0f);
    const int64_t frames = peaks.frames;
    const auto points = static_cast<int64_t>(peaks.min.size());
    if (frames <= 0 || points == 0 || end <= start) return;
    for (int c = 0; c < cols; ++c) {
        // The slice keeps its place in the range even where the range leaves the file,
        // so the drawing lines up with the playhead; only the frames are clamped.
        int64_t a = start + (end - start) * c / cols;
        int64_t b = std::max(start + (end - start) * (c + 1) / cols, a + 1);  // narrow range: nearest point
        if (a >= frames || b <= 0) continue;
        a = std::max<int64_t>(a, 0);
        b = std::min(b, frames);
        const int64_t p0 = a * points / frames, p1 = (b - 1) * points / frames;
        float l = 1.0f, h = -1.0f;
        for (int64_t p = p0; p <= p1; ++p) {
            l = std::min(l, peaks.min[static_cast<size_t>(p)]);
            h = std::max(h, peaks.max[static_cast<size_t>(p)]);
        }
        lo[static_cast<size_t>(c)] = l;
        hi[static_cast<size_t>(c)] = h;
    }
}

}  // namespace bigtop::io
