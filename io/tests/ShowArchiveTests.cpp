// ShowArchiveTests.cpp: export/import of a show as a .bigtop.zip, the kept originals,
// and the list of FLACs to rebuild. Offline: audio is generated (WAV) or the committed
// MP3 fixture; no device, no message loop.
#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <set>

#include <juce_audio_formats/juce_audio_formats.h>

#include "bigtop/ShowJson.h"
#include "bigtop/io/Importer.h"
#include "bigtop/io/ShowArchive.h"

using namespace bigtop;
using namespace bigtop::io;

namespace {

/// A unique temp directory, deleted with the object.
struct TempDir {
    juce::File dir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                         .getChildFile("bigtop_arch_" + juce::String(juce::Random::getSystemRandom().nextInt64()));
    TempDir() { dir.createDirectory(); }
    ~TempDir() { dir.deleteRecursively(); }
};

juce::File fixture(const char* name) { return juce::File(BIGTOP_IO_FIXTURES).getChildFile(name); }

/// Writes a short 24-bit stereo WAV (a ramp, so different seeds give different bytes).
void writeWav(const juce::File& f, int seed) {
    juce::AudioBuffer<float> buf(2, 4800);
    for (int ch = 0; ch < 2; ++ch)
        for (int i = 0; i < buf.getNumSamples(); ++i) buf.setSample(ch, i, 0.001f * static_cast<float>((i + seed) % 200));
    std::unique_ptr<juce::OutputStream> out = std::make_unique<juce::FileOutputStream>(f);
    auto w = juce::WavAudioFormat().createWriterFor(
        out, juce::AudioFormatWriterOptions{}.withSampleRate(48000.0).withNumChannels(2).withBitsPerSample(24));
    REQUIRE(w != nullptr);
    REQUIRE(w->writeFromAudioSampleBuffer(buf, 0, buf.getNumSamples()));
}

juce::int64 flacLength(const juce::File& f) {
    juce::AudioFormatManager m;
    m.registerFormat(new juce::FlacAudioFormat, true);
    const std::unique_ptr<juce::AudioFormatReader> r(m.createReaderFor(f));
    return r != nullptr ? r->lengthInSamples : -1;
}

/// A show folder as the app leaves it after three drops, plus one unused file:
///  1 WAV original + FLAC (step), 2 MP3 original + FLAC (step), 3 FLAC only (pad, no original),
///  4 WAV original + FLAC but used by nothing.
struct Fixture {
    TempDir t;
    juce::File dir = t.dir.getChildFile("My Show");
    Show show;
    Fixture() {
        dir.getChildFile("audio").createDirectory();
        dir.getChildFile("backups").createDirectory();
        const auto src = t.dir.getChildFile("src");
        src.createDirectory();
        const auto add = [&](FileId id, const juce::File& original, const char* flac, bool keep) {
            AudioFile f;
            f.id = id;
            f.path = std::string("audio/") + flac;
            f.sourceHash = hashFile(original);
            f.importVersion = kImportVersion;
            if (keep) {
                f.sourcePath = sourcePathFor(f.sourceHash, original);
                REQUIRE(keepOriginal(original, dir, f.sourcePath));
            }
            REQUIRE(convertToShowAudio(original, dir.getChildFile(juce::String(f.path))).ok);
            show.files.push_back(f);
        };
        writeWav(src.getChildFile("a.WAV"), 1);
        writeWav(src.getChildFile("c.wav"), 2);
        writeWav(src.getChildFile("d.wav"), 3);
        add(1, src.getChildFile("a.WAV"), "a.flac", true);
        add(2, fixture("sine440_44k1_stereo_lame.mp3"), "b.flac", true);
        add(3, src.getChildFile("c.wav"), "c.flac", false);
        add(4, src.getChildFile("d.wav"), "d.flac", true);
        Step s1, s2;
        s1.id = 1; s1.name = "One"; s1.sound = Sound{.file = 1};
        s2.id = 2; s2.name = "Two"; s2.sound = Sound{.file = 2};
        show.steps = {s1, s2};
        show.lastStepId = 2;
        show.pads[0].name = "Pad"; show.pads[0].file = 3;
        dir.getChildFile("show.json").replaceWithText(toJson(show));
        dir.getChildFile("state.json").replaceWithText(R"({"nextIndex":1,"mode":"show"})");
        // Things an export must leave out.
        dir.getChildFile("backups").getChildFile("show-20260101-000000.json").replaceWithText("{}");
    }
};

std::set<std::string> entryNames(const juce::File& zip) {
    juce::ZipFile z(zip);
    std::set<std::string> n;
    for (int i = 0; i < z.getNumEntries(); ++i) n.insert(z.getEntry(i)->filename.toStdString());
    return n;
}

/// Builds a zip with the given (name, text) entries, names stored as written (hostile ones too).
void makeZip(const juce::File& target, const std::vector<std::pair<juce::String, juce::String>>& entries) {
    juce::ZipFile::Builder b;
    for (const auto& [name, text] : entries) {
        const auto utf8 = text.toStdString();
        b.addEntry(std::make_unique<juce::MemoryInputStream>(utf8.data(), utf8.size(), true), 0, name, juce::Time::getCurrentTime());
    }
    juce::FileOutputStream out(target);
    REQUIRE(out.openedOk());
    REQUIRE(b.writeToStream(out, nullptr));
}

bool sameBytes(const juce::File& a, const juce::File& b) { return a.existsAsFile() && b.existsAsFile() && a.hasIdenticalContentTo(b); }

constexpr const char* kGoodShow = R"({"formatVersion":1})";

/// A refusal must say why and leave the folder empty (it may exist or not).
void checkRefused(const ArchiveResult& r, const juce::File& dir) {
    CHECK_FALSE(r.ok);
    CHECK_FALSE(r.error.empty());
    CHECK(dir.getNumberOfChildFiles(juce::File::findFilesAndDirectories) == 0);
}

}  // namespace

