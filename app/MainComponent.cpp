// MainComponent.cpp: screen switching, the operator controls and the timer
// that drives the GO screen and the session.
#include "MainComponent.h"

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
    for (juce::Component* c : {static_cast<juce::Component*>(&runningView_), static_cast<juce::Component*>(&steps_),
                               static_cast<juce::Component*>(&goButton_), static_cast<juce::Component*>(&fadeButton_)})
        addChildComponent(c);
    addChildComponent(pausedBanner_);   // after the running view so it draws over it; shown by timerCallback() while paused
    addChildComponent(modeBar_);
    addAndMakeVisible(status_);

    modeBar_.onPick = [this](ShowMode mode) { pickMode(mode); };
    modeBar_.setDetails(steps_.details());  // remembered per laptop (AppSettings)
    modeBar_.onDetails = [this](bool on) { steps_.setDetails(on); };
    status_.onSettings = [this] { toggleSettings(); };
    status_.onShows = [this] { chooseShow(); };
    status_.onShowSettings = [this] { openShowSettings(); };

    if (const auto last = settings.lastShow()) {
        std::string error;
        std::vector<juce::File> backups;
        if (!session_.open(*last, error, &backups)) start_.showFailure(*last, error, std::move(backups));
    }
    showChanged();
    startTimerHz(kUiHz);
    setSize(1024, 700);
}

MainComponent::~MainComponent() {
    stopTimer();
    flushState(true);  // a held Up/Down may not have been saved yet
    settingsWindow_.reset();  // it reports its final choice to the session
    closingWindow_.reset();
}

void MainComponent::pickMode(ShowMode mode) {
    flushState(true);
    if (!session_.setMode(mode)) {  // refused ("Resume first"): the strip keeps the old mode
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
        refreshStatus();
        return;
    }
    choosing_ = true;
    showChanged();
}

void MainComponent::showChanged() {
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

    switch (controller.go(uiSeconds())) {
        case GoResult::Ran:
            if (hasSound) running_.restarted(id);
            session_.stateChanged();
            break;
        case GoResult::IgnoredGuard: break;  // a double tap: the first one ran
        case GoResult::MissingFile:
            session_.flash("GO refused: step " + label + " has no playable sound file");
            break;
        case GoResult::EndOfList: session_.flash("End of the list: select a step to go on"); break;
        case GoResult::EngineBusy: session_.flash("Engine busy, press again"); break;
        case GoResult::Paused: session_.flash(juce::String::fromUTF8("Paused \xe2\x80\x93 press Resume")); break;
    }
    refreshStatus();
}

void MainComponent::fadeOut() {
    if (!session_.isOpen()) return;
    if (!session_.controller().fadeOutAll()) session_.flash("Engine busy, press again");
    refreshStatus();
}

void MainComponent::togglePause() {
    // Show mode has no Pause: ignore silently (a stray P must not even flash a notice).
    if (!session_.isOpen() || !allowed(session_.mode(), Action::Pause)) return;
    auto& controller = session_.controller();
    // The controller's own flag decides the direction, so a double tap cannot send two pauses.
    if (!(controller.paused() ? controller.resume() : controller.pause())) session_.flash("Engine busy, press again");
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
        select(session_.controller().nextIndex() - 1);
    } else if (key == juce::KeyPress::downKey) {
        select(session_.controller().nextIndex() + 1);
    } else if (key == juce::KeyPress::deleteKey) {
        session_.deleteStep(session_.controller().nextIndex());  // refused in Show mode
        refreshStatus();
    } else {
        return false;
    }
    return true;
}

bool MainComponent::keyStateChanged(bool) {
    releaseKeys();
    return false;
}

void MainComponent::releaseKeys() {
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
    const auto now = uiMs();
    if (now - lastTickMs_ >= kTickMs) {
        lastTickMs_ = now;
        session_.tick();
        refreshStatus();
    }
}

void MainComponent::paintOverChildren(juce::Graphics& g) {
    if (!goButton_.isVisible()) return;
    // The same colour as the strip, so the mode is visible right at the button that matters.
    g.setColour(juce::Colour(modeColour(session_.mode())));
    g.drawRoundedRectangle(goButton_.getBounds().toFloat().expanded(2.0f), 6.0f, 4.0f);
}

void MainComponent::refreshStatus() {
    if (session_.mode() != shownMode_) {
        shownMode_ = session_.mode();
        repaint(goButton_.getBounds().expanded(8));
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
    status_.update(session_.status(), session_.isOpen(), session_.mode(), session_.sender().status());
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
    const bool showPause = session_.isOpen() && !choosing_ && allowed(session_.mode(), Action::Pause);
    pauseButton_.setVisible(showPause);
    if (showPause) pauseButton_.setBounds(buttons.removeFromRight(buttons.getWidth() / 4).withTrimmedLeft(8));
    const bool showAddFade = session_.isOpen() && !choosing_ && allowed(session_.mode(), Action::EditStructure);
    addFadeButton_.setVisible(showAddFade);
    if (showAddFade) addFadeButton_.setBounds(buttons.removeFromRight(juce::jmax(110, buttons.getWidth() / 4)).withTrimmedLeft(8));
    goButton_.setBounds(buttons);
    steps_.setBounds(r);
}

}  // namespace bigtop::app
