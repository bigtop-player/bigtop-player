// StartPanel.cpp: folder choosers (async, so nothing blocks the window), the
// restore-backup offer, and show export / import (worker thread).
#include "StartPanel.h"

#include "bigtop/io/ShowArchive.h"

namespace bigtop::app {

namespace {

constexpr int kButtonHeight = 64;  // touch target, well above 48 px
constexpr int kButtonWidth = 360;

// "show-20261117-120431.json" -> "2026-11-17 12:04:31"; the name is the
// backup's time (a copied file can keep show.json's modification time).
juce::String backupTimeText(const juce::File& backup) {
    const auto s = backup.getFileNameWithoutExtension().substring(5, 20);
    if (s.length() != 15) return backup.getFileName();
    return s.substring(0, 4) + "-" + s.substring(4, 6) + "-" + s.substring(6, 8) + " " + s.substring(9, 11) + ":" +
           s.substring(11, 13) + ":" + s.substring(13, 15);
}

}  // namespace

StartPanel::StartPanel(ShowSession& session, std::function<void()> onOpened)
    : session_(session), onOpened_(std::move(onOpened)) {
    title_.setText("Bigtop Player", juce::dontSendNotification);
    title_.setFont(juce::FontOptions(32.0f));
    title_.setJustificationType(juce::Justification::centred);
    error_.setJustificationType(juce::Justification::centred);
    error_.setColour(juce::Label::textColourId, juce::Colours::orangered);
    new_.onClick = [this] { choose(true); };
    open_.onClick = [this] { choose(false); };
    restore_.onClick = [this] {
        std::string error;
        if (session_.restoreBackupAndOpen(failedDir_, backup_, error))
            opened();
        else
            showFailure(failedDir_, error, {});
    };
    back_.onClick = [this] { opened(); };
    export_.onClick = [this] {
        // Outside Edit the panel cannot be reached, but ask the table anyway (one source of truth).
        if (!session_.isOpen() || !session_.allowed(Action::SwitchShow)) return;
        // A file still converting has no FLAC yet: the zip would miss it.
        if (session_.importsRunning()) return note("Wait until the imports have finished, then export.", true);
        chooser_ = std::make_unique<juce::FileChooser>("Choose the folder for the exported ZIP",
                                                       juce::File::getSpecialLocation(juce::File::userDocumentsDirectory));
        chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                              [this](const juce::FileChooser& fc) {
                                  if (fc.getResult() != juce::File()) exportTo(fc.getResult());  // empty = cancelled
                              });
    };
    import_.onClick = [this] {
        if (session_.isOpen() && !session_.allowed(Action::SwitchShow)) return;
        chooser_ = std::make_unique<juce::FileChooser>(
            "Choose the show ZIP (.bigtop.zip)", juce::File::getSpecialLocation(juce::File::userDocumentsDirectory), "*.zip");
        chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [this](const juce::FileChooser& fc) {
                                  const auto zip = fc.getResult();
                                  if (zip == juce::File()) return;  // cancelled
                                  chooser2_ = std::make_unique<juce::FileChooser>(
                                      "Choose an empty folder to unpack the show into",
                                      juce::File::getSpecialLocation(juce::File::userDocumentsDirectory));
                                  chooser2_->launchAsync(
                                      juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                                      [this, zip](const juce::FileChooser& fc2) {
                                          if (fc2.getResult() != juce::File()) importFrom(zip, fc2.getResult());
                                      });
                              });
    };
    for (auto* c : std::initializer_list<juce::Component*>{&title_, &error_, &new_, &open_, &import_})
        addAndMakeVisible(c);
    addChildComponent(restore_);
    addChildComponent(back_);
    addChildComponent(export_);
}

void StartPanel::visibilityChanged() {
    back_.setVisible(session_.isOpen());
    export_.setVisible(session_.isOpen());  // it exports the open show
    if (session_.isOpen()) back_.setButtonText("Back to " + session_.status().showName);
    resized();
}

void StartPanel::choose(bool createNew) {
    // A show is a folder, not a file: the titles say so (show files travel as zips through Export / Import).
    const auto title = createNew ? "Choose an empty folder for the new show" : "Choose the show folder";
    chooser_ = std::make_unique<juce::FileChooser>(title, session_.browseStart());
    chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                          [this, createNew](const juce::FileChooser& fc) {
                              const auto dir = fc.getResult();
                              if (dir == juce::File()) return;  // cancelled
                              if (!createNew) return tryOpen(dir);
                              std::string error;
                              if (session_.createNew(dir, error))
                                  opened();
                              else
                                  showFailure(dir, error, {});
                          });
}

void StartPanel::tryOpen(const juce::File& dir) {
    std::string error;
    std::vector<juce::File> backups;
    if (session_.open(dir, error, &backups))
        opened();
    else
        showFailure(dir, error, std::move(backups));
}

