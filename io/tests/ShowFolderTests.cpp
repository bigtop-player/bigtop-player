// ShowFolderTests.cpp: safe saving, backups and state in a show folder.
// Every test works in its own temporary directory; no audio hardware needed.
#include <catch2/catch_test_macros.hpp>

#include <cmath>

#include "bigtop/ShowJson.h"
#include "bigtop/io/ShowFolder.h"

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

const juce::Time kT0(2026, 0, 1, 12, 0, 0, 0, true);
juce::Time at(int minutes) { return kT0 + juce::RelativeTime::minutes(minutes); }

std::string readShowText(const juce::File& dir) { return dir.getChildFile("show.json").loadFileAsString().toStdString(); }

/// A folder with a created-new show, plus a distinguishable good show saved into it.
struct Fixture {
    TempDir tmp;
    ShowFolder folder{tmp.dir};
    Show good;
    Fixture() {
        std::string err;
        REQUIRE(ShowFolder::createNew(tmp.dir, err));
        good.files.push_back({1, "audio/a.flac", -3.0, "abc"});
        REQUIRE(folder.save(good, kT0) == SaveResult::Saved);
    }
    int backupCount() const { return tmp.dir.getChildFile("backups").getNumberOfChildFiles(juce::File::findFiles, "*.json"); }
};

}  // namespace

TEST_CASE("createNew makes a loadable empty show") {
    TempDir t;
    const auto dir = t.dir.getChildFile("new");
    std::string err;
    REQUIRE(ShowFolder::createNew(dir, err));
    CHECK(dir.getChildFile("audio").isDirectory());
    CHECK(dir.getChildFile("backups").isDirectory());
    ShowFolder f(dir);
    const auto r = f.open(kT0);
    REQUIRE(r.show.has_value());
    CHECK(r.show->steps.empty());
    CHECK_FALSE(ShowFolder::createNew(dir, err));  // now non-empty
    CHECK_FALSE(err.empty());
}

TEST_CASE("save replaces show.json and leaves no tmp file") {
    Fixture fx;
    Show next = fx.good;
    next.files[0].trimDb = 2.5;
    CHECK(fx.folder.save(next, at(1)) == SaveResult::Saved);
    CHECK(fromJson(readShowText(fx.tmp.dir)) == next);
    CHECK_FALSE(fx.tmp.dir.getChildFile("show.json.tmp").exists());
}

TEST_CASE("A save that cannot be read back keeps the old file") {
    Fixture fx;
    Show bad = fx.good;
    bad.files.push_back({7, "audio/x.flac", std::nan(""), ""});
    CHECK(fx.folder.save(bad, at(1)) == SaveResult::ReadBackFailed);
    CHECK(fromJson(readShowText(fx.tmp.dir)) == fx.good);
    CHECK_FALSE(fx.tmp.dir.getChildFile("show.json.tmp").exists());
}

TEST_CASE("A save that cannot be written keeps the old file") {
    Fixture fx;
    // A directory where the tmp file belongs makes opening it for writing fail.
    REQUIRE(fx.tmp.dir.getChildFile("show.json.tmp").createDirectory().wasOk());
    Show next = fx.good;
    next.files[0].trimDb = 2.5;
    CHECK(fx.folder.save(next, at(1)) == SaveResult::WriteFailed);
    CHECK(fromJson(readShowText(fx.tmp.dir)) == fx.good);
}

TEST_CASE("open removes leftover tmp files") {
    Fixture fx;
    fx.tmp.dir.getChildFile("show.json.tmp").replaceWithText("half");
    fx.tmp.dir.getChildFile("state.json.tmp").replaceWithText("half");
    REQUIRE(fx.folder.open(at(1)).show.has_value());
    CHECK_FALSE(fx.tmp.dir.getChildFile("show.json.tmp").exists());
    CHECK_FALSE(fx.tmp.dir.getChildFile("state.json.tmp").exists());
}

