// ImporterTests.cpp: WAV/FLAC/MP3 import to 48 kHz stereo 24-bit FLAC, hashing,
// target naming. WAV and FLAC inputs are generated here; the MP3s come from
// fixtures/ (see fixtures/README.md). No audio hardware, no message loop.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>

#include <cmath>
#include <cstring>
#include <memory>
#include <string_view>

#include <juce_audio_formats/juce_audio_formats.h>

#include "bigtop/io/Importer.h"

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

constexpr double kPi = 3.14159265358979323846;

/// Channel ch of the test signal: a 440 Hz sine, amplitude 0.5 on channel 0
/// and a different amplitude per further channel so channel order is checkable.
float sample(int ch, int i, double rate) {
    const double amp = ch == 0 ? 0.5 : 0.1 * (ch + 1);
    return static_cast<float>(amp * std::sin(2.0 * kPi * 440.0 * i / rate));
}

/// Reader for any file the tests produce or consume (WAV, MP3, FLAC).
std::unique_ptr<juce::AudioFormatReader> open(const juce::File& f) {
    juce::AudioFormatManager m;
    m.registerFormat(new juce::WavAudioFormat, true);
    m.registerFormat(new juce::MP3AudioFormat, false);
    m.registerFormat(new juce::FlacAudioFormat, false);
    return std::unique_ptr<juce::AudioFormatReader>(m.createReaderFor(f));
}

juce::AudioBuffer<float> readAll(const juce::File& f) {
    auto r = open(f);
    REQUIRE(r != nullptr);
    juce::AudioBuffer<float> buf(static_cast<int>(r->numChannels), static_cast<int>(r->lengthInSamples));
    REQUIRE(r->read(&buf, 0, buf.getNumSamples(), 0, true, true));
    return buf;
}

/// Writes a 24-bit WAV of the test signal and returns what was written
/// (already 24-bit quantised, by reading the file back).
juce::AudioBuffer<float> writeWav(const juce::File& f, double rate, int channels, int frames) {
    juce::AudioBuffer<float> buf(channels, frames);
    for (int ch = 0; ch < channels; ++ch)
        for (int i = 0; i < frames; ++i) buf.setSample(ch, i, sample(ch, i, rate));
    {
        std::unique_ptr<juce::OutputStream> out = std::make_unique<juce::FileOutputStream>(f);
        auto w = juce::WavAudioFormat().createWriterFor(
            out, juce::AudioFormatWriterOptions{}.withSampleRate(rate).withNumChannels(channels).withBitsPerSample(24));
        REQUIRE(w != nullptr);
        REQUIRE(w->writeFromAudioSampleBuffer(buf, 0, frames));
    }
    return readAll(f);
}

/// Writes a 24-bit FLAC of the test signal and returns what was written.
juce::AudioBuffer<float> writeFlac(const juce::File& f, double rate, int channels, int frames) {
    juce::AudioBuffer<float> buf(channels, frames);
    for (int ch = 0; ch < channels; ++ch)
        for (int i = 0; i < frames; ++i) buf.setSample(ch, i, sample(ch, i, rate));
    {
        std::unique_ptr<juce::OutputStream> out = std::make_unique<juce::FileOutputStream>(f);
        auto w = juce::FlacAudioFormat().createWriterFor(
            out, juce::AudioFormatWriterOptions{}.withSampleRate(rate).withNumChannels(channels).withBitsPerSample(24));
        REQUIRE(w != nullptr);
        REQUIRE(w->writeFromAudioSampleBuffer(buf, 0, frames));
    }
    return readAll(f);
}

/// Best lag (frames) of the first 0.1 s of `a` against `b`. The sine repeats
/// every ~109 frames, so only lags within half a period (54) are unambiguous;
/// an untrimmed MP3 delay (1105 frames at 44.1 kHz) shows up as a lag of about 15.
int bestLag(const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b) {
    int best = 0;
    double bestC = -1e30;
    for (int lag = -54; lag <= 54; ++lag) {
        double c = 0.0;
        for (int i = 100; i < 4800; ++i) c += static_cast<double>(a.getSample(0, i)) * b.getSample(0, i + lag);
        if (c > bestC) { bestC = c; best = lag; }
    }
    return best;
}