TEST_CASE("keepOriginal copies unchanged under the hash name with a lower case extension, once") {
    TempDir t;
    const auto src = t.dir.getChildFile("Song.MP3");
    writeWav(src, 5);  // content is irrelevant to the copy
    const auto rel = sourcePathFor("ab12", src);
    CHECK(rel == "sources/ab12.mp3");
    const auto show = t.dir.getChildFile("show");
    REQUIRE(keepOriginal(src, show, rel));
    CHECK(sameBytes(src, show.getChildFile("sources/ab12.mp3")));
    CHECK_FALSE(show.getChildFile("sources/ab12.mp3.tmp").exists());
    REQUIRE(keepOriginal(src, show, rel));  // second time: already there
    CHECK(show.getChildFile("sources").getNumberOfChildFiles(juce::File::findFiles) == 1);
    CHECK_FALSE(keepOriginal(src, show, "../escape.mp3"));
    CHECK_FALSE(keepOriginal(src, show, "sources/missing-source.mp3.x/../../../x"));
    CHECK_FALSE(keepOriginal(t.dir.getChildFile("nope.wav"), show, "sources/zz.wav"));
    CHECK_FALSE(show.getChildFile("sources/zz.wav.tmp").exists());
}

TEST_CASE("archiveName is <name>-YYYYMMDD-HHMM.bigtop.zip") {
    const juce::Time when(2026, 10, 22, 21, 5, 59, 0, true);  // month is 0-based: 22 November
    CHECK(archiveName("Spring Show", when) == "Spring Show-20261122-2105.bigtop.zip");
    CHECK(archiveName("a/b:c", when).startsWith("a_b_c-"));
}

