// StreamingSourceTests.cpp: head in RAM, tail streamed, safe freeing.
// The fixture FLAC's samples encode their own frame index, so any frame read
// back can be checked (to well within one frame step). Tests that need the background read-ahead
// start the thread and poll with a deadline; tests that must not have it
// never start it. No audio hardware.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

#include <juce_audio_formats/juce_audio_formats.h>

#include "bigtop/Engine.h"
#include "bigtop/ShowController.h"
#include "bigtop/io/StreamingSource.h"

using namespace bigtop;
using namespace bigtop::io;

namespace {

/// A unique temp directory, deleted with the object.
struct TempDir {
    juce::File dir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                         .getChildFile("bigtop_test_" + juce::String(juce::Random::getSystemRandom().nextInt64()));
    TempDir() { dir.createDirectory(); }
    ~TempDir() { dir.deleteRecursively(); }
};

constexpr int64_t kFileFrames = 20 * 48000;
constexpr int64_t kHead = 5 * 48000;

/// Left sample of a frame; the right channel is its negative. Exact at 16 bit;
/// within 1e-6 at 24 bit (see same()).
float leftAt(int64_t frame) { return static_cast<float>(frame % 32768) / 32768.f; }

/// Writes the 20 s, 48 kHz, 24-bit stereo FLAC of the frame-index signal.
void writeFixture(const juce::File& f) {
    juce::AudioBuffer<float> buf(2, static_cast<int>(kFileFrames));
    for (int i = 0; i < buf.getNumSamples(); ++i) {
        buf.setSample(0, i, leftAt(i));
        buf.setSample(1, i, -leftAt(i));
    }
    std::unique_ptr<juce::OutputStream> out = std::make_unique<juce::FileOutputStream>(f);
    auto w = juce::FlacAudioFormat().createWriterFor(
        out, juce::AudioFormatWriterOptions{}.withSampleRate(48000.0).withNumChannels(2).withBitsPerSample(24));
    REQUIRE(w != nullptr);
    REQUIRE(w->writeFromAudioSampleBuffer(buf, 0, buf.getNumSamples()));
}

/// A show folder with audio/x.flac (file 1) used by step 1 from 0 s and
/// step 2 from 1 s, and a provider for it. Members are destroyed in reverse
/// order: the provider (and its readers) before the thread.
struct Fixture {
    TempDir tmp;
    juce::TimeSliceThread thread{"bigtop test read-ahead"};
    std::unique_ptr<StreamingSourceProvider> provider;
    Show show;
    Fixture() {
        REQUIRE(tmp.dir.getChildFile("audio").createDirectory());
        writeFixture(tmp.dir.getChildFile("audio/x.flac"));
        show.files.push_back({1, "audio/x.flac", 0.0, ""});
        Sound fromStart;
        fromStart.file = 1;
        Sound fromOne = fromStart;
        fromOne.startSeconds = 1.0;
        show.steps.push_back(makeStep(1, show.settings, fromStart));
        show.steps.push_back(makeStep(2, show.settings, fromOne));
        provider = std::make_unique<StreamingSourceProvider>(tmp.dir, thread);
        provider->setShow(show);
    }
};

/// Equal within 1e-6, far below the 1/32768 step between neighbouring frames.
/// Not exact: JUCE's FLAC writer scales positive samples by 0x7fffff/0x800000.
bool same(float a, float b) { return std::abs(a - b) <= 1e-6f; }

/// Reads [from, to) in engine-sized chunks; counts frames that differ from the file.
int64_t mismatches(SoundSource& s, int64_t from, int64_t to) {
    float l[512], r[512];
    int64_t bad = 0;
    for (int64_t f = from; f < to; f += 512) {
        const int n = static_cast<int>(std::min<int64_t>(512, to - f));
        s.read(f, l, r, n);
        for (int i = 0; i < n; ++i)
            if (!same(l[i], leftAt(f + i)) || !same(r[i], -leftAt(f + i))) ++bad;
    }
    return bad;
}

/// Polls until @p ok returns true or 2 s pass; returns the last result.
template <typename F>
bool pollFor(F ok) {
    const auto deadline = juce::Time::getMillisecondCounter() + 2000;
    while (!ok()) {
        if (juce::Time::getMillisecondCounter() > deadline) return false;
        juce::Thread::sleep(1);
    }
    return true;
}

/// Reads [from, to) like an engine that waits for each chunk: a chunk that
/// comes back wrong (late tail data) is re-read until right, up to 2 s each.
bool readWhenReady(SoundSource& s, int64_t from, int64_t to) {
    for (int64_t f = from; f < to; f += 512) {
        const int64_t end = std::min<int64_t>(f + 512, to);
        if (!pollFor([&] { return mismatches(s, f, end) == 0; })) return false;
    }
    return true;
}

Snapshot snapAt(int64_t frames) {
    Snapshot s;
    s.framesProcessed = frames;
    return s;
}

}  // namespace

