// LevelMeter.cpp: the status bar's output level meter.
#include "LevelMeter.h"

#include <cmath>

namespace bigtop::app {

namespace {

constexpr float kSegmentDb = -LevelMeter::kFloorDb / static_cast<float>(LevelMeter::kSegments);
constexpr float kScaleHeight = 12.0f;  // room for the dB labels under the bars

// Red for the top 2 dB, amber up from -6 dBFS, green below: the colour says how
// close to the limiter's ceiling the output is.
juce::Colour segmentColour(int index, bool lit) {
    const juce::Colour c = index >= LevelMeter::kSegments - 1 ? juce::Colour(0xffff5a5a)
                         : index >= LevelMeter::kSegments - 3 ? juce::Colour(0xffffb300)
                                                              : juce::Colour(0xff3ddc84);
    return lit ? c : c.withMultipliedBrightness(0.22f);  // unlit segments stay faintly visible, so the scale reads at rest
}

// Keeps the higher peak for kHoldMs, then lets it fall to the current level.
void hold(float now, float& held, juce::uint32& since, juce::uint32 nowMs) {
    if (now >= held || nowMs - since >= LevelMeter::kHoldMs) {
        held = now;
        since = nowMs;
    }
}

}  // namespace

int LevelMeter::litSegments(float level) noexcept {
    if (!(level > 0.0f)) return 0;  // also catches NaN
    const float db = 20.0f * std::log10(level);
    return juce::jlimit(0, kSegments, static_cast<int>(std::ceil((db - kFloorDb) / kSegmentDb)));
}

void LevelMeter::setPeaks(io::OutputPeaks peaks, juce::uint32 nowMs) {
    // Rise at once, fall smoothly: a bar that drops to the next peak each frame flickers.
    const float dt = lastMs_ == 0 ? 0.0f : static_cast<float>(nowMs - lastMs_) / 1000.0f;
    lastMs_ = nowMs == 0 ? 1 : nowMs;
    const float fall = std::pow(10.0f, -kFallDbPerSecond * dt / 20.0f);
    shown_.left = std::max(peaks.left, shown_.left * fall);
    shown_.right = std::max(peaks.right, shown_.right * fall);
    hold(shown_.left, held_.left, heldSinceL_, nowMs);
    hold(shown_.right, held_.right, heldSinceR_, nowMs);
    repaint();
}

void LevelMeter::paint(juce::Graphics& g) {
    auto r = getLocalBounds().toFloat().reduced(2.0f, 4.0f);
    const auto scale = r.removeFromBottom(kScaleHeight);
    const float laneHeight = (r.getHeight() - 2.0f) / 2.0f;
    const float segWidth = r.getWidth() / static_cast<float>(kSegments);
    const float levels[2][2] = {{shown_.left, held_.left}, {shown_.right, held_.right}};
    for (int c = 0; c < 2; ++c) {
        const float y = r.getY() + static_cast<float>(c) * (laneHeight + 2.0f);
        const int lit = litSegments(levels[c][0]);
        const int peak = litSegments(levels[c][1]) - 1;  // the held peak's segment, lit even after the bar falls
        for (int i = 0; i < kSegments; ++i) {
            g.setColour(segmentColour(i, i < lit || i == peak));
            g.fillRect(r.getX() + static_cast<float>(i) * segWidth, y, segWidth - 2.0f, laneHeight);
        }
    }
    // Labels at the levels an operator steers by, placed on the same dB-to-x scale as the segments.
    g.setColour(juce::Colour(0xff99aaaa));
    g.setFont(juce::FontOptions(11.0f));
    for (const int db : {-40, -20, -12, -6, 0}) {
        const float x = scale.getX() + scale.getWidth() * (static_cast<float>(db) - kFloorDb) / -kFloorDb;
        const auto just = db == -40 ? juce::Justification::centredLeft : db == 0 ? juce::Justification::centredRight : juce::Justification::centred;
        const float w = 24.0f;
        const float left = db == -40 ? x : db == 0 ? x - w : x - w / 2.0f;
        g.drawText(juce::String(db), juce::Rectangle<float>(left, scale.getY(), w, scale.getHeight()), just, false);
    }
}

}  // namespace bigtop::app