TEST_CASE("Export then import: same show.json, originals byte-identical, FLACs rebuilt and playable") {
    Fixture f;
    const auto zip = f.t.dir.getChildFile("out.bigtop.zip");
    const auto r = exportShow(f.dir, f.show, zip);
    REQUIRE(r.ok);
    CHECK_FALSE(zip.getSiblingFile("out.bigtop.zip.tmp").exists());

    // Contents: show.json, state.json, the two originals, the FLAC that has none. Nothing else.
    const auto names = entryNames(zip);
    CHECK(names == std::set<std::string>{"show.json", "state.json", f.show.files[0].sourcePath, f.show.files[1].sourcePath,
                                         "audio/c.flac"});

    // Stored, not deflated: the file is at least as big as its contents (the WAV ramp would deflate well).
    juce::ZipFile z(zip);
    juce::int64 sum = 0;
    for (int i = 0; i < z.getNumEntries(); ++i) sum += z.getEntry(i)->uncompressedSize;
    CHECK(zip.getSize() >= sum);

    const auto target = f.t.dir.getChildFile("imported");
    REQUIRE(importShow(zip, target).ok);
    const auto loaded = fromJson(target.getChildFile("show.json").loadFileAsString().toStdString());
    CHECK(loaded == f.show);
    // state.json keeps where the owner left off but an imported show always opens in Edit mode.
    const auto state = juce::JSON::parse(target.getChildFile("state.json"));
    CHECK(static_cast<int>(state["nextIndex"]) == 1);
    CHECK(state["mode"].toString() == "edit");
    CHECK(target.getChildFile("audio").isDirectory());
    CHECK(target.getChildFile("backups").isDirectory());
    for (int i : {0, 1}) {
        const auto p = juce::String(f.show.files[static_cast<size_t>(i)].sourcePath);
        CHECK(sameBytes(f.dir.getChildFile(p), target.getChildFile(p)));
    }
    CHECK(sameBytes(f.dir.getChildFile("audio/c.flac"), target.getChildFile("audio/c.flac")));
    CHECK_FALSE(target.getChildFile("backups/show-20260101-000000.json").exists());
    CHECK_FALSE(target.getChildFile("audio/d.flac").exists());  // unused file stays behind
    CHECK(target.getChildFile("sources").getNumberOfChildFiles(juce::File::findFiles) == 2);
}

TEST_CASE("A file without an original travels as its FLAC and imports as it was") {
    Fixture f;
    const auto zip = f.t.dir.getChildFile("out.bigtop.zip");
    REQUIRE(exportShow(f.dir, f.show, zip).ok);
    const auto target = f.t.dir.getChildFile("imported");
    REQUIRE(importShow(zip, target).ok);
    CHECK(sameBytes(f.dir.getChildFile("audio/c.flac"), target.getChildFile("audio/c.flac")));
    CHECK(flacLength(target.getChildFile("audio/c.flac")) > 0);
}

TEST_CASE("Re-conversion after import: filesToRebuild lists them and the importer makes playable FLACs") {
    Fixture f;
    const auto zip = f.t.dir.getChildFile("out.bigtop.zip");
    REQUIRE(exportShow(f.dir, f.show, zip).ok);
    const auto target = f.t.dir.getChildFile("imported");
    REQUIRE(importShow(zip, target).ok);
    const auto loaded = fromJson(target.getChildFile("show.json").loadFileAsString().toStdString());

    CHECK(filesToRebuild(target, loaded) == std::vector<FileId>{1, 2});
    for (const FileId id : filesToRebuild(target, loaded)) {
        const auto& af = loaded.files[id - 1];
        REQUIRE(convertToShowAudio(target.getChildFile(juce::String(af.sourcePath)), target.getChildFile(juce::String(af.path))).ok);
    }
    CHECK(filesToRebuild(target, loaded).empty());
    for (const char* n : {"audio/a.flac", "audio/b.flac"}) CHECK(flacLength(target.getChildFile(n)) > 0);
    // Same conversion as the original run: same length (the converter is deterministic).
    CHECK(flacLength(target.getChildFile("audio/a.flac")) == flacLength(f.dir.getChildFile("audio/a.flac")));
    CHECK(flacLength(target.getChildFile("audio/b.flac")) == flacLength(f.dir.getChildFile("audio/b.flac")));
}