TEST_CASE("Streaming: a start plays its first 5 s from the head even if the tail never loads") {
    Fixture fx;  // read-ahead thread never started
    SoundSource* s = fx.provider->sourceFor(1, 1, 0);
    REQUIRE(s != nullptr);
    CHECK(s->lengthFrames() == kFileFrames);
    CHECK(mismatches(*s, 0, kHead) == 0);
    CHECK(fx.provider->dropouts() == 0);
}

TEST_CASE("Streaming: the tail continues seamlessly after the head") {
    Fixture fx;
    fx.thread.startThread();
    SoundSource* s = fx.provider->sourceFor(1, 1, 0);
    REQUIRE(s != nullptr);
    // One pass from the last head chunk across the boundary to 1 s into the
    // tail, retried until the read-ahead has it all; the pass that succeeds
    // must also have caused no dropout. Each pass ends with a head read (like
    // a loop wrap) so the read-ahead waits at the head's end between passes;
    // otherwise it would chase the end of the last pass and evict the block
    // the next pass starts in.
    const bool clean = pollFor([&] {
        const uint32_t before = fx.provider->dropouts();
        const bool right = mismatches(*s, kHead - 512, kHead + 48000) == 0;
        mismatches(*s, 0, 512);
        return right && fx.provider->dropouts() == before;
    });
    CHECK(clean);
}

TEST_CASE("Streaming: a loop wraps from its end back to the head without silence") {
    Fixture fx;
    fx.thread.startThread();
    SoundSource* s = fx.provider->sourceFor(2, 1, 48000);
    REQUIRE(s != nullptr);
    REQUIRE(readWhenReady(*s, 48000, 10 * 48000));
    // The wrap: back to the loop start, which the head serves at once.
    const uint32_t before = fx.provider->dropouts();
    CHECK(mismatches(*s, 48000, 48000 + kHead) == 0);
    CHECK(fx.provider->dropouts() == before);
    // The head reads pointed the tail back at the head's end. The read-ahead
    // last sat near 10 s, so without that hint the first tail read after the
    // head is always late. Give it 2 s of head playing time (a real head
    // plays 5 s; head reads leave the read-ahead where it is; 2 s leaves room
    // for loaded CI runners), then the first try at the tail must succeed
    // with no dropout.
    const auto until = juce::Time::getMillisecondCounter() + 2000;
    while (juce::Time::getMillisecondCounter() < until) {
        mismatches(*s, 48000, 48000 + 512);
        juce::Thread::sleep(1);
    }
    CHECK(mismatches(*s, 48000 + kHead, 48000 + kHead + 512) == 0);
    CHECK(fx.provider->dropouts() == before);
}

TEST_CASE("Streaming: a seek into the tail reads silence first, then the right audio") {
    Fixture fx;
    fx.thread.startThread();
    SoundSource* s = fx.provider->sourceFor(1, 1, 0);
    REQUIRE(s != nullptr);
    float l[512], r[512];
    s->read(0, l, r, 512);
    const uint32_t before = fx.provider->dropouts();
    // 15 s is far outside the read-ahead window at the head's end.
    constexpr int64_t target = 15 * 48000;
    s->read(target, l, r, 512);
    bool silent = true;
    for (int i = 0; i < 512; ++i) silent = silent && l[i] == 0.f && r[i] == 0.f;
    CHECK(silent);
    CHECK(fx.provider->dropouts() == before + 1);
    CHECK(pollFor([&] { return mismatches(*s, target, target + 512) == 0; }));
}

TEST_CASE("Streaming: sources are freed after 1 s of engine audio without a read, never while pending") {
    Fixture fx;
    SoundSource* a = fx.provider->sourceFor(1, 1, 0);
    SoundSource* b = fx.provider->sourceFor(2, 1, 48000);
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    CHECK(fx.provider->liveSources() == 2);
    fx.provider->collect(snapAt(0));

    // 1 s of audio: a was read, b's key is pending (start delay, no reads).
    float l[512], r[512];
    a->read(0, l, r, 512);
    Snapshot snap = snapAt(48000);
    snap.numVoices = 1;
    snap.voices[0].key = 2;
    snap.voices[0].pending = true;
    fx.provider->collect(snap);
    CHECK(fx.provider->liveSources() == 2);

    // Another second: neither read, nothing pending.
    fx.provider->collect(snapAt(96000));
    CHECK(fx.provider->liveSources() == 0);
}

