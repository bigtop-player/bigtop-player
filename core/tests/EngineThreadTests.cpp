// EngineThreadTests.cpp: the engine's thread contract under load. One thread
// plays the audio-device role (process() in 256-frame blocks), the test thread
// plays the control thread (post()) and a UI reader (snapshot()) at once.
// Run under ThreadSanitizer (BIGTOP_TSAN, CI "tsan" job) this also proves the
// queue and the snapshot seqlock are race-free.
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <random>
#include <thread>
#include <vector>

#include "TestSources.h"

using namespace bigtop;

TEST_CASE("Engine survives concurrent posting, processing and snapshot reads", "[stress]") {
    constexpr double kRate = 48000.0;
    constexpr int kBlock = 256;
    constexpr VoiceKey kKeys = 40;  // more keys than voices, so starts get dropped too

    // Sources outlive the engine: voices hold raw pointers to them.
    std::vector<std::unique_ptr<ConstSource>> sources;
    for (int i = 0; i < 4; ++i) sources.push_back(std::make_unique<ConstSource>(static_cast<int64_t>(kRate) * (i + 1), 0.1f));
    auto engine = std::make_unique<Engine>(kRate);  // ~30 KB, keep it off the stack

    std::atomic<bool> done{false};
    std::thread audio([&] {
        std::vector<float> left(kBlock), right(kBlock);
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        while (std::chrono::steady_clock::now() < end) engine->process(left.data(), right.data(), kBlock);
        done.store(true);
    });

    // Fixed seed so a failure reproduces with the same command mix.
    std::mt19937 rng(20261117);
    auto pick = [&](int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); };
    // Failures are counted, not asserted, while the audio thread runs: a
    // throwing REQUIRE here would destroy a joinable std::thread.
    int badCount = 0, zeroKey = 0, framesWentBack = 0, snapshots = 0;
    int64_t lastFrames = 0;
    while (!done.load()) {
        const VoiceKey key = static_cast<VoiceKey>(pick(1, static_cast<int>(kKeys)));
        switch (pick(0, 4)) {
        case 0: {
            auto c = startCmd(key, *sources[static_cast<size_t>(pick(0, 3))]);
            c.start.loop = pick(0, 1) == 1;
            c.start.fadeInFrames = pick(0, 4800);
            c.start.delayFrames = pick(0, 2) == 0 ? pick(0, 4800) : 0;
            c.numChanges = 1;
            c.changes[0] = {static_cast<VoiceKey>(pick(1, static_cast<int>(kKeys))),
                            pick(0, 1) == 0 ? ChangeKind::Level : ChangeKind::FadeOut, 0.5f, pick(0, 4800)};
            if (pick(0, 1) == 1) { c.othersKind = ChangeKind::FadeOut; c.othersFrames = pick(0, 9600); }
            engine->post(c);
            break;
        }
        case 1: engine->post(SeekCommand{key, pick(0, 96000)}); break;
        case 2: engine->post(TrimCommand{key, static_cast<float>(pick(0, 200)) / 100.0f}); break;
        case 3: if (pick(0, 9) == 0) engine->post(FadeOutAllCommand{pick(0, 4800)}); break;
        // Pause and resume often enough that voices freeze, thaw and get
        // seeked, trimmed, kept and faded out while paused.
        default: if (pick(0, 4) == 0) engine->post(PauseCommand{pick(0, 1) == 1}); else engine->post(KeepTrimCommand{key}); break;
        }
        // post() may refuse when the queue is full; that is allowed and harmless.

        const Snapshot s = engine->snapshot();
        ++snapshots;
        if (s.numVoices < 0 || s.numVoices > kMaxVoices) {
            ++badCount;
        } else {
            for (int i = 0; i < s.numVoices; ++i)
                if (s.voices[i].key == 0) ++zeroKey;
        }
        if (s.framesProcessed < lastFrames) ++framesWentBack;
        lastFrames = s.framesProcessed;
    }
    audio.join();

    CHECK(snapshots > 0);
    CHECK(lastFrames > 0);
    CHECK(badCount == 0);
    CHECK(zeroKey == 0);
    CHECK(framesWentBack == 0);
}
