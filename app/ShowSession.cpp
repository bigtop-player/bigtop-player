// ShowSession.cpp: opening shows, saving edits, the 4 Hz housekeeping and the
// audio device choice, and the edits: drop to import, delete, move.
#include "ShowSession.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <utility>

#include "bigtop/Gain.h"
#include "bigtop/ShowEdits.h"

namespace bigtop::app {

namespace {

constexpr int kTicksPerRetry = 8;            // 4 Hz tick -> device retry every 2 s
constexpr juce::uint32 kStallMs = 1000;      // running but no engine frames this long = stalled

bool typeAvailable(juce::AudioDeviceManager& devices, const juce::String& type) {
    for (auto* t : devices.getAvailableDeviceTypes())
        if (t->getTypeName() == type) return true;
    return false;
}

// The setup a stored device state names, read the way JUCE's
// initialiseFromXML reads it, so a device missing at launch can still be the
// desired one (JUCE forgets it once it fails to open).
juce::AudioDeviceManager::AudioDeviceSetup setupFromXml(const juce::XmlElement& xml) {
    juce::AudioDeviceManager::AudioDeviceSetup s;
    s.outputDeviceName = xml.getStringAttribute("audioOutputDeviceName");
    s.sampleRate = xml.getDoubleAttribute("audioDeviceRate", s.sampleRate);
    s.bufferSize = xml.getIntAttribute("audioDeviceBufferSize", s.bufferSize);
    s.outputChannels.parseString(xml.getStringAttribute("audioDeviceOutChans", "11"), 2);
    s.useDefaultOutputChannels = !xml.hasAttribute("audioDeviceOutChans");
    s.inputChannels.clear();
    s.useDefaultInputChannels = false;
    return s;
}

// An Offline session's socket: accepts everything, sends nothing.
class DiscardOut : public io::UdpOut {
public:
    bool bind(const juce::String&) override { return true; }
    bool send(const juce::String&, const uint8_t*, int) override { return true; }
};

}  // namespace

std::vector<io::NetAdapter> ShowSession::adapters() const { return fakeAdapters_ ? *fakeAdapters_ : io::listAdapters(); }

ShowSession::ShowSession(io::AppSettings& settings, juce::AudioDeviceManager& devices, const Offline* offline)
    : settings_(settings), devices_(devices), engine_(std::make_unique<Engine>(io::kEngineSampleRate)) {
    readThread_.startThread();
    if (offline != nullptr) {
        fakeAdapters_ = offline->adapters;
        fixedSavedTime_ = offline->savedTime;
        fakeGainReductionDb_ = offline->gainReductionDb;
        fakeOutputPeaks_ = offline->outputPeaks;
    }
    // The lambda captures the engine, not this: the sender thread must not touch the session.
    sender_ = std::make_unique<io::ArtNetSender>([engine = engine_.get()] { return engine->snapshot(); },
                                                 offline != nullptr ? std::unique_ptr<io::UdpOut>(std::make_unique<DiscardOut>())
                                                                    : io::makeDatagramOut(),
                                                 true, [this] { return adapters(); });
    if (offline != nullptr) {  // no device is opened: nothing can play or be heard, the engine is driven by hand
        output_ = std::make_unique<io::AudioOutput>(*engine_, devices_);
        return;
    }

    // Outputs only, stereo. With a stored state JUCE opens that device, or a
    // default one if it is missing (which AudioOutput then mutes, see below).
    const auto stored = settings_.audioDeviceState();
    devices_.initialise(0, 2, stored.get(), true);
    output_ = std::make_unique<io::AudioOutput>(*engine_, devices_);

    const auto type = stored != nullptr ? stored->getStringAttribute("deviceType") : juce::String();
    const auto name = stored != nullptr ? stored->getStringAttribute("audioOutputDeviceName") : juce::String();
    const auto* open = devices_.getCurrentAudioDevice();
    if (stored != nullptr && open != nullptr && devices_.getCurrentAudioDeviceType() == type && open->getName() == name) {
        // The stored device opened: take its real setup (rate and buffer as opened).
        output_->setDesiredSetup(devices_.getAudioDeviceSetup(), type);
        hasDesired_ = true;
    } else if (name.isNotEmpty() && typeAvailable(devices_, type)) {
        // Missing at launch: keep wanting it; the fallback stays silent and it is retried.
        output_->setDesiredSetup(setupFromXml(*stored), type);
        hasDesired_ = true;
    } else if (open != nullptr) {
        // First launch, or the stored type is unknown on this machine (wanting
        // it would mute forever): what opened becomes the choice, and is
        // stored, so unplugging it later is reported and retried.
        output_->setDesiredSetup(devices_.getAudioDeviceSetup(), devices_.getCurrentAudioDeviceType());
        hasDesired_ = true;
        settings_.setAudioDeviceState(*stateXml());
    }
    // Otherwise the user chose "no device": nothing desired, nothing opened.
}

std::unique_ptr<juce::XmlElement> ShowSession::stateXml() const {
    if (auto xml = devices_.createStateXml()) return xml;
    // JUCE writes its state only for an explicitly chosen setup, not for the
    // default it opened on first launch; this mirrors its attribute names.
    const auto setup = devices_.getAudioDeviceSetup();
    auto xml = std::make_unique<juce::XmlElement>("DEVICESETUP");
    xml->setAttribute("deviceType", devices_.getCurrentAudioDeviceType());
    xml->setAttribute("audioOutputDeviceName", setup.outputDeviceName);
    xml->setAttribute("audioInputDeviceName", setup.inputDeviceName);
    xml->setAttribute("audioDeviceRate", setup.sampleRate);
    xml->setAttribute("audioDeviceBufferSize", setup.bufferSize);
    return xml;
}

ShowSession::~ShowSession() {
    sender_.reset();  // its thread reads the engine
    // Joined before anything goes: a running conversion finishes (an import is
    // never left half written under its real name anyway, see Importer.h).
    hashPool_.removeAllJobs(true, -1);
    importPool_.removeAllJobs(true, -1);
    peaksCancel_->store(true);  // a running waveform read stops at its next block
    peakPool_.removeAllJobs(true, -1);
    output_.reset();  // no audio callback may run while the show's sources go
}

bool ShowSession::quiet(std::string& error) {
    if (provider_ == nullptr) return true;
    // Every voice in the engine holds one of the provider's sources, and the
    // provider frees a source only once nothing can read it, so "no live
    // sources" is exactly "nothing of this show can play any more".
    provider_->collect(engine_->snapshot());
    if (provider_->liveSources() == 0) return true;
    // Without a playing device the engine stands still, so its sounds can
    // never finish and waiting would not help.
    error = output_->running() ? "Sounds are still playing. Fade out, wait until all is silent, then try again."
                               : "No audio device is playing, so this show's last sounds cannot finish. Get the "
                                 "audio device back (or restart the app), then try again.";
    return false;
}

bool ShowSession::open(const juce::File& dir, std::string& error, std::vector<juce::File>* backupsOnFailure) {
    if (!quiet(error)) return false;
    auto folder = std::make_unique<io::ShowFolder>(dir);
    auto result = folder->open(juce::Time::getCurrentTime());
    if (!result.show) {
        error = result.error;
        if (backupsOnFailure != nullptr) *backupsOnFailure = result.loadableBackups;
        return false;
    }
    auto provider = std::make_unique<io::StreamingSourceProvider>(dir, readThread_);
    provider->setShow(*result.show);
    auto controller = std::make_unique<ShowController>(*engine_, *provider);
    controller->setShow(std::move(*result.show));
    const auto state = folder->loadState();
    controller->selectNext(state.nextIndex);

    // The old controller refers to the old provider, so it goes first.
    controller_ = std::move(controller);
    provider_ = std::move(provider);
    folder_ = std::move(folder);
    mode_ = state.mode;
    // Imports still running for the old show finish into its folder, but their
    // results must not touch this one.
    importToken_ = std::make_shared<int>(0);
    importing_.clear();
    pendingDrops_.clear();
    importErrors_.clear();
    peaksCancel_->store(true);  // the old show's waveform reads stop; their results are dropped by the token
    peaksCancel_ = std::make_shared<std::atomic<bool>>(false);
    peaks_.clear();
    saveOk_ = true;
    saveText_.clear();
    settings_.setLastShow(dir);
    configureSender();
    // An audio file whose FLAC is missing but whose original is kept (an imported
    // zip, or a deleted audio/ file) is converted again: its rows say "importing..."
    // and GO is refused until done, exactly like a fresh drop.
    for (const FileId id : io::filesToRebuild(dir, controller_->show())) {
        const auto& files = controller_->show().files;
        const auto& entry = *std::find_if(files.begin(), files.end(), [&](const AudioFile& f) { return f.id == id; });
        convert(dir.getChildFile(juce::String::fromUTF8(entry.sourcePath.c_str())), id,
                dir.getChildFile(juce::String::fromUTF8(entry.path.c_str())), {});
    }
    // Files being rebuilt get theirs when the conversion is done (converted()).
    for (const auto& f : controller_->show().files)
        if (!importing_.contains(f.id)) readPeaksOf(f);
    return true;
}

bool ShowSession::createNew(const juce::File& dir, std::string& error) {
    if (!quiet(error)) return false;  // before creating anything on disk
    return io::ShowFolder::createNew(dir, error) && open(dir, error, nullptr);
}

bool ShowSession::restoreBackupAndOpen(const juce::File& dir, const juce::File& backup, std::string& error) {
    if (!quiet(error)) return false;  // the broken show.json is replaced only if we can open
    if (!io::ShowFolder(dir).restoreBackup(backup)) {
        error = "The backup could not be restored.";
        return false;
    }
    return open(dir, error, nullptr);
}

juce::File ShowSession::showDir() const { return folder_ != nullptr ? folder_->dir() : juce::File(); }

bool ShowSession::isOpen() const noexcept { return folder_ != nullptr; }

ShowController& ShowSession::controller() {
    jassert(isOpen());
    return *controller_;
}

const io::StreamingSourceProvider& ShowSession::sources() const {
    jassert(isOpen());
    return *provider_;
}

void ShowSession::saved(io::SaveResult result) {
    saveOk_ = result == io::SaveResult::Saved;
    switch (result) {
        case io::SaveResult::Saved:
            saveText_ = "Saved " + (fixedSavedTime_.isNotEmpty() ? fixedSavedTime_ : juce::Time::getCurrentTime().formatted("%H:%M:%S"));
            break;
        case io::SaveResult::ReadBackFailed:
            saveText_ = "NOT SAVED: the show could not be read back";
            break;
        case io::SaveResult::WriteFailed:
            saveText_ = "NOT SAVED: the disk refused the write";
            break;
    }
}

void ShowSession::commit(Show edited) {
    jassert(isOpen());
    controller_->updateShow(std::move(edited));
    provider_->setShow(controller_->show());  // new steps get their heads before any GO
    configureSender();
    saved(folder_->save(controller_->show(), juce::Time::getCurrentTime()));
    // The next step keeps its id but may have moved, so its index is saved too;
    // a show.json failure must stay visible, so only a state failure on top of a
    // good save changes the text.
    const auto stateResult = folder_->saveState({controller_->nextIndex(), mode_});
    if (saveOk_ && stateResult != io::SaveResult::Saved) saved(stateResult);
}

void ShowSession::configureSender() {
    if (!isOpen()) return;
    const auto& st = controller_->show().settings;
    const juce::String config = juce::String(st.artnetTarget) + "|" + settings_.artNetAdapter() + "|" + juce::String(st.timecodeFps);
    if (config == senderConfig_) return;  // configure() rebinds the socket, so only on a real change
    senderConfig_ = config;
    sender_->configure(juce::String(st.artnetTarget), settings_.artNetAdapter(), st.timecodeFps);
}

void ShowSession::setArtNetAdapter(const juce::String& address) {
    settings_.setArtNetAdapter(address);
    configureSender();
}

bool ShowSession::edit(Action action, const std::function<bool(Show&)>& change) {
    if (!isOpen() || !allowed(action)) return false;
    Show edited = controller_->show();
    if (!change(edited)) return false;
    commit(std::move(edited));
    return true;
}

bool ShowSession::keepFader(StepId id) {
    if (!isOpen() || !allowed(Action::KeepFader)) return false;
    const auto snap = engine_->snapshot();
    const auto* end = snap.voices + snap.numVoices;
    const auto* voice = std::find_if(snap.voices, end, [id](const VoiceState& v) { return v.key == id; });
    // The -inf stop folds to a silent programmed gain, which the level range cannot store.
    if (voice == end || voice->trim <= 0.0f) return false;
    const double trimDb = gainToDb(static_cast<double>(voice->trim));
    const auto& steps = controller_->show().steps;
    const auto step = std::find_if(steps.begin(), steps.end(), [id](const Step& s) { return s.id == id; });
    if (step == steps.end() || !step->sound) return false;  // a removed step has no level to store into
    if (!controller_->keepTrim(id)) {
        flash("Engine busy, press again");
        return false;
    }
    // The level heard may come from a later step's Level change, not the start step.
    return edit(Action::KeepFader, [&](Show& show) { return keepIntoShow(show, id, controller_->levelSetter(id), trimDb); });
}

void ShowSession::stateChanged() {
    jassert(isOpen());
    const auto result = folder_->saveState({controller_->nextIndex(), mode_});
    // A good state save must not hide a failed show.json save.
    if (result != io::SaveResult::Saved) saved(result);
}

ShowMode ShowSession::mode() const noexcept { return isOpen() ? mode_ : ShowMode::Edit; }

bool ShowSession::setMode(ShowMode mode) {
    if (!isOpen()) return false;
    // The engine's own word too: a pause the controller does not know about
    // (one left from another show) must not hide behind Show mode either.
    if (mode == ShowMode::Show && !controller_->show().settings.pauseInShow &&
        (controller_->paused() || engine_->snapshot().paused)) {
        flash(juce::String::fromUTF8(kShowRefusedPaused));
        return false;
    }
    mode_ = mode;
    stateChanged();
    // Drops hashed while drops were not allowed: added now, in drop order.
    if (bigtop::app::allowed(mode_, Action::DropFiles))
        for (auto& d : std::exchange(pendingDrops_, {})) hashed(d.files, d.hashes, d.index);
    return true;
}

bool ShowSession::allowed(Action action) {
    if (!isOpen()) return false;
    if (bigtop::app::allowed(mode_, action)) return true;
    // Say which mode refuses and the way out, so the operator is never left guessing.
    flash(mode_ == ShowMode::Show
              ? juce::String("Show mode: locked (hold a mode button for 2 s to leave)")
              : juce::String("Rehearsal mode: only levels and times can change (switch to Edit)"));
    return false;
}

void ShowSession::importFiles(const juce::StringArray& paths, int index) {
    if (!allowed(Action::DropFiles)) return;
    std::vector<juce::File> files;
    juce::StringArray refused, folders;
    for (const auto& path : paths) {
        const juce::File f(path);
        if (f.isDirectory())
            folders.add(f.getFileName());
        else if (io::isImportable(f) && f.existsAsFile())
            files.push_back(f);
        else
            refused.add(f.getFileName());
    }
    juce::StringArray notices;
    if (!folders.isEmpty()) notices.add("Folders are not imported; drop the audio files: " + folders.joinIntoString(", "));
    if (!refused.isEmpty()) notices.add("Not added (only WAV, MP3 and FLAC files): " + refused.joinIntoString(", "));
    if (!notices.isEmpty()) flash(notices.joinIntoString(" | "));
    if (files.empty()) return;
    // Only the token and the files cross threads; the show is touched in hashed().
    hashPool_.addJob([this, files, index, token = std::weak_ptr<int>(importToken_)] {
        std::vector<std::string> hashes;
        for (const auto& f : files) hashes.push_back(io::hashFile(f));
        juce::MessageManager::callAsync([this, files, hashes, index, token] {
            if (token.lock() != nullptr) hashed(files, hashes, index);
        });
    });
}

void ShowSession::hashed(const std::vector<juce::File>& files, const std::vector<std::string>& hashes, int index) {
    if (!bigtop::app::allowed(mode_, Action::DropFiles)) {
        // The mode left Edit since the drop: the show must not change now.
        pendingDrops_.push_back({files, hashes, index});
        size_t waiting = 0;
        for (const auto& d : pendingDrops_) waiting += d.files.size();
        flash(juce::String(static_cast<int>(waiting)) + (waiting == 1 ? " dropped file waits" : " dropped files wait") +
              " for Edit mode");
        return;
    }
    std::vector<DroppedFile> dropped;
    std::vector<juce::File> sources;  // parallel to dropped
    juce::StringArray unreadable;
    for (size_t i = 0; i < files.size(); ++i) {
        if (hashes[i].empty()) {
            unreadable.add(files[i].getFileName());
            continue;
        }
        dropped.push_back({files[i].getFileNameWithoutExtension().toStdString(), hashes[i]});
        sources.push_back(files[i]);
    }
    if (dropped.empty()) {  // nothing readable: no edit, no save
        flash("Could not be read: " + unreadable.joinIntoString(", "));
        return;
    }
    Show edited = controller_->show();
    const auto audioDir = folder_->audioDir();
    const auto out = addDroppedFiles(edited, index, dropped, io::kImportVersion, [&](const DroppedFile& d, const Show& show) {
        return "audio/" + io::chooseAudioTarget(audioDir, juce::String::fromUTF8(d.name.c_str()), show)
                              .getFileName()
                              .toStdString();
    });
    juce::StringArray notices;
    if (!unreadable.isEmpty()) notices.add("Could not be read: " + unreadable.joinIntoString(", "));
    if (!out.empty() && out.back().step == 0) notices.add("Some files were not added: the show has no step ids left");
    if (!notices.isEmpty()) flash(notices.joinIntoString(" | "));

    // Which files need converting, decided before the commit so the original's
    // place (sourcePath) is in show.json too. A known file whose audio is gone
    // (failed or crashed import) is imported again from this drop; one whose
    // audio is there is left alone.
    std::vector<bool> toConvert(out.size(), false);
    std::vector<std::pair<juce::File, std::string>> keepOnlyJobs;  // known files whose audio is fine but lack an original
    std::set<FileId> seen;
    for (size_t i = 0; i < out.size(); ++i) {
        if (out[i].step == 0 || !seen.insert(out[i].file).second) continue;
        auto& entry = *std::find_if(edited.files.begin(), edited.files.end(), [&](const AudioFile& f) { return f.id == out[i].file; });
        toConvert[i] = out[i].newFile || !folder_->dir().getChildFile(juce::String::fromUTF8(entry.path.c_str())).existsAsFile();
        // Any file lacking an original gets one from this drop, converted or not.
        if (entry.sourcePath.empty()) entry.sourcePath = io::sourcePathFor(entry.sourceHash, sources[i]);
        if (!toConvert[i]) keepOnlyJobs.emplace_back(sources[i], entry.sourcePath);
    }

    // In show.json before converting, so a crash mid-import leaves the step red, not lost.
    commit(std::move(edited));
    for (const auto& [src, rel] : keepOnlyJobs) keepOnly(src, rel);
    const auto& showFiles = controller_->show().files;
    for (size_t i = 0; i < out.size(); ++i) {
        if (!toConvert[i] || importing_.contains(out[i].file)) continue;
        const auto& entry = *std::find_if(showFiles.begin(), showFiles.end(),
                                          [&](const AudioFile& f) { return f.id == out[i].file; });
        convert(sources[i], out[i].file, folder_->dir().getChildFile(juce::String::fromUTF8(entry.path.c_str())), entry.sourcePath);
    }
}

void ShowSession::importToPad(const juce::File& file, int index) {
    if (!allowed(Action::AssignPad)) return;
    const auto& st = controller_->show().settings;
    if (index < 0 || index >= st.padRows * st.padCols) return;  // hidden pads cannot be assigned
    if (file.isDirectory()) {
        flash("Folders are not imported; drop an audio file: " + file.getFileName());
        return;
    }
    if (!io::isImportable(file) || !file.existsAsFile()) {
        flash("Not added (only WAV, MP3 and FLAC files): " + file.getFileName());
        return;
    }
    hashPool_.addJob([this, file, index, token = std::weak_ptr<int>(importToken_)] {
        const auto hash = io::hashFile(file);
        juce::MessageManager::callAsync([this, file, hash, index, token] {
            if (token.lock() != nullptr) padHashed(file, hash, index);
        });
    });
}

void ShowSession::padHashed(const juce::File& file, const std::string& hash, int index) {
    if (!bigtop::app::allowed(mode_, Action::AssignPad)) {
        flash("Pad not assigned: Edit mode ended");
        return;
    }
    if (hash.empty()) {
        flash("Could not be read: " + file.getFileName());
        return;
    }
    Show edited = controller_->show();
    // The grid may have shrunk while the file was hashed.
    if (index < 0 || index >= edited.settings.padRows * edited.settings.padCols) return;
    // fromJson can leave the vector shorter than the grid: grow it, never index past it.
    if (edited.pads.size() <= static_cast<size_t>(index)) edited.pads.resize(static_cast<size_t>(index) + 1);
    bool isNew = false;
    FileId id = 0;
    if (const auto known = findFileByHash(edited, hash, io::kImportVersion)) {
        id = *known;
    } else {
        id = addFile(edited, "audio/" + io::chooseAudioTarget(folder_->audioDir(), file.getFileNameWithoutExtension(), edited)
                                           .getFileName()
                                           .toStdString(),
                     hash, io::kImportVersion);
        isNew = true;
    }
    auto& entry = *std::find_if(edited.files.begin(), edited.files.end(), [&](const AudioFile& f) { return f.id == id; });
    const auto target = folder_->dir().getChildFile(juce::String::fromUTF8(entry.path.c_str()));
    // A known file with its audio gone is imported again, like a step drop.
    const bool needConvert = isNew || !target.existsAsFile();
    if (entry.sourcePath.empty()) entry.sourcePath = io::sourcePathFor(entry.sourceHash, file);
    const auto keepAs = entry.sourcePath;
    Pad& pad = edited.pads[static_cast<size_t>(index)];
    pad.file = id;
    pad.name = file.getFileNameWithoutExtension().toStdString();
    pad.startSeconds = 0.0;  // a new sound plays whole: the old range belonged to the old file
    pad.endSeconds.reset();
    commit(std::move(edited));  // in show.json before converting, as for steps
    if (!needConvert)
        keepOnly(file, keepAs);
    else if (!importing_.contains(id))
        convert(file, id, target, keepAs);
}

void ShowSession::convert(const juce::File& source, FileId file, const juce::File& target, const std::string& keepAs) {
    importing_.insert(file);
    importErrors_.erase(file);
    const auto showDir = folder_->dir();
    importPool_.addJob([this, source, file, target, showDir, keepAs, token = std::weak_ptr<int>(importToken_)] {
        // The original first: it is what an export carries and what a rebuild starts from.
        // A failed copy only loses that convenience (export then falls back to the FLAC).
        if (!keepAs.empty()) io::keepOriginal(source, showDir, keepAs);
        target.getParentDirectory().createDirectory();  // audio/ may be gone (rebuild after a copy)
        const auto result = io::convertToShowAudio(source, target);
        juce::MessageManager::callAsync([this, file, result, token] {
            if (token.lock() != nullptr) converted(file, result);
        });
    });
}

void ShowSession::keepOnly(const juce::File& source, const std::string& keepAs) {
    // On the import thread, after any running conversion: copying a big file must not stall the UI.
    importPool_.addJob([source, showDir = folder_->dir(), keepAs] { io::keepOriginal(source, showDir, keepAs); });
}

void ShowSession::converted(FileId file, const io::ImportResult& result) {
    importing_.erase(file);
    if (!result.ok) importErrors_[file] = result.error;
    // Only now can the provider load the head, which makes the step playable.
    provider_->setShow(controller_->show());
    const auto& files = controller_->show().files;
    const auto entry = std::find_if(files.begin(), files.end(), [file](const AudioFile& f) { return f.id == file; });
    if (result.ok && entry != files.end()) readPeaksOf(*entry);
}

void ShowSession::readPeaksOf(const AudioFile& file) {
    const auto flac = folder_->dir().getChildFile(juce::String::fromUTF8(file.path.c_str()));
    peakPool_.addJob([this, flac, id = file.id, cancel = peaksCancel_, token = std::weak_ptr<int>(importToken_)] {
        auto peaks = std::make_shared<const io::Peaks>(io::readPeaks(flac, cancel.get()));
        if (peaks->frames == 0) return;  // unreadable or cancelled: the bar stays plain
        juce::MessageManager::callAsync([this, id, peaks, token] {
            if (token.lock() != nullptr) peaks_[id] = peaks;
        });
    });
}

std::shared_ptr<const io::Peaks> ShowSession::peaks(FileId file) const {
    const auto it = peaks_.find(file);
    return it != peaks_.end() ? it->second : nullptr;
}

void ShowSession::deleteStep(int index) {
    if (!allowed(Action::Delete)) return;
    const auto& steps = controller_->show().steps;
    if (index < 0 || index >= static_cast<int>(steps.size())) return;
    Show edited = controller_->show();
    removeStep(edited, steps[static_cast<size_t>(index)].id);  // keeps its file (design 7.2)
    commit(std::move(edited));
}

bool ShowSession::assignStepToPad(StepId step, int pad) {
    if (!isOpen()) return false;
    const auto& steps = controller_->show().steps;
    const auto it = std::find_if(steps.begin(), steps.end(), [step](const Step& s) { return s.id == step; });
    if (it == steps.end()) return false;
    if (!it->sound) {
        flash("A fade step has no sound to put on a pad");
        return false;
    }
    const Sound sound = *it->sound;
    const std::string name = it->name;
    return edit(Action::AssignPad, [&](Show& show) {
        if (pad < 0 || pad >= show.settings.padRows * show.settings.padCols) return false;
        if (show.pads.size() <= static_cast<size_t>(pad)) show.pads.resize(static_cast<size_t>(pad) + 1);
        Pad& p = show.pads[static_cast<size_t>(pad)];
        p.file = sound.file;  // the same file entry: nothing is imported again
        p.name = name;
        p.startSeconds = sound.startSeconds;
        p.endSeconds = sound.endSeconds;
        return true;
    });
}

void ShowSession::moveStep(int from, int to) {
    if (!allowed(Action::Reorder)) return;
    const auto& steps = controller_->show().steps;
    if (from < 0 || from >= static_cast<int>(steps.size()) || from == to) return;
    Show edited = controller_->show();
    bigtop::moveStep(edited, steps[static_cast<size_t>(from)].id, to);
    commit(std::move(edited));
}

std::optional<StepId> ShowSession::addFadeStep() {
    if (!isOpen() || !allowed(Action::EditStructure)) return std::nullopt;
    Show edited = controller_->show();
    const auto id = allocateStepId(edited);
    if (!id) {
        flash("No step ids left");
        return std::nullopt;
    }
    Step step = makeStep(*id, edited.settings, std::nullopt);
    step.name = "Fade";
    // Index NEXT+1 is "after NEXT"; insertSteps clamps it when NEXT is the last step or past the end.
    insertSteps(edited, controller_->nextIndex() + 1, {std::move(step)});
    commit(std::move(edited));
    return id;
}

bool ShowSession::importing(FileId file) const { return importing_.contains(file); }

juce::String ShowSession::importError(FileId file) const {
    const auto it = importErrors_.find(file);
    return it != importErrors_.end() ? juce::String::fromUTF8(it->second.c_str()) : juce::String();
}

io::OutputPeaks ShowSession::outputPeaks() { return fakeOutputPeaks_ ? fakeOutputPeaks_() : output_->takePeaks(); }

void ShowSession::tick() {
    if (provider_ != nullptr) provider_->collect(engine_->snapshot());
    // The limiter holds its deepest reduction until read, so sampling at 4 Hz
    // misses no over, even one the release has undone between two ticks.
    const float reductionDb = fakeGainReductionDb_ ? fakeGainReductionDb_() : output_->takePeakReductionDb();
    if (reductionDb > kClipThresholdDb) {
        clipLatched_ = true;  // stays lit until resetClip(): an over seen once is not missed
    }
    if (++ticks_ % kTicksPerRetry == 0 && !settingsOpen_) output_->retryIfLost();
    // While not running the clock restarts, so a device coming back is not
    // reported stalled before its first callbacks.
    const auto frames = engine_->snapshot().framesProcessed;
    if (frames != lastFrames_ || !output_->running()) {
        lastFrames_ = frames;
        lastAdvanceMs_ = uiMs();
    }
}

ShowSession::Status ShowSession::status() const {
    Status s;
    const bool running = output_->running();
    const bool stalled = running && uiMs() - lastAdvanceMs_ >= kStallMs;
    s.audioOk = running && !stalled;
    s.audioText = s.audioOk     ? juce::String("Audio OK")
                  : stalled     ? juce::String("Audio: device stalled")
                  : hasDesired_ ? juce::String(juce::CharPointer_UTF8("Audio: device lost \xe2\x80\x94 retrying"))
                                : juce::String("Audio: no device");
    s.clip = clipLatched_;
    s.droppedStarts = engine_->snapshot().droppedStarts;
    s.dropouts = provider_ != nullptr ? provider_->dropouts() : 0;
    s.saveOk = saveOk_;
    s.saveText = saveText_;
    if (folder_ != nullptr) s.showName = folder_->dir().getFileName();
    if (uiMs() - flashMs_ < kFlashMs) s.message = flashText_;
    return s;
}

Snapshot ShowSession::snapshot() const noexcept { return engine_->snapshot(); }

void ShowSession::flash(const juce::String& text) {
    flashText_ = text;
    flashMs_ = uiMs();
}

void ShowSession::setDeviceSettingsOpen(bool open) {
    if (open == settingsOpen_) return;
    settingsOpen_ = open;
    // JUCE changes this XML only for deliberate picks in the selector, so a
    // device lost (and a default opened) while the window is open does not
    // count as a choice. No XML yet (first launch) reads as empty.
    const auto choice = [this] {
        const auto xml = devices_.createStateXml();
        return xml != nullptr ? xml->toString() : juce::String();
    };
    if (open) {
        choiceAtOpen_ = choice();
        return;
    }
    if (choice() == choiceAtOpen_) return;
    adoptDevice();
}

void ShowSession::adoptDevice() {
    const auto setup = devices_.getAudioDeviceSetup();
    if (setup.outputDeviceName.isEmpty()) {
        // JUCE records an explicit pick in its state XML only when it worked,
        // so an empty name there means "none" was chosen; otherwise the pick
        // failed to open and the previous choice stays (and is retried).
        const auto xml = devices_.createStateXml();
        if (xml == nullptr || xml->getStringAttribute("audioOutputDeviceName").isNotEmpty()) return;
        output_->clearDesiredSetup();
        hasDesired_ = false;
    } else {
        output_->setDesiredSetup(setup, devices_.getCurrentAudioDeviceType());
        hasDesired_ = true;
    }
    settings_.setAudioDeviceState(*stateXml());
}

}  // namespace bigtop::app