TEST_CASE("Unreadable show.json lists loadable backups newest first") {
    Fixture fx;
    const auto b = fx.tmp.dir.getChildFile("backups");
    for (const auto& f : b.findChildFiles(juce::File::findFiles, false)) f.deleteFile();
    const auto text = toJson(fx.good);
    b.getChildFile("show-20260101-100000.json").replaceWithText(text);
    b.getChildFile("show-20260101-110000.json").replaceWithText("{ broken");
    b.getChildFile("show-20260101-120000.json").replaceWithText(text);
    fx.tmp.dir.getChildFile("show.json").replaceWithText("{ not a show");

    const auto r = fx.folder.open(kT0);
    CHECK_FALSE(r.show.has_value());
    CHECK_FALSE(r.error.empty());
    REQUIRE(r.loadableBackups.size() == 2);
    CHECK(r.loadableBackups[0].getFileName() == "show-20260101-120000.json");
    CHECK(r.loadableBackups[1].getFileName() == "show-20260101-100000.json");
    CHECK(readShowText(fx.tmp.dir) == "{ not a show");  // open never touches it
}

TEST_CASE("restoreBackup makes the backup the show") {
    Fixture fx;
    const auto backup = fx.tmp.dir.getChildFile("backups").getChildFile("old.json");
    Show old;
    old.files.push_back({9, "audio/o.flac", 0.0, ""});
    backup.replaceWithText(toJson(old));
    fx.tmp.dir.getChildFile("show.json").replaceWithText("garbage");
    REQUIRE(fx.folder.restoreBackup(backup));
    CHECK(fromJson(readShowText(fx.tmp.dir)) == old);
    CHECK_FALSE(fx.tmp.dir.getChildFile("show.json.tmp").exists());
}

TEST_CASE("Backups: on open, at most every 5 minutes, newest 50 kept") {
    TempDir t;
    ShowFolder f(t.dir);
    std::string err;
    REQUIRE(ShowFolder::createNew(t.dir, err));
    const auto count = [&] { return t.dir.getChildFile("backups").getNumberOfChildFiles(juce::File::findFiles, "*.json"); };

    REQUIRE(f.open(kT0).show.has_value());
    CHECK(count() == 1);
    Show s;
    CHECK(f.save(s, at(1)) == SaveResult::Saved);
    CHECK(f.save(s, at(4)) == SaveResult::Saved);
    CHECK(count() == 1);
    CHECK(f.save(s, at(6)) == SaveResult::Saved);
    CHECK(count() == 2);

    for (int i = 0; i < 60; ++i) CHECK(f.save(s, at(20 + 6 * i)) == SaveResult::Saved);
    CHECK(count() == 50);
}

TEST_CASE("state.json round trip; missing or broken gives defaults") {
    Fixture fx;
    CHECK(fx.folder.loadState().nextIndex == 0);
    CHECK(fx.folder.loadState().mode == ShowMode::Edit);
    CHECK(fx.folder.saveState({4, ShowMode::Show}) == SaveResult::Saved);
    CHECK(fx.folder.loadState().nextIndex == 4);
    CHECK(fx.folder.loadState().mode == ShowMode::Show);
    CHECK_FALSE(fx.tmp.dir.getChildFile("state.json.tmp").exists());
    fx.tmp.dir.getChildFile("state.json").replaceWithText("{ broken");
    CHECK(fx.folder.loadState().nextIndex == 0);
}

TEST_CASE("state.json mode round trip") {
    Fixture fx;
    for (const auto m : {ShowMode::Edit, ShowMode::Rehearsal, ShowMode::Show}) {
        CHECK(fx.folder.saveState({1, m}) == SaveResult::Saved);
        CHECK(fx.folder.loadState().mode == m);
    }
}

TEST_CASE("old showMode true reads as Show, false as Edit") {
    Fixture fx;
    const auto file = fx.tmp.dir.getChildFile("state.json");
    file.replaceWithText(R"({"nextIndex": 2, "showMode": true})");
    CHECK(fx.folder.loadState().mode == ShowMode::Show);
    CHECK(fx.folder.loadState().nextIndex == 2);
    file.replaceWithText(R"({"nextIndex": 2, "showMode": false})");
    CHECK(fx.folder.loadState().mode == ShowMode::Edit);
}

TEST_CASE("unknown mode reads as Edit, and mode wins over showMode") {
    Fixture fx;
    const auto file = fx.tmp.dir.getChildFile("state.json");
    file.replaceWithText(R"({"mode": "party", "showMode": true})");
    CHECK(fx.folder.loadState().mode == ShowMode::Edit);
    file.replaceWithText(R"({"mode": "rehearsal", "showMode": true})");
    CHECK(fx.folder.loadState().mode == ShowMode::Rehearsal);
}

