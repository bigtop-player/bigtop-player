// ShowArchive.cpp: export/import of a show as a .bigtop.zip, and the originals in sources/.
#include "bigtop/io/ShowArchive.h"

#include <memory>
#include <set>

#include "bigtop/ShowJson.h"

namespace bigtop::io {

namespace {

/// The resolved file for a relative path, or an invalid File when the path may
/// not be used. One check for zip entries, show.json paths and export, so no
/// route can write or read outside the show folder (zip-slip): an empty name,
/// a drive letter or backslash, an absolute path, or anything that resolves
/// (".." included) to somewhere else is refused.
juce::File inside(const juce::File& dir, const juce::String& rel) {
    if (rel.isEmpty() || rel.containsAnyOf("\\:") || rel.startsWithChar('/')) return {};
    const auto resolved = dir.getChildFile(rel);
    const auto base = dir.getFullPathName().trimCharactersAtEnd("/\\") + juce::File::getSeparatorString();
    return resolved.getFullPathName().startsWith(base) ? resolved : juce::File();
}

juce::File tmpOf(const juce::File& f) { return f.getSiblingFile(f.getFileName() + ".tmp"); }

/// Deletes everything in dir (the folder itself stays), to undo a refused unpack.
void clear(const juce::File& dir) {
    for (const auto& c : dir.findChildFiles(juce::File::findFilesAndDirectories, false)) {
        if (c.isDirectory())
            c.deleteRecursively();
        else
            c.deleteFile();
    }
}

/// One thing going into the zip: its stored name and where the bytes come from.
struct Entry {
    juce::String name;
    juce::File file;   // used unless this is in-memory text
    std::string text;  // in-memory content (show.json)
    bool inMemory = false;
    juce::int64 size = 0;
};

/// The JSON of a state.json with the mode forced to Edit and the next step
/// kept (0 when missing or unreadable): a show that arrives by zip must never
/// open locked in Show mode on the receiving laptop.
std::string editModeState(const juce::File& state) {
    const auto parsed = juce::JSON::parse(state);
    auto* o = new juce::DynamicObject();
    o->setProperty("nextIndex", parsed.getDynamicObject() != nullptr && parsed["nextIndex"].isInt() ? parsed["nextIndex"] : juce::var(0));
    o->setProperty("mode", "edit");
    return juce::JSON::toString(juce::var(o)).toStdString();
}

/// JUCE's zip builder keeps sizes and offsets in 32 bits (no zip64).
constexpr juce::int64 kZipLimit = 0xFFFFFFFFLL;

}  // namespace

std::string sourcePathFor(std::string_view hash, const juce::File& original) {
    return "sources/" + std::string(hash) + original.getFileExtension().toLowerCase().toStdString();
}

bool keepOriginal(const juce::File& source, const juce::File& showDir, const std::string& sourcePath) {
    const auto dest = inside(showDir, juce::String::fromUTF8(sourcePath.c_str()));
    if (dest == juce::File()) return false;
    if (dest.existsAsFile()) return true;  // the name is the content hash
    dest.getParentDirectory().createDirectory();
    const auto tmp = tmpOf(dest);
    tmp.deleteFile();  // a leftover of a crash must not be mistaken for our copy
    if (!source.copyFileTo(tmp) || tmp.getSize() != source.getSize() || !tmp.moveFileTo(dest)) {
        tmp.deleteFile();
        return false;
    }
    return true;
}

juce::String archiveName(const juce::String& showName, juce::Time when) {
    return showName.replaceCharacters("\\/:*?\"<>|", "_________") + "-" + when.formatted("%Y%m%d-%H%M") + ".bigtop.zip";
}

ArchiveResult exportShow(const juce::File& showDir, const Show& show, const juce::File& zipTarget) {
    std::vector<Entry> entries;
    std::set<juce::String> names;
    const auto add = [&](Entry e) {
        if (names.insert(e.name).second) entries.push_back(std::move(e));
    };
    try {
        auto text = toJson(show);
        const auto size = static_cast<juce::int64>(text.size());
        add({"show.json", {}, std::move(text), true, size});
    } catch (const std::exception& e) {
        return {false, std::string("The show cannot be written: ") + e.what()};
    }
    if (const auto state = showDir.getChildFile("state.json"); state.existsAsFile()) {
        auto text = editModeState(state);
        const auto size = static_cast<juce::int64>(text.size());
        add({"state.json", {}, std::move(text), true, size});
    }

    // Only files a step or pad plays: unused ones would be dead weight.
    std::set<FileId> used;
    for (const auto& s : show.steps)
        if (s.sound) used.insert(s.sound->file);
    for (const auto& p : show.pads)
        if (p.file) used.insert(*p.file);
    std::vector<std::string> omitted;
    for (const auto& f : show.files) {
        if (!used.contains(f.id)) continue;
        const auto original = juce::String::fromUTF8(f.sourcePath.c_str());
        const auto flac = juce::String::fromUTF8(f.path.c_str());
        // The original is the smallest lossless form; the FLAC only when there is none.
        auto src = original.isEmpty() ? juce::File() : inside(showDir, original);
        auto rel = original;
        if (!src.existsAsFile()) {
            src = inside(showDir, flac);
            rel = flac;
            if (src == juce::File()) return {false, "An audio path leaves the show folder: " + f.path};
            if (!src.existsAsFile()) {  // nothing to carry; the step is red in the app too
                omitted.push_back(f.path);
                continue;
            }
        }
        add({rel, src, {}, false, src.getSize()});
    }
    // Refused before writing anything: past 4 GB the zip would only fail at
    // the read-back, after minutes of copying.
    juce::int64 total = 0;
    // Data plus the zip's own records: local header (30 + name) and central entry (46 + name) each, end record 22.
    for (const auto& e : entries) total += e.size + 76 + 2 * static_cast<juce::int64>(e.name.getNumBytesAsUTF8());
    total += 22;
    if (total > kZipLimit)
        return {false, "The show's audio adds up to more than 4 GB, which one zip cannot hold (" +
                           juce::File::descriptionOfSizeInBytes(total).toStdString() + ")."};

    // Build the zip next to the target so the final rename stays on one disk.
    const auto tmp = tmpOf(zipTarget);
    const auto fail = [&](std::string why) {
        tmp.deleteFile();
        return ArchiveResult{false, std::move(why)};
    };
    juce::ZipFile::Builder builder;
    for (const auto& e : entries) {
        if (e.inMemory)
            builder.addEntry(std::make_unique<juce::MemoryInputStream>(e.text.data(), e.text.size(), true), 0, e.name,
                             juce::Time::getCurrentTime());
        else
            builder.addFile(e.file, 0, e.name);  // level 0: stored, audio does not deflate
    }
    tmp.deleteFile();
    {
        juce::FileOutputStream out(tmp);
        if (out.failedToOpen()) return fail("Cannot write " + tmp.getFullPathName().toStdString());
        if (!builder.writeToStream(out, nullptr)) return fail("Writing the zip failed (disk full?)");
        out.flush();
        if (out.getStatus().failed()) return fail("Writing the zip failed (disk full?)");
    }
    // Read it back like an importer would: every entry present with its size.
    {
        juce::ZipFile check(tmp);
        if (check.getNumEntries() != static_cast<int>(entries.size())) return fail("The written zip did not read back correctly");
        for (const auto& e : entries) {
            const auto i = check.getIndexOfFileName(e.name);
            if (i < 0 || check.getEntry(i)->uncompressedSize != e.size)
                return fail("The written zip did not read back correctly: " + e.name.toStdString());
        }
    }
    if (!tmp.replaceFileIn(zipTarget)) return fail("Cannot write " + zipTarget.getFullPathName().toStdString());
    return {true, {}, std::move(omitted)};
}

ArchiveResult importShow(const juce::File& zipFile, const juce::File& emptyDir) {
    if (emptyDir.exists() && (!emptyDir.isDirectory() || emptyDir.getNumberOfChildFiles(juce::File::findFilesAndDirectories) > 0))
        return {false, "The folder is not empty."};  // nothing of ours inside: touch nothing
    if (!emptyDir.createDirectory().wasOk()) return {false, "Cannot create the folder."};
    const auto refuse = [&](std::string why) {
        clear(emptyDir);
        return ArchiveResult{false, std::move(why)};
    };

    // Scoped: the zip file handle must be closed before the caller may delete the zip.
    juce::ZipFile zip(zipFile);
    // A file that is not a zip, or whose end (central directory) is cut off, lists no entries.
    if (zip.getNumEntries() == 0) return refuse("Not a readable Bigtop show zip (damaged or empty).");

    // Check every name before unpacking any, so a hostile zip writes nothing at all.
    std::set<juce::String> seen;
    for (int i = 0; i < zip.getNumEntries(); ++i) {
        const auto& name = zip.getEntry(i)->filename;
        const auto dest = inside(emptyDir, name);
        if (dest == juce::File()) return refuse("The zip holds an unsafe path: " + name.toStdString());
        // Two entries for one file (Windows ignores case; "a/../b" is "b") would land in one
        // file, the second appended to the first: compare where they land, not how they are spelt.
        if (!seen.insert(dest.getFullPathName().toLowerCase()).second) return refuse("The zip holds the same name twice: " + name.toStdString());
    }
    for (int i = 0; i < zip.getNumEntries(); ++i) {
        const auto* entry = zip.getEntry(i);
        const auto dest = inside(emptyDir, entry->filename);
        if (entry->filename.endsWithChar('/')) {
            dest.createDirectory();
            continue;
        }
        const std::unique_ptr<juce::InputStream> in(zip.createStreamForEntry(i));
        if (in == nullptr) return refuse("The zip is damaged: " + entry->filename.toStdString());
        dest.getParentDirectory().createDirectory();
        juce::FileOutputStream out(dest);
        if (out.failedToOpen()) return refuse("Cannot write " + dest.getFullPathName().toStdString());
        // A cut-off or corrupt entry decodes to fewer bytes than it announces.
        if (out.writeFromInputStream(*in, -1) != entry->uncompressedSize)
            return refuse("The zip is damaged: " + entry->filename.toStdString());
    }

    const auto showJson = emptyDir.getChildFile("show.json");
    if (!showJson.existsAsFile()) return refuse("The zip has no show.json.");
    Show show;
    try {
        show = fromJson(showJson.loadFileAsString().toStdString());
    } catch (const std::exception& e) {
        return refuse(std::string("show.json cannot be read: ") + e.what());
    }
    // The app later reads and writes these paths, so they must stay in the folder too.
    for (const auto& f : show.files)
        for (const auto* p : {&f.path, &f.sourcePath})
            if (!p->empty() && inside(emptyDir, juce::String::fromUTF8(p->c_str())) == juce::File())
                return refuse("show.json names a path outside the folder: " + *p);

    // Normalised here too, not only on export: a zip from elsewhere may say Show mode.
    if (const auto state = emptyDir.getChildFile("state.json"); state.existsAsFile() && !state.replaceWithText(editModeState(state)))
        return refuse("Cannot write " + state.getFullPathName().toStdString());

    emptyDir.getChildFile("audio").createDirectory();
    emptyDir.getChildFile("backups").createDirectory();
    return {true, {}};
}

std::vector<FileId> filesToRebuild(const juce::File& showDir, const Show& show) {
    std::vector<FileId> ids;
    for (const auto& f : show.files) {
        if (f.sourcePath.empty()) continue;
        const auto flac = inside(showDir, juce::String::fromUTF8(f.path.c_str()));
        const auto original = inside(showDir, juce::String::fromUTF8(f.sourcePath.c_str()));
        if (flac != juce::File() && original != juce::File() && !flac.exists() && original.existsAsFile()) ids.push_back(f.id);
    }
    return ids;
}

}  // namespace bigtop::io