TEST_CASE("filesToRebuild: only a missing FLAC with a present original") {
    Fixture f;
    CHECK(filesToRebuild(f.dir, f.show).empty());  // everything present
    f.dir.getChildFile("audio/a.flac").deleteFile();  // 1: FLAC gone, original there
    f.dir.getChildFile("audio/c.flac").deleteFile();  // 3: FLAC gone, never had an original
    f.dir.getChildFile("audio/b.flac").deleteFile();
    f.dir.getChildFile(juce::String(f.show.files[1].sourcePath)).deleteFile();  // 2: both gone
    CHECK(filesToRebuild(f.dir, f.show) == std::vector<FileId>{1});
    f.show.files[0].sourcePath = "../outside.wav";  // an escaping path is ignored, never followed
    CHECK(filesToRebuild(f.dir, f.show).empty());
}

TEST_CASE("Import refuses a path that escapes the folder and leaves the target empty") {
    TempDir t;
    const auto target = t.dir.getChildFile("target");
    for (const char* name : {"../evil.txt", "sub/../../evil.txt", "/abs-evil.txt", "C:/evil.txt", "..\\evil.txt"}) {
        const auto zip = t.dir.getChildFile("bad.zip");
        makeZip(zip, {{"show.json", kGoodShow}, {name, "x"}});
        checkRefused(importShow(zip, target), target);
        CHECK_FALSE(t.dir.getChildFile("evil.txt").exists());
    }
}

TEST_CASE("Import refuses a zip without show.json") {
    TempDir t;
    const auto zip = t.dir.getChildFile("bad.zip"), target = t.dir.getChildFile("target");
    makeZip(zip, {{"audio/a.flac", "x"}});
    checkRefused(importShow(zip, target), target);
}

TEST_CASE("Import refuses a damaged zip") {
    TempDir t;
    const auto target = t.dir.getChildFile("target");
    const auto junk = t.dir.getChildFile("junk.zip");
    junk.replaceWithText("this is not a zip file at all");
    checkRefused(importShow(junk, target), target);
    checkRefused(importShow(t.dir.getChildFile("missing.zip"), target), target);

    Fixture f;  // a real export with its end cut off
    const auto zip = f.t.dir.getChildFile("out.bigtop.zip");
    REQUIRE(exportShow(f.dir, f.show, zip).ok);
    juce::MemoryBlock all;
    REQUIRE(zip.loadFileAsData(all));
    const auto cut = f.t.dir.getChildFile("cut.zip");
    cut.replaceWithData(all.getData(), all.getSize() / 2);
    const auto target2 = f.t.dir.getChildFile("target2");
    checkRefused(importShow(cut, target2), target2);
}

TEST_CASE("Import refuses a show.json the loader refuses, or one naming paths outside the folder") {
    TempDir t;
    const auto zip = t.dir.getChildFile("bad.zip"), target = t.dir.getChildFile("target");
    makeZip(zip, {{"show.json", "{ not json"}});
    checkRefused(importShow(zip, target), target);
    makeZip(zip, {{"show.json", R"({"formatVersion":2})"}});
    checkRefused(importShow(zip, target), target);
    makeZip(zip, {{"show.json", R"({"formatVersion":1,"files":[{"id":1,"path":"../../x.flac"}]})"}});
    checkRefused(importShow(zip, target), target);
    makeZip(zip, {{"show.json", R"({"formatVersion":1,"files":[{"id":1,"path":"audio/a.flac","sourcePath":"/etc/x"}]})"}});
    checkRefused(importShow(zip, target), target);
}