TEST_CASE("Opening a folder that does not exist is an error, not a crash") {
    TempDir t;
    ShowFolder f(t.dir.getChildFile("nope"));
    const auto r = f.open(kT0);
    CHECK_FALSE(r.show.has_value());
    CHECK_FALSE(r.error.empty());
    CHECK(r.loadableBackups.empty());
}

TEST_CASE("A missing backups folder is recreated and backed up") {
    Fixture fx;
    fx.tmp.dir.getChildFile("backups").deleteRecursively();
    CHECK(fx.folder.save(fx.good, at(10)) == SaveResult::Saved);
    CHECK(fx.backupCount() == 1);
}

TEST_CASE("A newest backup stamped in the future does not stop backups") {
    Fixture fx;
    const auto b = fx.tmp.dir.getChildFile("backups");
    b.getChildFile("show-20300101-000000.json").replaceWithText(toJson(fx.good));
    const int before = fx.backupCount();
    CHECK(fx.folder.save(fx.good, at(10)) == SaveResult::Saved);
    CHECK(fx.backupCount() == before + 1);
    CHECK(fx.folder.save(fx.good, at(11)) == SaveResult::Saved);  // fresh again: no flood
    CHECK(fx.backupCount() == before + 1);
}

TEST_CASE("Backup order compares the stamp first; foreign names are ignored and kept") {
    Fixture fx;
    const auto b = fx.tmp.dir.getChildFile("backups");
    for (const auto& f : b.findChildFiles(juce::File::findFiles, false)) f.deleteFile();
    const auto text = toJson(fx.good);
    b.getChildFile("show-20260101-120000.json").replaceWithText(text);
    b.getChildFile("show-20260101-120000-2.json").replaceWithText(text);
    b.getChildFile("show-20260102-000000.json").replaceWithText(text);
    b.getChildFile("show-before-premiere.json").replaceWithText(text);
    fx.tmp.dir.getChildFile("show.json").replaceWithText("{ broken");

    const auto r = fx.folder.open(kT0);
    REQUIRE(r.loadableBackups.size() == 3);
    CHECK(r.loadableBackups[0].getFileName() == "show-20260102-000000.json");
    CHECK(r.loadableBackups[1].getFileName() == "show-20260101-120000-2.json");
    CHECK(r.loadableBackups[2].getFileName() == "show-20260101-120000.json");
    CHECK(b.getChildFile("show-before-premiere.json").existsAsFile());
}

TEST_CASE("A foreign backups file does not make every save take a backup") {
    Fixture fx;  // one real backup at kT0
    fx.tmp.dir.getChildFile("backups").getChildFile("show-before-premiere.json").replaceWithText("x");
    CHECK(fx.folder.save(fx.good, at(1)) == SaveResult::Saved);
    CHECK(fx.backupCount() == 2);  // the foreign file + the one real backup
}

TEST_CASE("A failed write leaves no tmp file behind") {
    Fixture fx;
    // A directory as show.json makes the final swap fail after the tmp was written.
    fx.tmp.dir.getChildFile("show.json").deleteFile();
    REQUIRE(fx.tmp.dir.getChildFile("show.json").createDirectory().wasOk());
    fx.tmp.dir.getChildFile("show.json").getChildFile("x").replaceWithText("x");
    CHECK(fx.folder.save(fx.good, at(1)) == SaveResult::WriteFailed);
    CHECK_FALSE(fx.tmp.dir.getChildFile("show.json.tmp").exists());
}

TEST_CASE("restoreBackup refuses a missing backup") {
    Fixture fx;
    CHECK_FALSE(fx.folder.restoreBackup(fx.tmp.dir.getChildFile("backups").getChildFile("nope.json")));
    CHECK(fromJson(readShowText(fx.tmp.dir)) == fx.good);
}

TEST_CASE("A show that cannot be serialised is ReadBackFailed, not a crash") {
    Fixture fx;
    Show bad = fx.good;
    bad.files.push_back({8, "audio/\xff\xfe.flac", 0.0, ""});  // invalid UTF-8 makes toJson throw
    CHECK(fx.folder.save(bad, at(1)) == SaveResult::ReadBackFailed);
    CHECK(fromJson(readShowText(fx.tmp.dir)) == fx.good);
}
