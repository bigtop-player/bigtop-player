// TestSources.h: test-only helpers for the engine and controller tests. Simple
// in-memory SoundSources with predictable samples, a block-by-block render
// loop, a shorthand for the most common StepCommand (start one sound, change
// nothing), a map-backed SourceProvider and two ready-made shows.
#pragma once

#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <tuple>
#include <vector>

#include "bigtop/Engine.h"
#include "bigtop/Show.h"
#include "bigtop/ShowController.h"
#include "bigtop/SoundSource.h"

namespace bigtop {

// Every sample is `value` on both channels; handy for checking gains and sums.
class ConstSource : public SoundSource {
public:
    explicit ConstSource(int64_t length, float value = 1.0f) : length_(length), value_(value) {}
    int64_t lengthFrames() const noexcept override { return length_; }
    void read(int64_t, float* left, float* right, int frames) noexcept override {
        std::fill(left, left + frames, value_);
        std::fill(right, right + frames, value_);
    }

private:
    int64_t length_;
    float value_;
};

// Sample value equals its frame index, so a test can read the play position
// straight off the output (exact in float up to 2^24 frames).
class RampSource : public SoundSource {
public:
    explicit RampSource(int64_t length) : length_(length) {}
    int64_t lengthFrames() const noexcept override { return length_; }
    void read(int64_t startFrame, float* left, float* right, int frames) noexcept override {
        for (int i = 0; i < frames; ++i) left[i] = right[i] = static_cast<float>(startFrame + i);
    }

private:
    int64_t length_;
};

// Runs the engine for `frames` frames in blocks of `blockSize` (like an audio
// device would) and returns the left channel.
inline std::vector<float> render(Engine& e, int frames, int blockSize = 64) {
    std::vector<float> left(static_cast<size_t>(frames)), right(static_cast<size_t>(frames));
    for (int off = 0; off < frames; off += blockSize) {
        const int n = std::min(blockSize, frames - off);
        e.process(left.data() + off, right.data() + off, n);
    }
    return left;
}

// A step that starts the whole of `src` at gain 1 now, with no fade, no
// delay, no changes and Others = Keep.
inline StepCommand startCmd(VoiceKey key, SoundSource& src) {
    StepCommand c;
    c.hasStart = true;
    c.start.key = key;
    c.start.source = &src;
    c.start.startFrame = 0;
    c.start.endFrame = src.lengthFrames();
    c.start.loop = false;
    c.start.gain = 1.0f;
    c.start.fadeInFrames = 0;
    c.start.delayFrames = 0;
    c.start.drivesTimecode = false;
    c.start.timecodeStart = {};
    return c;
}

// SourceProvider backed by a map. Sources live on the heap so their addresses
// stay stable while the engine holds raw pointers to them; replacing an id
// that is still playing would leave the engine with a dangling pointer.
class MapProvider : public SourceProvider {
public:
    void add(FileId id, ConstSource src) { sources_[id] = std::make_unique<ConstSource>(src); }
    SoundSource* sourceFor(VoiceKey, FileId file, int64_t) noexcept override {
        const auto it = sources_.find(file);
        return it == sources_.end() ? nullptr : it->second.get();
    }

private:
    std::map<FileId, std::unique_ptr<ConstSource>> sources_;
};

// Forwards to a MapProvider and records (key, file, startFrame) of every call.
class RecordingProvider : public SourceProvider {
public:
    explicit RecordingProvider(MapProvider& inner) : inner_(inner) {}
    SoundSource* sourceFor(VoiceKey key, FileId file, int64_t startFrame) noexcept override {
        calls.emplace_back(key, file, startFrame);
        return inner_.sourceFor(key, file, startFrame);
    }
    std::vector<std::tuple<VoiceKey, FileId, int64_t>> calls;

private:
    MapProvider& inner_;
};

// Files 1 and 2, 10 s each at 1 kHz (value 1); file 2 trimmed -6.0206 dB (x0.5).
// Step 1 plays file 1, step 2 plays file 2, both with makeStep defaults.
inline Show twoTrackShow() {
    Show s;
    s.files = {{1, "a.wav", 0.0, ""}, {2, "b.wav", -6.0206, ""}};
    s.steps.push_back(makeStep(1, s.settings, Sound{.file = 1}));
    s.steps.push_back(makeStep(2, s.settings, Sound{.file = 2}));
    return s;
}
inline void addTwoTrackSources(MapProvider& p) {
    p.add(1, ConstSource(10000));
    p.add(2, ConstSource(10000));
}

// The spec's worked example. Steps 3 Music A (file 10), 4 Atmo (file 11, loop,
// fade in 3 s, others FadeOut 5 s), 5 (no sound; Atmo Level -12 dB in 2 s),
// 6 Music B (file 12, fade in 5 s; Atmo Level -15 dB in 3 s; others Keep),
// 7 (no sound; Atmo FadeOut 8 s; Music B Keep). Steps without an explicit
// others rule here keep everything.
inline Show specExampleShow() {
    Show s;
    s.files = {{10, "music_a.wav", 0.0, ""}, {11, "atmo.wav", 0.0, ""}, {12, "music_b.wav", 0.0, ""}};
    Step a = makeStep(3, s.settings, Sound{.file = 10});
    a.name = "Music A";
    Step atmo = makeStep(4, s.settings, Sound{.file = 11, .fadeInSeconds = 3.0, .loop = true});
    atmo.name = "Atmo";
    atmo.others = {ChangeKind::FadeOut, 5.0};
    Step dip = makeStep(5, s.settings, std::nullopt);
    dip.changes = {{4, ChangeKind::Level, -12.0, 2.0}};
    Step b = makeStep(6, s.settings, Sound{.file = 12, .fadeInSeconds = 5.0});
    b.name = "Music B";
    b.changes = {{4, ChangeKind::Level, -15.0, 3.0}};
    b.others = {};
    Step out = makeStep(7, s.settings, std::nullopt);
    out.changes = {{4, ChangeKind::FadeOut, 0.0, 8.0}, {6, ChangeKind::Keep, 0.0, 0.0}};
    s.steps = {a, atmo, dip, b, out};
    return s;
}
// 60 s ConstSources (value 1) at 1 kHz for every file of specExampleShow().
inline void addSpecExampleSources(MapProvider& p) {
    for (FileId id : {10u, 11u, 12u}) p.add(id, ConstSource(60000));
}

}  // namespace bigtop