TEST_CASE("Import into a folder with content is refused and touches nothing") {
    TempDir t;
    const auto zip = t.dir.getChildFile("ok.zip"), target = t.dir.getChildFile("target");
    makeZip(zip, {{"show.json", kGoodShow}});
    target.getChildFile("mine.txt").create();
    const auto r = importShow(zip, target);
    CHECK_FALSE(r.ok);
    CHECK(target.getChildFile("mine.txt").existsAsFile());
    CHECK(target.getNumberOfChildFiles(juce::File::findFilesAndDirectories) == 1);
}

TEST_CASE("A minimal valid zip imports into a folder that does not exist yet") {
    TempDir t;
    const auto zip = t.dir.getChildFile("ok.zip"), target = t.dir.getChildFile("new/folder");
    makeZip(zip, {{"show.json", kGoodShow}});
    CHECK(importShow(zip, target).ok);
    CHECK(target.getChildFile("show.json").existsAsFile());
}

TEST_CASE("A failed export leaves no zip and no .tmp, and keeps an older zip") {
    Fixture f;
    const auto zip = f.t.dir.getChildFile("out.bigtop.zip");
    zip.replaceWithText("older");
    f.show.files[0].sourcePath = "../outside.wav";
    f.show.files[0].path = "../outside.flac";  // used by step 1: the path escapes
    const auto r = exportShow(f.dir, f.show, zip);
    CHECK_FALSE(r.ok);
    CHECK_FALSE(r.error.empty());
    CHECK(zip.loadFileAsString() == "older");
    CHECK_FALSE(f.t.dir.getChildFile("out.bigtop.zip.tmp").exists());
}

TEST_CASE("A used file with neither original nor FLAC is left out, the export still works") {
    Fixture f;
    f.dir.getChildFile("audio/c.flac").deleteFile();
    const auto zip = f.t.dir.getChildFile("out.bigtop.zip");
    const auto r = exportShow(f.dir, f.show, zip);
    REQUIRE(r.ok);
    CHECK(entryNames(zip).count("audio/c.flac") == 0);
    CHECK(r.omitted == std::vector<std::string>{"audio/c.flac"});  // the caller warns about it
    // A complete export omits nothing.
    Fixture g;
    CHECK(exportShow(g.dir, g.show, g.t.dir.getChildFile("ok.zip")).omitted.empty());
}

// Two entries with one name (in any case: Windows folders ignore case) would
// be written into one file, the second appended to the first.
TEST_CASE("Import refuses a zip with two entries of the same name, in any case") {
    TempDir t;
    const auto zip = t.dir.getChildFile("dup.zip"), target = t.dir.getChildFile("target");
    makeZip(zip, {{"show.json", kGoodShow}, {"sources/a.wav", "first"}, {"sources/a.wav", "second"}});
    checkRefused(importShow(zip, target), target);
    makeZip(zip, {{"show.json", kGoodShow}, {"SHOW.json", kGoodShow}});
    checkRefused(importShow(zip, target), target);
    // Another spelling of the same file is refused too (JUCE's builder stores such a name empty,
    // so here it is the unsafe-path check that refuses; the duplicate check keys on the resolved file).
    makeZip(zip, {{"show.json", kGoodShow}, {"sources/a.wav", "first"}, {"sources/x/../a.wav", "second"}});
    checkRefused(importShow(zip, target), target);
}

// A zip made by hand (or by an older version) may say Show mode, which would
// open the show locked on the receiving laptop.
TEST_CASE("Import opens in Edit mode even when the zip's state.json says show") {
    TempDir t;
    const auto zip = t.dir.getChildFile("ok.zip"), target = t.dir.getChildFile("target");
    makeZip(zip, {{"show.json", kGoodShow}, {"state.json", R"({"nextIndex":3,"mode":"show"})"}});
    REQUIRE(importShow(zip, target).ok);
    const auto state = juce::JSON::parse(target.getChildFile("state.json"));
    CHECK(state["mode"].toString() == "edit");
    CHECK(static_cast<int>(state["nextIndex"]) == 3);
}