double rmsDb(const juce::AudioBuffer<float>& b, int ch, int start, int len) {
    return juce::Decibels::gainToDecibels(static_cast<double>(b.getRMSLevel(ch, start, len)));
}

juce::File fixture(const char* name) { return juce::File(BIGTOP_IO_FIXTURES).getChildFile(name); }

/// Checks the failure contract: an error message and nothing left behind.
void checkFailedCleanly(const juce::File& source, const juce::File& target) {
    const auto r = convertToShowAudio(source, target);
    CHECK_FALSE(r.ok);
    CHECK_FALSE(r.error.empty());
    CHECK_FALSE(target.exists());
    CHECK_FALSE(target.withFileExtension(".flac.tmp").exists());
}

}  // namespace

TEST_CASE("44.1 kHz mono WAV becomes 48 kHz stereo 24-bit FLAC of the same duration and level") {
    TempDir t;
    const auto src = t.dir.getChildFile("in.wav"), dst = t.dir.getChildFile("out.flac");
    const auto in = writeWav(src, 44100.0, 1, 44100);
    const auto r = convertToShowAudio(src, dst);
    REQUIRE(r.ok);
    CHECK_FALSE(dst.withFileExtension(".flac.tmp").exists());
    auto reader = open(dst);
    REQUIRE(reader != nullptr);
    CHECK(reader->sampleRate == 48000.0);
    CHECK(reader->numChannels == 2);
    CHECK(reader->bitsPerSample == 24);
    CHECK(std::abs(reader->lengthInSamples - 48000) <= 2);
    const auto out = readAll(dst);
    bool same = true;
    for (int i = 0; i < out.getNumSamples(); ++i) same = same && out.getSample(0, i) == out.getSample(1, i);
    CHECK(same);
    // middle 0.8 s, away from the filter's edges
    CHECK(std::abs(rmsDb(out, 0, 4800, 38400) - rmsDb(in, 0, 4410, 35280)) < 0.1);
    // no time shift: the output is the same sine sampled at 48 kHz
    float worst = 0.0f;
    for (int i = 4800; i < 43200; ++i) worst = std::max(worst, std::abs(out.getSample(0, i) - sample(0, i, 48000.0)));
    CHECK(worst < 1e-3f);
}

TEST_CASE("44.1 kHz stereo WAV keeps both channels apart when resampled") {
    TempDir t;
    const auto src = t.dir.getChildFile("in.wav"), dst = t.dir.getChildFile("out.flac");
    writeWav(src, 44100.0, 2, 22050);
    REQUIRE(convertToShowAudio(src, dst).ok);
    const auto out = readAll(dst);
    REQUIRE(out.getNumChannels() == 2);
    CHECK(std::abs(out.getNumSamples() - 24000) <= 2);
    // channel 0 has amplitude 0.5, channel 1 has 0.2 (see sample())
    CHECK(std::abs(out.getMagnitude(0, 2400, 19200) - 0.5f) < 0.01f);
    CHECK(std::abs(out.getMagnitude(1, 2400, 19200) - 0.2f) < 0.01f);
}

TEST_CASE("An existing target is never overwritten") {
    TempDir t;
    const auto src = t.dir.getChildFile("in.wav"), dst = t.dir.getChildFile("out.flac");
    writeWav(src, 48000.0, 1, 480);
    REQUIRE(dst.replaceWithText("keep me"));
    const auto r = convertToShowAudio(src, dst);
    CHECK_FALSE(r.ok);
    CHECK_FALSE(r.error.empty());
    CHECK(dst.loadFileAsString() == "keep me");
    CHECK_FALSE(dst.withFileExtension(".flac.tmp").exists());
}