TEST_CASE("Streaming: a delay ending exactly at a block end never frees the unread source") {
    // The engine does not read in the block where the delay runs out at the
    // block's end: the snapshot then shows the voice no longer pending with
    // the source never read. Collected after every block, as the app does.
    Fixture fx;
    auto engine = std::make_unique<Engine>(48000.0);
    constexpr int kBlock = 480;
    constexpr int kDelayBlocks = 150;  // 1.5 s, longer than the 1 s idle rule
    SoundSource* s = fx.provider->sourceFor(1, 1, 0);
    REQUIRE(s != nullptr);
    StepCommand cmd;
    cmd.hasStart = true;
    cmd.start = {1, s, 0, kFileFrames, false, 1.0f, 0, int64_t{kDelayBlocks} * kBlock, false, {}};
    REQUIRE(engine->post(cmd));
    float l[kBlock], r[kBlock];
    for (int b = 0; b < kDelayBlocks; ++b) {
        engine->process(l, r, kBlock);
        fx.provider->collect(engine->snapshot());
        // Stop before process() would read a freed source.
        REQUIRE(fx.provider->liveSources() == 1);
    }
    CHECK_FALSE(engine->snapshot().voices[0].pending);
    engine->process(l, r, kBlock);
    int64_t bad = 0;
    for (int i = 0; i < kBlock; ++i)
        if (!same(l[i], leftAt(i)) || !same(r[i], -leftAt(i))) ++bad;
    CHECK(bad == 0);
}

TEST_CASE("Streaming: nothing is freed while no audio runs") {
    Fixture fx;
    REQUIRE(fx.provider->sourceFor(1, 1, 0) != nullptr);
    for (int i = 0; i < 100; ++i) fx.provider->collect(snapAt(5000));
    CHECK(fx.provider->liveSources() == 1);
}

TEST_CASE("Streaming: a file missing from audio/ gives nullptr, not a crash") {
    Fixture fx;
    Show show = fx.show;
    show.files.push_back({7, "audio/gone.flac", 0.0, ""});
    Sound gone;
    gone.file = 7;
    show.steps.insert(show.steps.begin(), makeStep(3, show.settings, gone));
    fx.provider->setShow(show);
    CHECK(fx.provider->fileUsable(1));
    CHECK_FALSE(fx.provider->fileUsable(7));
    CHECK(fx.provider->fileLengthFrames(1) == kFileFrames);
    CHECK(fx.provider->fileLengthFrames(7) == 0);
    CHECK(fx.provider->sourceFor(3, 7, 0) == nullptr);

    auto engine = std::make_unique<Engine>(48000.0);
    ShowController controller(*engine, *fx.provider);
    controller.setShow(show);
    CHECK(controller.go(0.0) == GoResult::MissingFile);
}

namespace {

/// Tail block 20 of the fixture (frames 655360..688128), a stretch the head
/// does not cover; the chunk lies inside one block so it is never half loaded.
/// The signal is 0.5 there, far from 0, so a missing rise shows.
constexpr int64_t kLate = 20 * 32768 + 16384;
constexpr int kRise = 480;  // kDeclickSeconds * 48000

/// Reads 1024 frames at @p at until the first read that returns real data
/// (the last sample is non-zero); that read is the one that must rise.
bool firstRealRead(SoundSource& s, int64_t at, float* l, float* r) {
    return pollFor([&] {
        s.read(at, l, r, 1024);
        return l[1023] != 0.f;
    });
}

/// Checks the rise in a first real read: 0 at the start, full at kRise, and no
/// step between neighbours above 3 * (full / kRise) (the signal's own slope,
/// 1/32768 per frame, is far below that; 3 leaves room for rounding).
void checkRise(const float* l, int64_t at) {
    CHECK(std::abs(l[0]) < 1e-6f);
    CHECK(same(l[kRise], leftAt(at + kRise)));
    const float full = leftAt(at);
    for (int i = 1; i < 1024; ++i) CHECK(std::abs(l[i] - l[i - 1]) <= 3.0f * full / kRise);
}

}  // namespace

