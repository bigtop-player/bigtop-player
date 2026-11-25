// ShowFolder.cpp: verified atomic saves, timed backups, state.json.
#include "bigtop/io/ShowFolder.h"

#include <algorithm>
#include <regex>

#include "bigtop/ShowJson.h"

namespace bigtop::io {

namespace {

constexpr int kMaxBackups = 50;
constexpr juce::int64 kBackupIntervalMs = 5 * 60 * 1000;
const char* const kStampFormat = "%Y%m%d-%H%M%S";

juce::File tmpOf(const juce::File& f) { return f.getSiblingFile(f.getFileName() + ".tmp"); }

/// Writes text to f via f.tmp so a crash or full disk can never leave a
/// half-written f: the old file stays until the new one is complete.
SaveResult writeAtomically(const juce::File& f, const std::string& text) {
    const auto tmp = tmpOf(f);
    const auto fail = [&] {
        tmp.deleteFile();  // never leave a partial tmp behind
        return SaveResult::WriteFailed;
    };
    {
        juce::FileOutputStream out(tmp);
        if (out.failedToOpen()) return SaveResult::WriteFailed;  // nothing of ours to delete
        out.setPosition(0);
        if (out.truncate().failed()) return fail();  // a leftover tmp must not keep stale bytes
        if (!out.write(text.data(), text.size())) return fail();
        out.flush();  // push to the OS before the swap
        if (out.getStatus().failed()) return fail();
    }
    // replaceFileIn swaps in one step (ReplaceFile / rename), unlike
    // moveFileTo, which deletes the target first and so could lose both files.
    if (!tmp.replaceFileIn(f)) return fail();
    return SaveResult::Saved;
}

/// A backup file whose name matches show-YYYYMMDD-HHMMSS[-N].json.
struct Backup {
    juce::File file;
    std::string stamp;  ///< The 15-character YYYYMMDD-HHMMSS part.
    int copy = 1;       ///< 1 for the plain name, N for a same-second "-N" copy.
};

/// Backups sorted oldest first (stamp, then copy number). Files in backups/
/// that do not match the pattern are the owner's: ignored, never deleted.
std::vector<Backup> listBackups(const juce::File& dir) {
    static const std::regex pattern(R"(show-(\d{8}-\d{6})(?:-(\d+))?)");
    std::vector<Backup> v;
    for (const auto& f : dir.getChildFile("backups").findChildFiles(juce::File::findFiles, false, "show-*.json")) {
        const auto stem = f.getFileNameWithoutExtension().toStdString();
        std::smatch m;
        if (!std::regex_match(stem, m, pattern)) continue;
        v.push_back({f, m[1].str(), m[2].matched ? std::atoi(m[2].str().c_str()) : 1});
    }
    std::sort(v.begin(), v.end(), [](const Backup& a, const Backup& b) {
        return a.stamp != b.stamp ? a.stamp < b.stamp : a.copy < b.copy;
    });
    return v;
}

/// Time encoded in a backup's stamp (local time, as written).
juce::Time backupTime(const Backup& b) {
    const auto s = juce::String(b.stamp);
    const auto num = [&](int from, int len) { return s.substring(from, from + len).getIntValue(); };
    return juce::Time(num(0, 4), num(4, 2) - 1, num(6, 2), num(9, 2), num(11, 2), num(13, 2), 0, true);
}

bool loads(const juce::File& f) {
    try {
        fromJson(f.loadFileAsString().toStdString());
        return true;
    } catch (const ShowFormatError&) {
        return false;
    }
}

}  // namespace

ShowFolder::ShowFolder(juce::File dir) : dir_(std::move(dir)) {}

const juce::File& ShowFolder::dir() const noexcept { return dir_; }

juce::File ShowFolder::audioDir() const { return dir_.getChildFile("audio"); }

bool ShowFolder::createNew(const juce::File& dir, std::string& error) {
    if (dir.exists() && (!dir.isDirectory() || dir.getNumberOfChildFiles(juce::File::findFilesAndDirectories) > 0)) {
        error = "The folder is not empty.";
        return false;
    }
    if (!dir.createDirectory().wasOk() || !dir.getChildFile("audio").createDirectory().wasOk() ||
        !dir.getChildFile("backups").createDirectory().wasOk() ||
        writeAtomically(dir.getChildFile("show.json"), toJson(Show{})) != SaveResult::Saved) {
        error = "Could not create the show folder.";
        return false;
    }
    return true;
}

OpenResult ShowFolder::open(juce::Time now) {
    OpenResult r;
    if (!dir_.isDirectory()) {
        r.error = "The show folder does not exist.";
        return r;
    }
    // Leftovers of a save that was interrupted; the real files are intact.
    tmpOf(dir_.getChildFile("show.json")).deleteFile();
    tmpOf(dir_.getChildFile("state.json")).deleteFile();

    const auto file = dir_.getChildFile("show.json");
    if (!file.existsAsFile()) {
        r.error = "show.json is missing.";
    } else {
        try {
            r.show = fromJson(file.loadFileAsString().toStdString());
            backupIfDue(now);
            return r;
        } catch (const ShowFormatError& e) {
            r.error = std::string("show.json cannot be read: ") + e.what();
        }
    }
    const auto all = listBackups(dir_);
    for (auto it = all.rbegin(); it != all.rend(); ++it)
        if (loads(it->file)) r.loadableBackups.push_back(it->file);
    return r;
}

SaveResult ShowFolder::save(const Show& show, juce::Time now) {
    std::string text;
    // Prove the text reads back before it may replace the good file. toJson
    // itself can throw (e.g. invalid UTF-8 in a path), which is the same
    // verdict: this show cannot be stored.
    try {
        text = toJson(show);
        fromJson(text);
    } catch (const std::exception&) {
        return SaveResult::ReadBackFailed;
    }
    const auto r = writeAtomically(dir_.getChildFile("show.json"), text);
    if (r == SaveResult::Saved) backupIfDue(now);
    return r;
}

SaveResult ShowFolder::saveState(const ShowState& state) {
    auto* o = new juce::DynamicObject();
    o->setProperty("nextIndex", state.nextIndex);
    // The old key is not written: old builds then read "Edit", the safe default.
    const char* name = state.mode == ShowMode::Show ? "show" : state.mode == ShowMode::Rehearsal ? "rehearsal" : "edit";
    o->setProperty("mode", name);
    return writeAtomically(dir_.getChildFile("state.json"), juce::JSON::toString(juce::var(o)).toStdString());
}

ShowState ShowFolder::loadState() const {
    ShowState s;
    const auto v = juce::JSON::parse(dir_.getChildFile("state.json"));
    if (const auto* o = v.getDynamicObject()) {
        if (o->getProperty("nextIndex").isInt()) s.nextIndex = o->getProperty("nextIndex");
        if (o->getProperty("mode").isString()) {
            // An unknown name stays Edit (the default), like any unreadable state.
            const auto name = o->getProperty("mode").toString();
            if (name == "show") s.mode = ShowMode::Show;
            else if (name == "rehearsal") s.mode = ShowMode::Rehearsal;
        } else if (o->getProperty("showMode").isBool() && static_cast<bool>(o->getProperty("showMode"))) {
            s.mode = ShowMode::Show;  // files written before modes existed
        }
    }
    return s;
}

bool ShowFolder::restoreBackup(const juce::File& backup) {
    if (!backup.existsAsFile()) return false;
    // Same tmp-flush-swap path as a normal save.
    return writeAtomically(dir_.getChildFile("show.json"), backup.loadFileAsString().toStdString()) ==
           SaveResult::Saved;
}

void ShowFolder::backupIfDue(juce::Time now) {
    auto backups = listBackups(dir_);
    // Judge by the newest backup that is not stamped in the future (clock
    // change, DST fall-back, folder from another machine); a future one would
    // otherwise either block backups or, if treated as due, trigger one per save.
    for (auto it = backups.rbegin(); it != backups.rend(); ++it) {
        const auto diff = now.toMilliseconds() - backupTime(*it).toMilliseconds();
        if (diff < 0) continue;
        if (diff < kBackupIntervalMs) return;
        break;
    }
    const auto folder = dir_.getChildFile("backups");
    folder.createDirectory();  // the owner may have removed it
    const auto stem = "show-" + now.formatted(kStampFormat);
    auto target = folder.getChildFile(stem + ".json");
    for (int n = 2; target.exists(); ++n) target = folder.getChildFile(stem + "-" + juce::String(n) + ".json");
    // Best effort: a failed backup must not fail the save it follows.
    if (!dir_.getChildFile("show.json").copyFileTo(target)) return;
    backups.push_back({target, stem.substring(5).toStdString(), 1});
    for (std::size_t i = 0; backups.size() - i > static_cast<std::size_t>(kMaxBackups); ++i) backups[i].file.deleteFile();
}

}  // namespace bigtop::io