TEST_CASE("48 kHz stereo WAV is not resampled") {
    TempDir t;
    const auto src = t.dir.getChildFile("in.wav"), dst = t.dir.getChildFile("out.flac");
    const auto in = writeWav(src, 48000.0, 2, 24000);
    REQUIRE(convertToShowAudio(src, dst).ok);
    const auto out = readAll(dst);
    REQUIRE(out.getNumChannels() == 2);
    REQUIRE(out.getNumSamples() == in.getNumSamples());
    float worst = 0.0f;
    for (int ch = 0; ch < 2; ++ch)
        for (int i = 0; i < in.getNumSamples(); ++i)
            worst = std::max(worst, std::abs(out.getSample(ch, i) - in.getSample(ch, i)));
    CHECK(worst <= 1.0f / (1 << 23));
}

TEST_CASE("MP3 imports") {
    const auto mp3 = fixture("sine440_44k1_mono.mp3");
    if (!mp3.existsAsFile()) SKIP("MP3 fixture not generated yet: see io/tests/fixtures/README.md");
    TempDir t;
    const auto dst = t.dir.getChildFile("out.flac");
    REQUIRE(convertToShowAudio(mp3, dst).ok);
    const auto out = readAll(dst);
    CHECK(open(dst)->sampleRate == 48000.0);
    CHECK(std::abs(out.getNumSamples() / 48000.0 - 2.0) <= 0.05);
    const float peak = out.getMagnitude(0, 0, out.getNumSamples());
    CHECK(peak >= 0.45f);
    CHECK(peak <= 0.55f);
}

TEST_CASE("FLAC at 48 kHz imports") {
    TempDir t;
    const auto src = t.dir.getChildFile("in.flac"), dst = t.dir.getChildFile("out.flac");
    const auto in = writeFlac(src, 48000.0, 2, 24000);
    REQUIRE(convertToShowAudio(src, dst).ok);
    const auto out = readAll(dst);
    REQUIRE(out.getNumChannels() == 2);
    REQUIRE(out.getNumSamples() == in.getNumSamples());
    float worst = 0.0f;
    for (int ch = 0; ch < 2; ++ch)
        for (int i = 0; i < in.getNumSamples(); ++i)
            worst = std::max(worst, std::abs(out.getSample(ch, i) - in.getSample(ch, i)));
    CHECK(worst <= 1.0f / (1 << 23));
}

TEST_CASE("FLAC at 44.1 kHz imports at 48 kHz") {
    TempDir t;
    const auto src = t.dir.getChildFile("in.flac"), dst = t.dir.getChildFile("out.flac");
    const auto in = writeFlac(src, 44100.0, 1, 44100);
    REQUIRE(convertToShowAudio(src, dst).ok);
    auto reader = open(dst);
    REQUIRE(reader != nullptr);
    CHECK(reader->sampleRate == 48000.0);
    CHECK(reader->numChannels == 2);
    CHECK(std::abs(reader->lengthInSamples - 48000) <= 2);
    const auto out = readAll(dst);
    CHECK(std::abs(rmsDb(out, 0, 4800, 38400) - rmsDb(in, 0, 4410, 35280)) < 0.1);
    float worst = 0.0f;
    for (int i = 4800; i < 43200; ++i) worst = std::max(worst, std::abs(out.getSample(0, i) - sample(0, i, 48000.0)));
    CHECK(worst < 1e-3f);
}

TEST_CASE("An MP3 with a LAME tag imports without its encoder delay") {
    const auto mp3 = fixture("sine440_44k1_stereo_lame.mp3"), wav = fixture("sine440_44k1_stereo_lame.wav");
    if (!mp3.existsAsFile() || !wav.existsAsFile()) SKIP("LAME fixtures not generated yet: see io/tests/fixtures/README.md");
    TempDir t;
    const auto fromMp3 = t.dir.getChildFile("mp3.flac"), fromWav = t.dir.getChildFile("wav.flac");
    REQUIRE(convertToShowAudio(mp3, fromMp3).ok);
    REQUIRE(convertToShowAudio(wav, fromWav).ok);  // the reference: same audio, no encoder involved
    const auto a = readAll(fromMp3), b = readAll(fromWav);
    CHECK(std::abs(a.getNumSamples() - b.getNumSamples()) <= 2);
    CHECK(std::abs(bestLag(a, b)) <= 2);
}