void StartPanel::note(const juce::String& text, bool isError) {
    error_.setColour(juce::Label::textColourId, isError ? juce::Colours::orangered : juce::Colours::lightgreen);
    error_.setText(text, juce::dontSendNotification);
}

void StartPanel::setBusy(bool busy) {
    // One archive job at a time, and nothing else may open or switch a show under it.
    for (auto* b : {&new_, &open_, &export_, &import_, &restore_, &back_}) b->setEnabled(!busy);
}

void StartPanel::exportTo(const juce::File& folder) {
    const auto dir = session_.showDir();
    const auto show = session_.controller().show();  // a copy: later edits must not change it under the worker
    const auto zip = folder.getChildFile(io::archiveName(dir.getFileName(), juce::Time::getCurrentTime()));
    restore_.setVisible(false);
    note("Exporting to " + zip.getFullPathName() + " ...", false);
    setBusy(true);
    pool_.addJob([dir, show, zip, self = juce::Component::SafePointer<StartPanel>(this)] {
        const auto r = io::exportShow(dir, show, zip);
        juce::MessageManager::callAsync([self, r, zip] {
            if (self == nullptr) return;
            self->setBusy(false);
            if (!r.ok) return self->note("Export failed: " + juce::String(r.error), true);
            if (r.omitted.empty()) return self->note("Exported: " + zip.getFullPathName(), false);
            juce::StringArray names;
            for (const auto& p : r.omitted) names.add(juce::String::fromUTF8(p.c_str()));
            self->note("Exported, but " + juce::String(static_cast<int>(r.omitted.size())) + " audio file(s) are missing and not in the ZIP: " +
                           names.joinIntoString(", "),
                       true);
        });
    });
}

void StartPanel::importFrom(const juce::File& zip, const juce::File& emptyDir) {
    restore_.setVisible(false);
    note("Importing " + zip.getFileName() + " ...", false);
    setBusy(true);
    pool_.addJob([zip, emptyDir, self = juce::Component::SafePointer<StartPanel>(this)] {
        const auto r = io::importShow(zip, emptyDir);
        juce::MessageManager::callAsync([self, r, emptyDir] {
            if (self == nullptr) return;
            self->setBusy(false);
            if (!r.ok) return self->note("Import failed: " + juce::String(r.error), true);
            // Opening also starts the conversion of the FLACs the zip left out.
            std::string error;
            if (self->session_.open(emptyDir, error, nullptr))
                self->opened();
            else
                self->note("Unpacked to " + emptyDir.getFullPathName() + ", but it did not open: " + juce::String(error), true);
        });
    });
}

void StartPanel::opened() {
    error_.setText({}, juce::dontSendNotification);
    restore_.setVisible(false);
    failedDir_ = juce::File();
    onOpened_();
}

void StartPanel::showMissing(const juce::String& path) {
    note("'" + juce::File(path).getFileName() + "' was not found at " + path, true);
}

void StartPanel::showFailure(const juce::File& dir, const std::string& error, std::vector<juce::File> backups) {
    failedDir_ = dir;
    error_.setColour(juce::Label::textColourId, juce::Colours::orangered);
    error_.setText(dir.getFullPathName() + "\n" + juce::String(error), juce::dontSendNotification);
    restore_.setVisible(!backups.empty());
    if (!backups.empty()) {
        backup_ = backups.front();  // newest loadable
        restore_.setButtonText("Restore backup from " + backupTimeText(backup_));
    }
}

void StartPanel::resized() {
    auto area = getLocalBounds().reduced(16);
    auto column = area.withSizeKeepingCentre(juce::jmin(kButtonWidth * 2, area.getWidth()), area.getHeight());
    title_.setBounds(column.removeFromTop(80));
    column.removeFromTop(16);
    const auto centred = [&](juce::Rectangle<int> r) {
        return r.withSizeKeepingCentre(juce::jmin(kButtonWidth, r.getWidth()), kButtonHeight);
    };
    for (auto* b : {&back_, &new_, &open_}) {
        b->setBounds(centred(column.removeFromTop(kButtonHeight)));
        column.removeFromTop(16);
    }
    // Export (only with an open show) and Import share one row.
    auto row = centred(column.removeFromTop(kButtonHeight));
    if (export_.isVisible()) {
        export_.setBounds(row.removeFromLeft((row.getWidth() - 8) / 2));
        row.removeFromLeft(8);
    }
    import_.setBounds(row);
    column.removeFromTop(16);
    restore_.setBounds(centred(column.removeFromTop(kButtonHeight)));
    column.removeFromTop(16);
    error_.setBounds(column.removeFromTop(96));
}

}  // namespace bigtop::app
