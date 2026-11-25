// StepList.cpp: painting the rows from the show, the next index and the snapshot,
// and the list's own row drag (handle, insertion line, edge auto-scroll). The rows
// are one painted component in a Viewport, laid out from rowTops().
#include "StepList.h"

#include <algorithm>
#include <array>

#include "Pointer.h"

namespace bigtop::app {

namespace {

// The rules, checked at compile time.
static_assert(stepRowState(0, 0, false, false, false) == StepRowState::MissingFile);
static_assert(stepRowState(2, 2, true, false, true) == StepRowState::Next);
static_assert(stepRowState(1, 2, true, true, true) == StepRowState::Starting);
static_assert(stepRowState(3, 2, true, false, true) == StepRowState::Playing);
static_assert(stepRowState(1, 2, false, false, true) == StepRowState::Done);
static_assert(stepRowState(3, 2, false, false, true) == StepRowState::None);
static_assert(stepRowState(2, 2, false, false, false, true) == StepRowState::Importing);
static_assert(movedIndex(1, 4) == 3);  // down: the gap counts the row itself
static_assert(movedIndex(3, 0) == 0);
static_assert(movedIndex(2, 2) == 2 && movedIndex(2, 3) == 2);  // its own gaps: no move

static_assert(stepRowHeight(0) == 48 && stepRowHeight(0) == StepList::kRowHeight);  // Details off: rows as before
static_assert(stepRowHeight(3) == 108);

// Row offsets for line counts, as the list computes them.
template <std::size_t N>
constexpr std::array<int, N + 1> tops(const std::array<int, N>& lines) {
    std::array<int, N + 1> t{};
    rowTops(lines, t);
    return t;
}
constexpr auto kFlat = tops(std::array<int, 10>{});  // ten rows without Details: 48 px each
static_assert(kFlat[2] == 96 && kFlat[10] == 480);
// Row 2 spans 96..143. Top half -> gap 2, bottom half -> gap 3 (the old index * 48 rule).
static_assert(insertionGap(96, kFlat, 0) == 2 && insertionGap(119, kFlat, 0) == 2);
static_assert(insertionGap(120, kFlat, 0) == 3 && insertionGap(143, kFlat, 0) == 3);
static_assert(insertionGap(5000, kFlat, 0) == 10);  // below the last row: after it
static_assert(insertionGap(-30, kFlat, 0) == 0);    // above the first
static_assert(insertionGap(10, kFlat, 96) == 2);    // scrolled two rows: y 10 is row 2's top half
static_assert(insertionGap(30, kFlat, 96) == 3);    // ...and 30 is its bottom half
static_assert(insertionGap(0, std::span<const int>{}, 0) == 0);  // no rows at all
static_assert(insertionGap(0, tops(std::array<int, 0>{}), 0) == 0);  // empty list
// Variable heights: 48, 88 (two lines), 48 -> tops 0, 48, 136, 184.
constexpr auto kMixed = tops(std::array<int, 3>{0, 2, 0});
static_assert(kMixed[1] == 48 && kMixed[2] == 136 && kMixed[3] == 184);
static_assert(insertionGap(91, kMixed, 0) == 1 && insertionGap(92, kMixed, 0) == 2);  // row 1's middle is 92
static_assert(insertionGap(159, kMixed, 0) == 2 && insertionGap(160, kMixed, 0) == 3);
static_assert(rowAt(47, kMixed) == 0 && rowAt(48, kMixed) == 1 && rowAt(135, kMixed) == 1 && rowAt(136, kMixed) == 2);
static_assert(rowAt(-1, kMixed) == -1 && rowAt(184, kMixed) == -1);
// Keep NEXT visible: in view stays, above scrolls up to its top, below puts its bottom at the edge.
static_assert(scrollToShow(48, 136, 0, 200) == 0);
static_assert(scrollToShow(48, 136, 100, 200) == 48);
static_assert(scrollToShow(400, 488, 0, 200) == 288);
static_assert(scrollToShow(400, 700, 0, 200) == 400);  // taller than the view: its top shows
// Edit: the button column of the name line only; a tall row's Details lines below it select.
static_assert(StepList::editHit(500, 10, 600) && StepList::editHit(492, 47, 600));
static_assert(!StepList::editHit(491, 10, 600));  // left of the column
static_assert(!StepList::editHit(500, 48, 600) && !StepList::editHit(500, 80, 600));  // under the name line

const juce::Colour kRed(0xffff5a5a);  // a light red stays readable on the NEXT highlight
constexpr int kScrollTimerMs = 33;    // edge auto-scroll while a drag runs

}  // namespace

/// @cond INTERNAL
/// The scrolled content: paints every row; StepList listens to its mouse events.
class StepList::Rows : public juce::Component, public juce::TooltipClient {
public:
    explicit Rows(StepList& owner) : owner_(owner) {
        setWantsKeyboardFocus(false);
        // Not a keyboard container (the list never takes keyboard focus); this only makes JUCE's
        // accessibility tree list the row items as this component's children.
        setFocusContainerType(FocusContainerType::focusContainer);
    }
    void paint(juce::Graphics& g) override { owner_.paintRows(g); }
    // A failed import's reason, for the row under the pointer.
    juce::String getTooltip() override { return owner_.tooltipAt(getMouseXYRelative().y); }
    // A list, as JUCE's ListBox; its items are the RowItem children.
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override {
        return std::make_unique<juce::AccessibilityHandler>(*this, juce::AccessibilityRole::list);
    }

private:
    StepList& owner_;
};

/// One row for screen readers: paints nothing and lets every click through to the rows (taps,
/// drags and tooltips stay the rows' own). Its handler reads rowTitle() and its press is a tap.
class StepList::RowItem : public juce::Component {
public:
    RowItem(StepList& owner, int row) : owner_(owner), row_(row) {
        setInterceptsMouseClicks(false, false);
        setWantsKeyboardFocus(false);
    }

    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override {
        return std::make_unique<Handler>(*this);
    }