TEST_CASE("A LAME MP3 behind a large ID3v2 tag is still trimmed") {
    const auto mp3 = fixture("sine440_44k1_stereo_lame.mp3"), wav = fixture("sine440_44k1_stereo_lame.wav");
    if (!mp3.existsAsFile() || !wav.existsAsFile()) SKIP("LAME fixtures not generated yet: see io/tests/fixtures/README.md");
    TempDir t;
    juce::MemoryBlock bytes, file;
    REQUIRE(mp3.loadFileAsData(bytes));
    // The fixture's own 44-byte ID3v2 tag is replaced by one with 64 KB of
    // non-zero "cover art", like a tagged release; sizes are syncsafe.
    constexpr size_t kBody = 64 * 1024, kOwnTag = 44;
    const unsigned char hdr[10] = {'I', 'D', '3', 4, 0, 0, (kBody >> 21) & 0x7f, (kBody >> 14) & 0x7f, (kBody >> 7) & 0x7f, kBody & 0x7f};
    file.append(hdr, 10);
    juce::MemoryBlock art(kBody);
    std::memset(art.getData(), 0x55, kBody);
    file.append(art.getData(), kBody);
    file.append(static_cast<const char*>(bytes.getData()) + kOwnTag, bytes.getSize() - kOwnTag);
    const auto src = t.dir.getChildFile("art.mp3"), fromArt = t.dir.getChildFile("art.flac"), fromWav = t.dir.getChildFile("wav.flac");
    REQUIRE(src.replaceWithData(file.getData(), file.getSize()));
    REQUIRE(convertToShowAudio(src, fromArt).ok);
    REQUIRE(convertToShowAudio(wav, fromWav).ok);
    CHECK(std::abs(bestLag(readAll(fromArt), readAll(fromWav))) <= 2);
}

TEST_CASE("An MP3 without a LAME tag imports as before") {
    const auto mp3 = fixture("sine440_44k1_mono.mp3");
    if (!mp3.existsAsFile()) SKIP("MP3 fixture not generated yet: see io/tests/fixtures/README.md");
    TempDir t;
    juce::MemoryBlock bytes;
    REQUIRE(mp3.loadFileAsData(bytes));
    // Break the "Info" marker so the file looks like a tag-less encode.
    const auto* p = static_cast<const char*>(bytes.getData());
    const auto at = std::string_view(p, bytes.getSize()).find("Info");
    REQUIRE(at != std::string_view::npos);
    bytes.copyFrom("Xxxx", static_cast<int>(at), 4);
    const auto src = t.dir.getChildFile("plain.mp3"), dst = t.dir.getChildFile("plain.flac"), tagged = t.dir.getChildFile("tagged.flac");
    REQUIRE(src.replaceWithData(bytes.getData(), bytes.getSize()));
    REQUIRE(convertToShowAudio(src, dst).ok);
    REQUIRE(convertToShowAudio(mp3, tagged).ok);
    // Nothing trimmed: the tag-less import keeps at least the encoder delay and
    // padding (576 + 1080 frames at 44.1 kHz, about 1.7k at 48 kHz) the tagged one drops.
    CHECK(readAll(dst).getNumSamples() - readAll(tagged).getNumSamples() >= 1700);
}