TEST_CASE("Streaming: data after a late stretch fades in over 10 ms") {
    Fixture fx;  // read-ahead thread held back: the tail is late
    SoundSource* s = fx.provider->sourceFor(1, 1, 0);
    REQUIRE(s != nullptr);
    float l[1024], r[1024];
    s->read(kLate, l, r, 1024);
    s->read(kLate + 1024, l, r, 1024);  // two silent blocks
    fx.thread.startThread();            // release the tail
    REQUIRE(firstRealRead(*s, kLate, l, r));
    checkRise(l, kLate);
}

TEST_CASE("Streaming: a seek into the tail rises without a step") {
    Fixture fx;
    fx.thread.startThread();
    SoundSource* s = fx.provider->sourceFor(1, 1, 0);
    REQUIRE(s != nullptr);
    float l[1024], r[1024];
    s->read(0, l, r, 512);  // playing in the head, then a reposition
    REQUIRE(firstRealRead(*s, kLate, l, r));
    checkRise(l, kLate);
}

TEST_CASE("Streaming: head reads never fade") {
    Fixture fx;
    SoundSource* s = fx.provider->sourceFor(2, 1, 48000);
    REQUIRE(s != nullptr);
    float l[512], r[512];
    s->read(48000, l, r, 512);
    CHECK(same(l[0], leftAt(48000)));  // full value from the first frame
    CHECK(mismatches(*s, 48000, 48000 + 4096) == 0);
}

TEST_CASE("Streaming: collect frees nothing while paused") {
    Fixture fx;
    REQUIRE(fx.provider->sourceFor(1, 1, 0) != nullptr);
    fx.provider->collect(snapAt(0));
    Snapshot paused = snapAt(2 * 48000);  // 2 s of engine audio, never read
    paused.paused = true;
    fx.provider->collect(paused);
    CHECK(fx.provider->liveSources() == 1);
    // After the resume the idle time counts from the pause's end, so a voice
    // that has not yet had its first block back is not caught out.
    fx.provider->collect(snapAt(2 * 48000));
    CHECK(fx.provider->liveSources() == 1);
    fx.provider->collect(snapAt(3 * 48000));
    CHECK(fx.provider->liveSources() == 0);
}

TEST_CASE("Streaming: a partly loaded chunk after silence also rises from 0") {
    // The chunk starts 512 frames before a tail block boundary (688128), so
    // while the next block is not loaded it is real data followed by zeros.
    // Whether the read-ahead delivers the whole chunk or only its first half,
    // the first real frame must be 0 and rise without a step. The cliff to
    // zeros at the boundary is not checked (nothing can hide missing data).
    Fixture fx;  // read-ahead held back
    SoundSource* s = fx.provider->sourceFor(1, 1, 0);
    REQUIRE(s != nullptr);
    constexpr int64_t at = 21 * 32768 - 512;
    float l[1024], r[1024];
    s->read(at, l, r, 1024);  // silent: now a rise is pending
    fx.thread.startThread();
    REQUIRE(pollFor([&] {
        s->read(at, l, r, 1024);
        return l[511] != 0.f;
    }));
    CHECK(std::abs(l[0]) < 1e-6f);
    const float full = leftAt(at);
    for (int i = 1; i < 512; ++i) CHECK(std::abs(l[i] - l[i - 1]) <= 3.0f * full / kRise);
    CHECK(same(l[kRise], leftAt(at + kRise)));
}

// Two late chunks in a row: the first ends with real data whose rise is still
// running, the second starts with that data and ends in a missing block. The
// second must carry the rise on, not restart it from 0 (a downward click).
TEST_CASE("Streaming: a dropout mid-rise does not restart the rise") {
    constexpr int kFrames = 100;
    DataRise rise{kFrames};
    std::vector<float> l(160), r(160);
    std::fill(l.begin(), l.begin() + 120, 0.0f);  // chunk 1: 120 zeros, then 40 of data
    std::fill(l.begin() + 120, l.end(), 1.0f);
    r = l;
    rise.late(l.data(), r.data(), 160);
    std::vector<float> l2(160, 1.0f), r2(160, 1.0f);  // chunk 2: 100 of data, then 60 zeros
    std::fill(l2.begin() + 100, l2.end(), 0.0f);
    r2 = l2;
    rise.late(l2.data(), r2.data(), 160);
    std::vector<float> all(l.begin(), l.end());
    all.insert(all.end(), l2.begin(), l2.begin() + 100);  // up to the missing block
    for (size_t i = 1; i < all.size(); ++i) CHECK(std::abs(all[i] - all[i - 1]) <= 1.0f / kFrames + 1e-6f);
    CHECK(all.back() == 1.0f);  // the rise finished in chunk 2
}