    // The words last given to a screen reader; empty while none asked, so the per-tick check
    // in notifyTitles() costs nothing without one.
    juce::String spoken;

private:
    class Handler : public juce::AccessibilityHandler {
    public:
        explicit Handler(RowItem& item)
            : AccessibilityHandler(item, juce::AccessibilityRole::listItem,
                                   juce::AccessibilityActions().addAction(juce::AccessibilityActionType::press,
                                                                          [&item] { item.select(); })),
              item_(item) {}
        juce::String getTitle() const override {
            item_.spoken = item_.owner_.rowTitle(item_.row_);
            return item_.spoken;
        }
        // Rows scrolled out of view are still read (as ListBox rows are).
        juce::AccessibleState getCurrentState() const override {
            return AccessibilityHandler::getCurrentState().withAccessibleOffscreen();
        }

    private:
        RowItem& item_;
    };

    // The tap's path (StepList::tapped's select branch): MainComponent decides and refuses.
    void select() {
        if (owner_.session_.isOpen() && row_ < owner_.numRows() && owner_.onSelect) owner_.onSelect(row_);
    }

    StepList& owner_;
    const int row_;
};
/// @endcond

StepList::StepList(ShowSession& session, io::AppSettings& settings)
    : session_(session), settings_(settings), rows_(std::make_unique<Rows>(*this)) {
    detailsOn_ = settings_.stepDetails();
    view_.setViewedComponent(rows_.get(), false);
    view_.setScrollBarsShown(true, false);
    view_.setWantsKeyboardFocus(false);  // a Viewport eats arrow keys
    addAndMakeVisible(view_);
    rows_->addMouseListener(this, false);  // for the handle drag and taps
}

StepList::~StepList() = default;

void StepList::setDetails(bool on) {
    detailsOn_ = on;
    settings_.setStepDetails(on);
    layoutRows();
    // The rows above NEXT changed height: bring it back into view.
    if (session_.isOpen()) showRow(session_.controller().nextIndex());
    rows_->repaint();
}

void StepList::refresh(const Snapshot& snap) {
    snap_ = snap;
    layoutRows();
    const int next = session_.controller().nextIndex();
    if (next != shownNext_) {
        shownNext_ = next;
        showRow(next);
    }
    notifyTitles();  // NEXT, a file's state or the Details words may have changed
    rows_->repaint();
}

void StepList::layoutRows() {
    static const std::vector<Step> kNoSteps;
    static const std::vector<AudioFile> kNoFiles;
    const bool open = session_.isOpen();
    const auto& steps = open ? session_.controller().show().steps : kNoSteps;
    const auto& files = open ? session_.controller().show().files : kNoFiles;
    const bool on = open && detailsOn_;
    bool relayout = tops_.size() != steps.size() + 1;
    // What really plays now, so the NEXT row tells what will really be faded: pending voices count (the engine's everything-else
    // rule fades them too), pads and voices already fading out do not. Sorted: the snapshot's order is not stable.
    runningNow_.clear();
    for (int i = 0; i < snap_.numVoices; ++i)
        if (const VoiceState& v = snap_.voices[i]; v.key < kPadKeyBase && !v.fadingOut) runningNow_.push_back(v.key);
    std::sort(runningNow_.begin(), runningNow_.end());
    const int next = open ? session_.controller().nextIndex() : -1;
    const auto describeRow = [&](int row) {
        const auto r = static_cast<size_t>(row);
        // Only the NEXT row uses the live set; every other row keeps the list walk.
        details_[r] = describeStep(session_.controller().show(), r, false, row == next ? &runningNow_ : nullptr);
        // A fading everything-else rule always keeps its line's room, worded or blank: a row's height
        // depends only on the show, so the live NEXT line coming and going never moves the rows under a finger.
        const Step& s = steps[r];
        const auto fixed = (s.sound ? 1 : 0) + std::count_if(s.changes.begin(), s.changes.end(), [](const Change& c) {
                               return c.kind != ChangeKind::Keep;
                           });
        if (s.others.kind == ChangeKind::FadeOut && std::ssize(details_[r]) == fixed) details_[r].push_back({});
        unknownFile_[r] = steps[r].sound && !details_[r].empty() && details_[r][0].missingFile;
    };
    const auto inList = [&](int row) { return row >= 0 && static_cast<size_t>(row) < steps.size(); };
    if (on && (!described_ || steps != describedSteps_ || files != describedFiles_)) {
        // The words depend on the steps (names, sounds, changes, and for the everything-else line
        // the steps before each one) and the files (names); the NEXT row also on what plays (below).
        details_.assign(steps.size(), {});
        unknownFile_.assign(steps.size(), false);
        for (size_t i = 0; i < steps.size(); ++i) describeRow(static_cast<int>(i));
        describedSteps_ = steps;
        describedFiles_ = files;
        describedRunning_ = runningNow_;
        describedNext_ = next;
        described_ = true;
        ++describes_;
        relayout = true;
    } else if (on && (next != describedNext_ || runningNow_ != describedRunning_)) {
        // Only the NEXT row's words depend on what plays: the old NEXT row goes back to the list walk.
        const int oldNext = describedNext_;
        describedNext_ = next;
        describedRunning_ = runningNow_;
        if (inList(oldNext) && oldNext != next) describeRow(oldNext);
        if (inList(next)) describeRow(next);
        ++describes_;
        relayout = true;
    } else if (!on && described_) {
        described_ = false;
        describedSteps_.clear();
        describedFiles_.clear();
        relayout = true;
    }
    if (relayout) {
        if (!on) details_.assign(steps.size(), {});
        std::vector<int> lines(steps.size(), 0);
        for (size_t i = 0; i < lines.size(); ++i) lines[i] = static_cast<int>(details_[i].size());
        tops_.resize(lines.size() + 1);
        rowTops(lines, tops_);
    }
    if (on) {
        // Whether a file can play changes without a show edit (imports, files gone from disk): per tick.
        for (size_t i = 0; i < steps.size(); ++i) {
            if (!steps[i].sound || details_[i].empty()) continue;  // a sound step has its sound line first
            // Red exactly when the row says "missing file" / "import failed" (not while importing).
            const bool missing =
                    !session_.importing(steps[i].sound->file) && !session_.sources().fileUsable(steps[i].sound->file);
            details_[i][0].missingFile = unknownFile_[i] || missing;
        }
    }
    rows_->setSize(contentWidth(), tops_.back());
    syncItems();
}

void StepList::syncItems() {
    const size_t n = tops_.size() - 1;
    const bool changed = items_.size() != n;
    while (items_.size() > n) items_.pop_back();  // ~Component takes it off the rows
    while (items_.size() < n) {
        items_.push_back(std::make_unique<RowItem>(*this, static_cast<int>(items_.size())));
        rows_->addAndMakeVisible(*items_.back());
    }
    for (size_t i = 0; i < n; ++i) items_[i]->setBounds(0, tops_[i], rows_->getWidth(), tops_[i + 1] - tops_[i]);
    if (changed)
        if (auto* h = rows_->getAccessibilityHandler()) h->notifyAccessibilityEvent(juce::AccessibilityEvent::structureChanged);
}

void StepList::notifyTitles() {
    for (size_t i = 0; i < items_.size(); ++i) {
        RowItem& item = *items_[i];
        if (item.spoken.isEmpty()) continue;  // never read: nothing to correct
        auto now = rowTitle(static_cast<int>(i));
        if (now == item.spoken) continue;
        item.spoken = std::move(now);
        if (auto* h = item.getAccessibilityHandler()) h->notifyAccessibilityEvent(juce::AccessibilityEvent::titleChanged);
    }
}

int StepList::contentWidth() const {
    // Always leave room for the scroll bar: with the visible width the rows would get narrower for a
    // tick when the bar appears, and the Edit buttons would sit under it (as in ShowSettingsPanel).
    return juce::jmax(0, view_.getWidth() - view_.getScrollBarThickness());
}

void StepList::showRow(int row) {
    if (row < 0 || row >= numRows() || static_cast<size_t>(row) + 1 >= tops_.size()) return;  // past the end: nothing to show
    const auto r = static_cast<size_t>(row);
    view_.setViewPosition(0, scrollToShow(tops_[r], tops_[r + 1], view_.getViewPositionY(), view_.getViewHeight()));
}

void StepList::resized() {
    view_.setBounds(getLocalBounds());
    layoutRows();
}

void StepList::paint(juce::Graphics& g) { g.fillAll(getLookAndFeel().findColour(juce::ListBox::backgroundColourId)); }

int StepList::numRows() const {
    return session_.isOpen() ? static_cast<int>(session_.controller().show().steps.size()) : 0;
}

bool StepList::handleShown() const { return allowed(session_.mode(), Action::Reorder); }

void StepList::paintRows(juce::Graphics& g) {
    const auto clip = g.getClipBounds();
    const int width = rows_->getWidth();
    // The offsets are from the last layout; a step added since then waits for the next refresh.
    const int rows = std::min(numRows(), static_cast<int>(tops_.size()) - 1);
    for (int row = 0; row < rows; ++row) {
        const auto i = static_cast<size_t>(row);
        if (tops_[i + 1] <= clip.getY() || tops_[i] >= clip.getBottom()) continue;  // off screen
        const juce::Graphics::ScopedSaveState state(g);
        g.setOrigin(0, tops_[i]);
        g.reduceClipRegion(0, 0, width, tops_[i + 1] - tops_[i]);
        paintRow(row, g, width, tops_[i + 1] - tops_[i]);
    }
}

void StepList::paintRow(int row, juce::Graphics& g, int width, int height) {
    const auto& steps = session_.controller().show().steps;
    const Step& step = steps[static_cast<size_t>(row)];
    const int next = session_.controller().nextIndex();

    const VoiceState* begin = snap_.voices;
    const VoiceState* end = begin + snap_.numVoices;
    const VoiceState* voice = std::find_if(begin, end, [&](const VoiceState& v) { return v.key == step.id; });
    const bool usable = !step.sound || session_.sources().fileUsable(step.sound->file);
    const bool importing = step.sound && session_.importing(step.sound->file);
    const bool failed = step.sound && session_.importError(step.sound->file).isNotEmpty();
    const auto state = stepRowState(row, next, voice != end, voice != end && voice->pending, usable, importing);

    auto& lf = getLookAndFeel();
    if (row == next) g.fillAll(lf.findColour(juce::TextEditor::highlightColourId));  // the scheme's accent, tinted
    if (row == dragFrom_ && dragActive_) g.fillAll(juce::Colours::white.withAlpha(0.12f));          // the row being carried
    const auto textColour = lf.findColour(juce::ListBox::textColourId);
    g.setColour(textColour);
    g.setFont(juce::FontOptions(20.0f));
    // The name line keeps the old 48 px layout; Details lines go below it.
    auto r = juce::Rectangle<int>(width, kRowHeight);
    if (handleShown()) {
        // Three bars, the usual "grab here" sign; the whole 48 px strip is the touch target.
        g.drawText(juce::String::fromUTF8("\xe2\x89\xa1"), r.removeFromLeft(kHandleWidth), juce::Justification::centred);
    }
    r.reduce(12, 0);
    if (allowed(session_.mode(), Action::EditValue)) {
        const auto button = r.removeFromRight(kEditWidth).reduced(0, 4);
        g.setColour(juce::Colour(0xff3a4a60));
        g.fillRoundedRectangle(button.toFloat(), 6.0f);
        g.setColour(juce::Colours::white);
        g.drawText("Edit", button, juce::Justification::centred);
        g.setColour(textColour);
        r.removeFromRight(12);
    }
    g.drawText(juce::String(row + 1), r.removeFromLeft(48), juce::Justification::centredLeft);
    const auto linesArea = r;  // under the name and the state, left of the Edit button
    const auto stateArea = r.removeFromRight(160);
    g.drawText(juce::String::fromUTF8(step.name.c_str()), r, juce::Justification::centredLeft, true);

    juce::String text;
    switch (state) {
        case StepRowState::None: break;
        case StepRowState::Next: text = "NEXT"; break;
        case StepRowState::Starting: text = juce::String::fromUTF8("starting\xe2\x80\xa6"); break;
        case StepRowState::Playing: text = "playing"; break;
        case StepRowState::Done: text = "done"; break;
        case StepRowState::Importing: text = juce::String::fromUTF8("importing\xe2\x80\xa6"); break;
        case StepRowState::MissingFile:
            text = failed ? "import failed" : "missing file";  // the reason is the row's tooltip
            g.setColour(kRed);
            break;
    }
    // A step without a sound is a fade step: say so where a file's state would go.
    if (!step.sound) text = text.isEmpty() ? "fade" : text + juce::String::fromUTF8(" \xc2\xb7 fade");
    g.drawText(text, stateArea, juce::Justification::centredRight);

    // Details: smaller grey lines, the first one starting 8 px above the name line's bottom.
    g.setFont(juce::FontOptions(15.0f));
    int y = kRowHeight - 8;
    for (const DetailLine& line : details_[static_cast<size_t>(row)]) {
        if (y >= height) break;
        g.setColour(line.missingFile ? kRed : textColour.withAlpha(0.6f));
        g.drawText(juce::String::fromUTF8(line.text.c_str()), linesArea.getX(), y, linesArea.getWidth(), kDetailLineHeight,
                   juce::Justification::centredLeft, true);
        y += kDetailLineHeight;
    }
}

void StepList::tapped(int row, int x, int contentY) {
    const auto& steps = session_.controller().show().steps;
    if (row < 0 || row >= static_cast<int>(steps.size())) return;
    if (handleShown() && x < kHandleWidth) return;  // the handle only drags
    const int rowY = contentY - tops_[static_cast<size_t>(row)];
    if (allowed(session_.mode(), Action::EditValue) && editHit(x, rowY, rows_->getWidth())) {
        if (onEdit) onEdit(steps[static_cast<size_t>(row)].id);  // never touches NEXT
    } else if (onSelect) {
        onSelect(row);
    }
}

juce::String StepList::tooltipAt(int contentY) const {
    const int row = rowAt(contentY, tops_);  // within the laid-out rows or -1
    if (row < 0 || row >= numRows()) return {};
    const Step& step = session_.controller().show().steps[static_cast<size_t>(row)];
    return step.sound ? session_.importError(step.sound->file) : juce::String();
}

juce::String StepList::rowTitle(int row) const {
    if (row < 0 || row >= numRows() || static_cast<size_t>(row) >= details_.size()) return {};
    const Step& step = session_.controller().show().steps[static_cast<size_t>(row)];
    juce::String title = juce::String(row + 1) + ", " + juce::String::fromUTF8(step.name.c_str());
    if (row == session_.controller().nextIndex()) title << ", next";
    // The words the row shows in red (stepRowState's missing file, not while importing).
    if (step.sound && !session_.importing(step.sound->file) && !session_.sources().fileUsable(step.sound->file))
        title << (session_.importError(step.sound->file).isNotEmpty() ? ", import failed" : ", missing file");
    for (const DetailLine& line : details_[static_cast<size_t>(row)])
        if (!line.text.empty()) title << ", " << juce::String::fromUTF8(line.text.c_str());  // a reserved blank line says nothing
    return title;
}

bool StepList::isInterestedInFileDrag(const juce::StringArray&) { return true; }

void StepList::filesDropped(const juce::StringArray& files, int, int y) {
    fileDragExit({});
    if (!session_.isOpen()) return;
    session_.importFiles(files, insertionGap(y - view_.getY(), tops_, view_.getViewPositionY()));
}

void StepList::fileDragEnter(const juce::StringArray&, int, int y) {
    fileDrag_ = true;
    dragTo(y - view_.getY());
}

void StepList::fileDragMove(const juce::StringArray&, int, int y) { dragTo(y - view_.getY()); }

void StepList::fileDragExit(const juce::StringArray&) {
    fileDrag_ = false;
    gap_ = -1;
    if (!dragActive_) stopTimer();
    repaint();
}

void StepList::dragTo(int y) {
    pointerY_ = y;
    if (fileDrag_ && !allowed(session_.mode(), Action::DropFiles)) {
        gap_ = -1;  // the drop would be refused: promise nothing
    } else {
        gap_ = insertionGap(y, tops_, view_.getViewPositionY());
        startTimer(kScrollTimerMs);  // the edge auto-scroll must run while the pointer rests there
    }
    repaint();
}

void StepList::timerCallback() {
    if (!dragActive_ && !fileDrag_) {
        stopTimer();
        return;
    }
    // Within one row of the top or bottom edge the list scrolls; closer to the edge is faster.
    const int h = view_.getHeight();
    int dy = 0;
    if (pointerY_ < kRowHeight)
        dy = -(2 + (kRowHeight - std::max(pointerY_, 0)) / 4);
    else if (pointerY_ > h - kRowHeight)
        dy = 2 + (std::min(pointerY_, h) - (h - kRowHeight)) / 4;
    if (dy == 0) return;
    view_.setViewPosition(view_.getViewPositionX(), view_.getViewPositionY() + dy);
    dragTo(pointerY_);  // the gap under the resting pointer moved with the content
}

void StepList::paintOverChildren(juce::Graphics& g) {
    if (gap_ < 0 || static_cast<size_t>(gap_) >= tops_.size()) return;
    g.reduceClipRegion(view_.getBounds());
    // 3 px bright accent line across the full width with small end caps, centred on the boundary between two rows.
    const int y = view_.getY() + tops_[static_cast<size_t>(gap_)] - view_.getViewPositionY();
    g.setColour(juce::Colour(0xff4a9eff));
    g.fillRect(0, y - 1, getWidth(), 3);
    g.fillRect(0, y - 6, 4, 13);
    g.fillRect(getWidth() - 4, y - 6, 4, 13);
}

void StepList::cancelDrag() {
    rows_->setViewportIgnoreDragFlag(false);
    dragFrom_ = -1;
    dragActive_ = false;
    gap_ = -1;
    if (!fileDrag_) stopTimer();
    repaint();
}

void StepList::mouseDown(const juce::MouseEvent& e) {
    if (e.eventComponent != rows_.get()) return;
    // The same finger pressing again while its drag is set: that drag's release was lost (touch
    // driver, capture taken away). Without this the old row stayed attached and the next release
    // dropped it.
    if (dragFrom_ >= 0 && pointerIndex(e) == pressSource_) cancelDrag();
    // A second finger while a press is held must not hijack it.
    if (dragFrom_ >= 0 || !session_.isOpen() || !handleShown()) return;
    if (e.x >= kHandleWidth) return;  // only presses on the handle strip of a row start a drag
    const int row = rowAt(e.y, tops_);
    if (row < 0) return;
    dragFrom_ = row;
    dragActive_ = false;  // armed only by real movement, so a held tap never scrolls or reorders
    pressSource_ = pointerIndex(e);
    // The Viewport turns a touch drag into scrolling unless the pressed component says not to.
    // It reads the flag on every drag event, so setting it now (and clearing it on release) is enough.
    rows_->setViewportIgnoreDragFlag(true);
}

void StepList::mouseDrag(const juce::MouseEvent& e) {
    if (dragFrom_ < 0 || pointerIndex(e) != pressSource_) return;
    if (!dragActive_) {
        if (!e.mouseWasDraggedSinceMouseDown()) return;
        dragActive_ = true;
    }
    dragTo(e.getEventRelativeTo(&view_).getPosition().y);
}

void StepList::mouseExit(const juce::MouseEvent& e) {
    // While a button is held JUCE keeps the drag on the rows; an exit with none held means the
    // release never reached us.
    // Only that finger: another one lifting and leaving says nothing about the held drag.
    if (e.eventComponent == rows_.get() && dragFrom_ >= 0 && pointerIndex(e) == pressSource_ &&
        !e.mods.isAnyMouseButtonDown())
        cancelDrag();
}

void StepList::mouseUp(const juce::MouseEvent& e) {
    if (e.eventComponent != rows_.get()) return;
    if (pointerIndex(e) != pressSource_ && dragFrom_ >= 0) return;  // another finger lifting
    if (dragFrom_ < 0) {
        // Not a handle press: a tap, unless the press moved (a scroll: dragging outside the handle never reorders).
        if (!e.mouseWasDraggedSinceMouseDown() && session_.isOpen()) tapped(rowAt(e.y, tops_), e.x, e.y);
        return;
    }
    const int from = dragFrom_;
    const int gap = gap_;
    const bool active = dragActive_;
    cancelDrag();
    if (!active || gap < 0 || from >= numRows()) return;  // never armed, or the list changed meanwhile
    // Released well outside the list: cancel rather than throw the row to the top or end.
    const auto p = e.getEventRelativeTo(&view_).getPosition();
    if (p.x < 0 || p.x > view_.getWidth() || p.y < -kRowHeight || p.y > view_.getHeight() + kRowHeight) return;
    // NEXT follows its step by id inside the session's commit; nothing here calls onSelect.
    session_.moveStep(from, movedIndex(from, gap));
}

}  // namespace bigtop::app