TEST_CASE("A six-channel WAV keeps its first two channels") {
    TempDir t;
    const auto src = t.dir.getChildFile("in.wav"), dst = t.dir.getChildFile("out.flac");
    const auto in = writeWav(src, 48000.0, 6, 4800);
    REQUIRE(convertToShowAudio(src, dst).ok);
    const auto out = readAll(dst);
    REQUIRE(out.getNumChannels() == 2);
    REQUIRE(out.getNumSamples() == 4800);
    for (int ch = 0; ch < 2; ++ch)
        CHECK(std::abs(out.getMagnitude(ch, 0, 4800) - in.getMagnitude(ch, 0, 4800)) < 1e-6f);
}

TEST_CASE("Not really audio fails cleanly") {
    TempDir t;
    const auto dst = t.dir.getChildFile("out.flac");
    SECTION("random bytes named x.mp3") {
        const auto src = t.dir.getChildFile("x.mp3");
        juce::Random rng(12345);
        juce::MemoryBlock junk(64 * 1024);
        rng.fillBitsRandomly(junk.getData(), junk.getSize());
        REQUIRE(src.replaceWithData(junk.getData(), junk.getSize()));
        checkFailedCleanly(src, dst);
    }
    SECTION("a 0-byte x.wav") {
        const auto src = t.dir.getChildFile("x.wav");
        REQUIRE(src.create().wasOk());
        checkFailedCleanly(src, dst);
    }
    SECTION("a missing file") { checkFailedCleanly(t.dir.getChildFile("none.wav"), dst); }
}

// A playable MP3 is never refused: one that ends early imports what decodes.
TEST_CASE("An MP3 cut in half by bytes imports its first half") {
    const auto mp3 = fixture("sine440_44k1_mono.mp3");
    if (!mp3.existsAsFile()) SKIP("MP3 fixture not generated yet: see io/tests/fixtures/README.md");
    TempDir t;
    juce::MemoryBlock bytes;
    REQUIRE(mp3.loadFileAsData(bytes));
    const auto src = t.dir.getChildFile("half.mp3"), dst = t.dir.getChildFile("out.flac");
    REQUIRE(src.replaceWithData(bytes.getData(), bytes.getSize() / 2));
    const auto r = convertToShowAudio(src, dst);
    REQUIRE(r.ok);
    const double seconds = readAll(dst).getNumSamples() / 48000.0;
    CHECK(seconds > 0.6);  // about 1 s of the 2 s; generous for frame/tag overhead
    CHECK(seconds < 1.4);
}

