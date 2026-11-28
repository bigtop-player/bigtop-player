// MainComponent.cpp: screen switching, the operator controls and the timer
// that drives the GO screen and the session.
#include "MainComponent.h"

#include "Confirm.h"
#include "KeepAwake.h"

namespace bigtop::app {

namespace {
constexpr int kUiHz = 30;               // GO screen refresh: one snapshot per tick
constexpr juce::uint32 kTickMs = 250;   // ShowSession::tick() is the 4 Hz housekeeping
constexpr juce::uint32 kStateSaveMs = 500;  // state.json at most this often while Up/Down repeat
constexpr int kSideWidth = 460;         // the step editor / show settings panel
constexpr int kButtonHeight = 96;       // GO is the biggest control on screen
}  // namespace

MainComponent::MainComponent(io::AppSettings& settings, juce::AudioDeviceManager& devices, const ShowSession::Offline* offline)
    : devices_(devices),
      session_(settings, devices, offline),
      start_(session_, [this] {
          choosing_ = false;
          showChanged();
      }),
      running_(session_),
      steps_(session_, settings),
      editor_(session_),
      settingsPanel_(session_),
      padEditor_(session_),
      pads_(session_) {
    setWantsKeyboardFocus(true);
    // An Offline session (screenshots, tests) never touches the machine's power state.
    keepAwake_ = offline == nullptr ? std::function<void(bool)>(setKeepAwake)
                                    : (offline->keepAwake ? offline->keepAwake : std::function<void(bool)>([](bool) {}));
    addChildComponent(start_);

    runningView_.setViewedComponent(&running_, false);
    runningView_.setScrollBarsShown(true, false);
    runningView_.setWantsKeyboardFocus(false);  // a Viewport eats arrow keys
    steps_.onSelect = [this](int row) { select(row); };
    goButton_.onClick = [this] { go(); };
    fadeButton_.onClick = [this] { fadeOut(); };
    pauseButton_.onClick = [this] { togglePause(); };
    running_.onEdit = [this](StepId id) { openEditor(id); };
    pausedBanner_.setJustificationType(juce::Justification::centred);
    pausedBanner_.setFont(juce::FontOptions(22.0f, juce::Font::bold));
    pausedBanner_.setColour(juce::Label::backgroundColourId, juce::Colour(0xffc77800));
    pausedBanner_.setColour(juce::Label::textColourId, juce::Colours::white);
    pausedBanner_.setInterceptsMouseClicks(false, false);  // rows underneath stay usable (seek, faders, Fade)
    steps_.onEdit = [this](StepId id) { openEditor(id); };
    editor_.onClose = [this] { closeSide(); };
    editor_.onDelete = [this](int index) { deleteAt(index); };
    settingsPanel_.onClose = [this] { closeSide(); };
    padEditor_.onClose = [this] { closeSide(); };
    pads_.onEditPad = [this](int index) { openPadEditor(index); };
    addChildComponent(editor_);
    addChildComponent(settingsPanel_);
    addChildComponent(padEditor_);
    addChildComponent(pads_);  // shown by resized() whenever no panel covers the slot
    addFadeButton_.onClick = [this] {
        if (const auto id = session_.addFadeStep()) {
            session_.flash("Fade step added after NEXT");
            openEditor(*id);  // its changes are what a fade step is for (design 6)
        }
        refreshStatus();
    };
    for (auto* b : {&goButton_, &fadeButton_, &addFadeButton_, &pauseButton_}) b->setWantsKeyboardFocus(false);
    addChildComponent(addFadeButton_);  // shown by resized() in Edit mode
    addChildComponent(pauseButton_);    // shown by resized() where the mode allows Pause
    addChildComponent(goFrame_);        // before GO, so it sits behind it; resized() shows it with GO
    for (juce::Component* c : {static_cast<juce::Component*>(&runningView_), static_cast<juce::Component*>(&steps_),
                               static_cast<juce::Component*>(&goButton_), static_cast<juce::Component*>(&fadeButton_)})
        addChildComponent(c);
    // Last child, so it draws over the step list; a tap dismisses, the hint opens Show check.
    alertBanner_.onDismiss = [this] { session_.dismissAlert(); refreshAlert(); };
    alertBanner_.onShowCheck = [this] { openShowCheck(); };
    showCheck_.onClose = [this] { showCheck_.setVisible(false); };
    settingsPanel_.onShowCheck = [this] { openShowCheck(); };
    addChildComponent(pausedBanner_);   // after the running view so it draws over it; shown by timerCallback() while paused
    addChildComponent(modeBar_);
    addAndMakeVisible(status_);
    addChildComponent(showCheck_);
    addChildComponent(alertBanner_);  // last, so it draws over the step list

    modeBar_.onPick = [this](ShowMode mode) { pickMode(mode); };
    modeBar_.setDetails(steps_.details());  // remembered per laptop (AppSettings)
    modeBar_.onDetails = [this](bool on) { steps_.setDetails(on); };
    status_.onSettings = [this] { toggleSettings(); };
    status_.onShows = [this] { chooseShow(); };
    status_.onShowSettings = [this] { openShowSettings(); };
    // A step dragged by its handle onto a pad gives the pad its sound.
    steps_.onDragOver = [this](std::optional<juce::Point<int>> screen) {
        const int pad = screen ? pads_.padAtScreen(*screen) : -1;
        pads_.setDropHighlight(pad);
        return pad >= 0;
    };
    steps_.onDropOutside = [this](StepId step, juce::Point<int> screen) {
        const int pad = pads_.padAtScreen(screen);
        if (pad < 0) return false;
        session_.assignStepToPad(step, pad);
        return true;
    };
    status_.onClipTapped = [this] {
        session_.resetClip();
        refreshStatus();
    };

    if (const auto last = settings.lastShow()) {
        std::string error;
        std::vector<juce::File> backups;
        if (!session_.open(*last, error, &backups)) start_.showFailure(*last, error, std::move(backups));
    } else if (const auto path = settings.lastShowPath(); path.isNotEmpty()) {
        start_.showMissing(path);
    }
    showChanged();
    startTimerHz(kUiHz);
    setSize(1024, 700);
}

MainComponent::~MainComponent() {
    if (awake_.value_or(false)) keepAwake_(false);
    stopTimer();
    flushState(true);  // a held Up/Down may not have been saved yet
    settingsWindow_.reset();  // it reports its final choice to the session
    closingWindow_.reset();
}

void MainComponent::pickMode(ShowMode mode) {
    flushState(true);
    if (!session_.setMode(mode)) {  // refused (paused, see kShowRefusedPaused): the strip keeps the old mode
        refreshStatus();
        return;
    }
    // Device settings are Edit-only, so the window goes when the mode leaves Edit.
    if (!allowed(mode, Action::AudioSettings)) settingsWindow_.reset();
    // The start panel over an open show is the Shows... action: close it when the mode forbids it.
    if (!allowed(mode, Action::SwitchShow)) choosing_ = false;
    showChanged();
}

void MainComponent::openEditor(StepId id) {
    if (!session_.isOpen() || !session_.allowed(Action::EditValue)) return;  // Show mode: the panel never opens
    settingsPanel_.setVisible(false);
    padEditor_.setVisible(false);
    editor_.open(id);  // never changes NEXT
    resized();
}

void MainComponent::openPadEditor(int index) {
    if (!session_.isOpen() || !session_.allowed(Action::EditValue)) return;  // Show mode: the panel never opens
    editor_.setVisible(false);
    settingsPanel_.setVisible(false);
    padEditor_.open(index);
    resized();
}

void MainComponent::openShowSettings() {
    if (!session_.isOpen() || !session_.allowed(Action::ShowSettings)) return;
    editor_.setVisible(false);
    padEditor_.setVisible(false);
    settingsPanel_.open();
    resized();
}

void MainComponent::closeSide() {
    editor_.setVisible(false);
    settingsPanel_.setVisible(false);
    padEditor_.setVisible(false);
    resized();
    if (session_.isOpen() && !choosing_) grabKeyboardFocus();  // not while the start panel is shown
}

void MainComponent::setDetails(bool on) {
    modeBar_.setDetails(on);
    steps_.setDetails(on);
}

void MainComponent::flushState(bool force) {
    if (!stateDirty_ || !session_.isOpen()) return;
    const auto now = uiMs();
    if (!force && now - lastStateSaveMs_ < kStateSaveMs) return;
    stateDirty_ = false;
    lastStateSaveMs_ = now;
    session_.stateChanged();
}

void MainComponent::chooseShow() {
    flushState(true);  // the show is about to be left
    // Refused while sounds play (design 8.4), with the same notice open() gives.
    std::string error;
    if (!session_.quiet(error)) {
        session_.flash(juce::String(error));
        session_.alert(AlertKind::Notice, juce::String(error));  // the status bar alone was too easy to miss
        refreshStatus();
        return;
    }
    choosing_ = true;
    showChanged();
}

juce::String MainComponent::windowTitle() const {
    const auto name = session_.status().showName;
    return name.isEmpty() ? juce::String("Bigtop Player") : "Bigtop Player " + juce::String::fromUTF8("\xe2\x80\x93 ") + name;
}

void MainComponent::showChanged() {
    if (auto* top = getTopLevelComponent(); top != nullptr && top != this) top->setName(windowTitle());
    const bool open = session_.isOpen() && !choosing_;
    start_.setVisible(!open);
    if (!open) {
        closeSide();  // a side panel belongs to the show that was open
        pausedBanner_.setVisible(false);
    }
    for (juce::Component* c : {static_cast<juce::Component*>(&runningView_), static_cast<juce::Component*>(&steps_),
                               static_cast<juce::Component*>(&goButton_), static_cast<juce::Component*>(&fadeButton_)})
        c->setVisible(open);
    resized();  // the Add fade step button depends on the screen and the mode
    if (open) {
        const auto snap = session_.snapshot();
        running_.refresh(snap);
        steps_.refresh(snap);
        grabKeyboardFocus();
    }
    refreshStatus();
}

void MainComponent::go() {
    if (!session_.isOpen()) return;
    auto& controller = session_.controller();
    const auto& stepsList = controller.show().steps;
    const int index = controller.nextIndex();
    const Step* step = index < static_cast<int>(stepsList.size()) ? &stepsList[static_cast<size_t>(index)] : nullptr;
    const StepId id = step != nullptr ? step->id : 0;
    const bool hasSound = step != nullptr && step->sound.has_value();
    const juce::String label = step != nullptr ? juce::String(index + 1) + " \"" +
                                                     juce::String::fromUTF8(step->name.c_str()) + "\""
                                               : juce::String();

    // Read before GO: the cue belongs to the step that runs now, not to the next one.
    const auto cue = hasSound ? juce::String::fromUTF8(step->sound->ontimeCue.c_str()) : juce::String();
    switch (controller.go(uiSeconds())) {
        case GoResult::Ran:
            if (hasSound) running_.restarted(id);
            session_.ontime().startCue(cue);  // queued; nothing waits for Ontime
            session_.stateChanged();
            break;
        case GoResult::IgnoredGuard: break;  // a double tap: the first one ran
        case GoResult::MissingFile: {
            session_.flash("GO refused: step " + label + " has no playable sound file");
            // A stuck NEXT would stall the show: move it on, so the next GO plays.
            const auto name = "'" + juce::String::fromUTF8(step->name.c_str()) + "'";
            const auto playable = [this](const Step& s) {
                return !s.sound || (!session_.importing(s.sound->file) && session_.sources().fileUsable(s.sound->file) &&
                                     !session_.fileProblem(s.sound->file));
            };
            int to = index + 1;
            while (to < static_cast<int>(stepsList.size()) && !playable(stepsList[static_cast<size_t>(to)])) ++to;
            if (to < static_cast<int>(stepsList.size())) {
                controller.selectNext(to);
                session_.stateChanged();
                session_.alert(AlertKind::MissingFile,
                               "Step " + juce::String(index + 1) + " " + name +
                                   juce::String::fromUTF8(" has no playable file \xe2\x80\x93 NEXT moved to step ") +
                                   juce::String(to + 1));
            } else {
                session_.alert(AlertKind::MissingFile, "No playable step after " + name);
            }
            break;
        }
        case GoResult::EndOfList: session_.flash("End of the list: select a step to go on"); break;
        case GoResult::EngineBusy: session_.flash("Engine busy, press again"); break;
        case GoResult::Paused: session_.flash(juce::String::fromUTF8("Paused \xe2\x80\x93 press Resume")); break;
    }
    refreshStatus();
}

void MainComponent::fadeOut() {
    if (!session_.isOpen()) return;
    if (!session_.controller().fadeOutAll()) session_.flash("Engine busy, press again");
    else if (session_.controller().show().settings.ontimeFollowTransport) session_.ontime().stop();
    refreshStatus();
}

void MainComponent::togglePause() {
    // Show mode has no Pause unless the show allows it: ignore silently (a stray P must not even flash a notice).
    if (!session_.isOpen() || !pauseAllowed(session_.mode(), session_.controller().show().settings.pauseInShow)) return;
    auto& controller = session_.controller();
    // The controller's own flag decides the direction, so a double tap cannot send two pauses.
    const bool resuming = controller.paused();
    if (!(resuming ? controller.resume() : controller.pause())) session_.flash("Engine busy, press again");
    else if (controller.show().settings.ontimeFollowTransport) resuming ? session_.ontime().resume() : session_.ontime().pause();
    refreshStatus();
}

void MainComponent::select(int index) {
    if (!session_.isOpen()) return;
    auto& controller = session_.controller();
    const int before = controller.nextIndex();
    controller.selectNext(index);
    if (controller.nextIndex() == before) return;
    stateDirty_ = true;
    flushState(false);  // at once unless one was written within kStateSaveMs; the timer catches up
}

bool MainComponent::keyPressed(const juce::KeyPress& key) {
    if (!session_.isOpen() || choosing_) return false;
    if (key == juce::KeyPress::spaceKey) {
        if (!spaceHeld_) go();
        spaceHeld_ = true;
    } else if (key == juce::KeyPress::escapeKey) {
        if (!escHeld_) fadeOut();
        escHeld_ = true;
    } else if ((key.getTextCharacter() == 'p' || key.getTextCharacter() == 'P') && !key.getModifiers().isCtrlDown() &&
               !key.getModifiers().isAltDown() && !key.getModifiers().isCommandDown()) {  // Ctrl+P etc. are not ours
        if (!pHeld_) togglePause();
        pHeld_ = true;
    } else if (key == juce::KeyPress::upKey) {
        arrow(-1);
    } else if (key == juce::KeyPress::downKey) {
        arrow(+1);
    } else if (key == juce::KeyPress::deleteKey) {
        deleteAt(session_.controller().nextIndex());
    } else {
        return false;
    }
    return true;
}

bool MainComponent::keyStateChanged(bool) {
    releaseKeys();
    return false;
}

void MainComponent::arrow(int delta) {
    const auto now = uiMs();
    if (arrowHeld_ && (now - arrowFirstMs_ < kRepeatDelayMs || now - arrowLastMs_ < kRepeatIntervalMs)) return;
    if (!arrowHeld_) arrowFirstMs_ = now;
    arrowHeld_ = true;
    arrowLastMs_ = now;
    select(session_.controller().nextIndex() + delta);
}

void MainComponent::deleteAt(int index) {
    const auto& steps = session_.controller().show().steps;
    if (index < 0 || index >= static_cast<int>(steps.size())) return;
    const StepId id = steps[static_cast<size_t>(index)].id;
    const auto snap = session_.snapshot();
    const bool playing = std::any_of(snap.voices, snap.voices + snap.numVoices, [id](const VoiceState& v) { return v.key == id; });
    if (playing && allowed(session_.mode(), Action::Delete)) {
        // Deleting what is heard is easy to do by accident; the sound itself is never cut.
        confirmAsync(this, "Step is playing. Delete it? The sound plays on.", "Delete", "Cancel", [this, id] {
            const auto& now = session_.controller().show().steps;
            const auto it = std::find_if(now.begin(), now.end(), [id](const Step& s) { return s.id == id; });
            if (it != now.end()) session_.deleteStep(static_cast<int>(it - now.begin()));
            refreshStatus();
        });
        return;
    }
    session_.deleteStep(index);  // refused in Show mode
    refreshStatus();
}

void MainComponent::releaseKeys() {
    if (arrowHeld_ && !juce::KeyPress::isKeyCurrentlyDown(juce::KeyPress::upKey) &&
        !juce::KeyPress::isKeyCurrentlyDown(juce::KeyPress::downKey))
        arrowHeld_ = false;
    if (spaceHeld_ && !juce::KeyPress::isKeyCurrentlyDown(juce::KeyPress::spaceKey)) spaceHeld_ = false;
    if (escHeld_ && !juce::KeyPress::isKeyCurrentlyDown(juce::KeyPress::escapeKey)) escHeld_ = false;
    if (pHeld_ && !juce::KeyPress::isKeyCurrentlyDown('P')) pHeld_ = false;
}

void MainComponent::toggleSettings() {
    if (settingsWindow_ != nullptr) {
        settingsWindow_->toFront(true);
        return;
    }
    settingsWindow_ = std::make_unique<SettingsWindow>(devices_, session_, [this] {
        // Deleted later (the window is still inside its own close callback),
        // but moved aside now so a quick reopen gets a new window that the
        // pending delete cannot touch.
        closingWindow_ = std::move(settingsWindow_);
        juce::MessageManager::callAsync([safe = juce::Component::SafePointer<MainComponent>(this)] {
            if (safe != nullptr) safe->closingWindow_.reset();
        });
    });
}

void MainComponent::timerCallback() {
    // A release can come while focus was elsewhere; polling here means a held
    // flag never sticks and blocks Space or Esc for good.
    releaseKeys();
    // Pending selection: saved every kStateSaveMs while Up/Down are held, and
    // at once after they are released.
    if (stateDirty_ && !juce::KeyPress::isKeyCurrentlyDown(juce::KeyPress::upKey) &&
        !juce::KeyPress::isKeyCurrentlyDown(juce::KeyPress::downKey))
        flushState(true);
    else
        flushState(false);
    status_.setPeaks(session_.outputPeaks(), uiMs());  // the meter moves at the UI rate, show open or not
    if (session_.isOpen()) {
        const auto snap = session_.snapshot();
        running_.refresh(snap);
        steps_.refresh(snap);
        // The display follows what the engine reports (not only the controller flag the GO refusal uses).
        pauseButton_.setButtonText(snap.paused ? "Resume (P)" : "Pause (P)");
        const bool banner = snap.paused && runningView_.isVisible();
        if (banner != pausedBanner_.isVisible()) {
            pausedBanner_.setVisible(banner);
            resized();  // the strip takes its height from the running area's top
        }
        editor_.refresh();
        settingsPanel_.refresh();
        padEditor_.refresh();
        pads_.refresh(snap);
        if (running_.rows() != shownRunningRows_) resized();  // the running area takes its height from its rows
        // Focus can end up nowhere (a hidden start panel had it); take it back
        // whenever our window is the active one, so the keys keep working.
        if (auto* peer = getPeer(); peer != nullptr && peer->isFocused() && !hasKeyboardFocus(true))
            grabKeyboardFocus();
    }
    refreshAlert();
    if (showCheck_.isVisible()) showCheck_.setLines(session_.checkReport());
    const auto now = uiMs();
    if (now - lastTickMs_ >= kTickMs) {
        lastTickMs_ = now;
        session_.tick();
        refreshStatus();
    }
}

void MainComponent::openShowCheck() {
    if (!session_.isOpen()) return;
    session_.checkFilesNow();
    showCheck_.setLines(session_.checkReport());
    showCheck_.setVisible(true);
    showCheck_.toFront(false);
    alertBanner_.toFront(false);
}

bool MainComponent::quitRequested() {
    if (!session_.isOpen() || session_.mode() != ShowMode::Show) return true;
    confirmAsync(this, "Quit Bigtop Player during the show?", "Quit", "Cancel", [this] { onQuitConfirmed_(); });
    return false;
}

void MainComponent::refreshKeepAwake() {
    const bool want = session_.isOpen() && session_.mode() != ShowMode::Edit;
    if (awake_ == want) return;
    awake_ = want;
    keepAwake_(want);
}

void MainComponent::refreshAlert() {
    const auto a = session_.isOpen() ? session_.currentAlert() : std::nullopt;
    if (a) {
        const auto hint = a->more > 0 ? "+" + juce::String(a->more) + juce::String::fromUTF8(" more \xe2\x80\x93 Show check") : juce::String();
        alertBanner_.show(a->text, hint, a->kind == AlertKind::Ontime || a->kind == AlertKind::Notice);
    }
    if (alertBanner_.isVisible() != a.has_value()) alertBanner_.setVisible(a.has_value());
}

void MainComponent::refreshStatus() {
    refreshKeepAwake();
    if (session_.mode() != shownMode_) {
        shownMode_ = session_.mode();
        resized();  // Add fade step appears and disappears with the mode
    }
    // The mode table decides what a panel may show: leaving Edit/Rehearsal closes the editor
    // (Show mode never has it), leaving Edit closes the show settings.
    if (editor_.isVisible() && !allowed(session_.mode(), Action::EditValue)) closeSide();
    if (settingsPanel_.isVisible() && !allowed(session_.mode(), Action::ShowSettings)) closeSide();
    if (padEditor_.isVisible() && !allowed(session_.mode(), Action::EditValue)) closeSide();
    modeBar_.setMode(session_.mode());
    if (modeBar_.isVisible() != session_.isOpen()) {  // the strip takes its height only with a show open
        modeBar_.setVisible(session_.isOpen());
        resized();
    }
    status_.update(session_.status(), session_.isOpen(), session_.mode(), session_.sender().status(),
                   session_.isOpen() ? session_.ontime().state() : io::OntimeClient::State::Off, session_.ontime().lastError());
}

void MainComponent::resized() {
    auto r = getLocalBounds();
    status_.setBounds(r.removeFromBottom(StatusBar::kHeight));
    if (session_.isOpen()) modeBar_.setBounds(r.removeFromTop(ModeBar::kHeight));
    start_.setBounds(r);
    // The side area is always there while a show is open: the pad grid, or the panel that replaced it.
    const bool open = session_.isOpen() && !choosing_;
    pads_.setVisible(open && !editor_.isVisible() && !settingsPanel_.isVisible() && !padEditor_.isVisible());
    // PAUSED strip and the running area span the window; the side area and the step list share what is left.
    if (pausedBanner_.isVisible()) pausedBanner_.setBounds(r.removeFromTop(28));
    runningView_.setBounds(r.removeFromTop(runningAreaHeight(running_.rows(), RunningSounds::kRowHeight)));
    running_.setSize(runningView_.getMaximumVisibleWidth(),
                     running_.rows() == 0 ? kRunningEmptyHeight : running_.rows() * RunningSounds::kRowHeight);
    shownRunningRows_ = running_.rows();
    if (open) {
        const auto panel = r.removeFromRight(juce::jmin(kSideWidth, r.getWidth() / 2));
        editor_.setBounds(panel);
        settingsPanel_.setBounds(panel);
        padEditor_.setBounds(panel);
        pads_.setBounds(panel);
    }
    auto buttons = r.removeFromBottom(kButtonHeight).reduced(8);
    fadeButton_.setBounds(buttons.removeFromRight(buttons.getWidth() / 3).withTrimmedLeft(8));
    const bool showPause = session_.isOpen() && !choosing_ &&
                           pauseAllowed(session_.mode(), session_.controller().show().settings.pauseInShow);
    pauseButton_.setVisible(showPause);
    if (showPause) pauseButton_.setBounds(buttons.removeFromRight(buttons.getWidth() / 4).withTrimmedLeft(8));
    const bool showAddFade = session_.isOpen() && !choosing_ && allowed(session_.mode(), Action::EditStructure);
    addFadeButton_.setVisible(showAddFade);
    if (showAddFade) addFadeButton_.setBounds(buttons.removeFromRight(juce::jmax(110, buttons.getWidth() / 4)).withTrimmedLeft(8));
    goButton_.setBounds(buttons);
    // The same colour as the strip, so the mode is visible right at the button that matters.
    goFrame_.setVisible(goButton_.isVisible());
    goFrame_.setBounds(buttons.expanded(ModeFrame::kWidth));
    goFrame_.setColour(juce::Colour(modeColour(session_.mode())));
    steps_.setBounds(r);
    showCheck_.setBounds(r);
    alertBanner_.setBounds(r.withHeight(juce::jmin(AlertBanner::kHeight, r.getHeight())));
}

}  // namespace bigtop::app