TEST_CASE("An MP3 with trailing junk imports at full length") {
    const auto mp3 = fixture("sine440_44k1_mono.mp3");
    if (!mp3.existsAsFile()) SKIP("MP3 fixture not generated yet: see io/tests/fixtures/README.md");
    TempDir t;
    juce::MemoryBlock bytes;
    REQUIRE(mp3.loadFileAsData(bytes));
    juce::MemoryBlock junk(20 * 1024);  // like an APE/Lyrics tag JUCE does not know
    juce::Random(777).fillBitsRandomly(junk.getData(), junk.getSize());
    bytes.append(junk.getData(), junk.getSize());
    const auto src = t.dir.getChildFile("tagged.mp3"), dst = t.dir.getChildFile("out.flac");
    REQUIRE(src.replaceWithData(bytes.getData(), bytes.getSize()));
    REQUIRE(convertToShowAudio(src, dst).ok);
    CHECK(std::abs(readAll(dst).getNumSamples() / 48000.0 - 2.0) <= 0.05);
}
TEST_CASE("Same bytes give the same hash; one changed byte does not") {
    TempDir t;
    const auto a = t.dir.getChildFile("a.bin"), b = t.dir.getChildFile("b.bin");
    REQUIRE(a.replaceWithText("abc"));
    CHECK(hashFile(a) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");  // FIPS 180-2 test vector
    REQUIRE(b.replaceWithText("abc"));
    CHECK(hashFile(b) == hashFile(a));
    REQUIRE(b.replaceWithText("abd"));
    CHECK(hashFile(b) != hashFile(a));
    CHECK(hashFile(t.dir.getChildFile("missing.bin")).empty());
    CHECK(hashFile(t.dir).empty());  // a directory is not readable as bytes
}

TEST_CASE("chooseAudioTarget avoids files on disk and paths in the show") {
    TempDir t;
    const auto audio = t.dir.getChildFile("audio");
    REQUIRE(audio.createDirectory().wasOk());
    Show show;
    CHECK(chooseAudioTarget(audio, "song", show) == audio.getChildFile("song.flac"));
    REQUIRE(audio.getChildFile("song.flac").create().wasOk());
    CHECK(chooseAudioTarget(audio, "song", show) == audio.getChildFile("song (2).flac"));
    show.files.push_back({1, "audio/song (2).flac", 0.0, "h"});  // in the show, not on disk
    CHECK(chooseAudioTarget(audio, "song", show) == audio.getChildFile("song (3).flac"));
    show.files.push_back({2, "audio/Other.flac", 0.0, "h2"});
    CHECK(chooseAudioTarget(audio, "other", show) == audio.getChildFile("other (2).flac"));  // case-insensitive file systems
}

TEST_CASE("isImportable accepts WAV, AIFF, FLAC, MP3 and Ogg in any case; refuses .txt") {
    for (const char* ok : {"a.wav", "a.WAV", "a.mp3", "a.Mp3", "a.flac", "a.FLAC", "a.aiff", "a.AIF", "a.ogg"})
        CHECK(isImportable(juce::File::getCurrentWorkingDirectory().getChildFile(ok)));
    for (const char* no : {"a.m4a", "a.txt", "wav", "a.wav.txt"})
        CHECK_FALSE(isImportable(juce::File::getCurrentWorkingDirectory().getChildFile(no)));
}
TEST_CASE("Import reports rising progress, at most every 100 ms, and ends at 1") {
    TempDir t;
    const auto src = t.dir.getChildFile("long.wav"), dst = t.dir.getChildFile("long.flac");
    writeWav(src, 44100.0, 2, 44100 * 20);  // 20 s, resampled: long enough for several reports
    std::vector<float> seen;
    const auto start = juce::Time::getMillisecondCounter();
    const auto r = convertToShowAudio(src, dst, [&](float p) { seen.push_back(p); });
    const auto took = juce::Time::getMillisecondCounter() - start;
    REQUIRE(r.ok);
    REQUIRE_FALSE(seen.empty());
    CHECK(seen.back() == 1.0f);
    CHECK(std::is_sorted(seen.begin(), seen.end()));
    CHECK(seen.size() <= took / 100 + 2);
}
TEST_CASE("AIFF and Ogg Vorbis import to 48 kHz FLAC of the same length") {
    for (const char* ext : {"aiff", "ogg"}) {
        TempDir t;
        const auto src = t.dir.getChildFile(juce::String("in.") + ext), dst = t.dir.getChildFile("out.flac");
        {
            juce::AudioBuffer<float> b(2, 88200);
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < b.getNumSamples(); ++i) b.setSample(ch, i, sample(ch, i, 44100.0));
            std::unique_ptr<juce::AudioFormat> f;
            if (juce::String(ext) == "aiff") f = std::make_unique<juce::AiffAudioFormat>();
            else f = std::make_unique<juce::OggVorbisAudioFormat>();
            std::unique_ptr<juce::OutputStream> out = std::make_unique<juce::FileOutputStream>(src);
            auto w = f->createWriterFor(out, juce::AudioFormatWriterOptions{}.withSampleRate(44100.0).withNumChannels(2).withBitsPerSample(16));
            REQUIRE(w != nullptr);
            REQUIRE(w->writeFromAudioSampleBuffer(b, 0, b.getNumSamples()));
        }
        CHECK(isImportable(src));
        const auto r = convertToShowAudio(src, dst);
        INFO(ext << ": " << r.error);
        REQUIRE(r.ok);
        auto reader = open(dst);
        REQUIRE(reader != nullptr);
        CHECK(std::abs(reader->lengthInSamples - 96000) <= 2048);  // Ogg rounds to its packets
    }
}
