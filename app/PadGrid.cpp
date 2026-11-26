// PadGrid.cpp: the pad buttons are painted by one inner component (no child per
// pad), so a re-layout after a settings change cannot leave stale buttons, and the
// hit test is the same constexpr rule that the static_asserts in the header pin.
#include "PadGrid.h"

#include <algorithm>

#include "RunningSounds.h"  // splitTime

namespace bigtop::app {

namespace {
constexpr int kHeaderHeight = 48;
const juce::Colour kIdle(0xff34506f);
const juce::Colour kEmpty(0xff262a30);
const juce::Colour kPlaying(0xff2e7d32);
const juce::Colour kProgress(0xff4caf50);
const juce::Colour kRed(0xffe57373);
const juce::Colour kEditRing(0xffffb300);

juce::String text(const std::string& utf8) { return juce::String::fromUTF8(utf8.c_str()); }
}  // namespace

/// @cond INTERNAL
/// The scrolled content: paints the buttons and turns a release on one into a tap.
class PadGrid::Cells : public juce::Component {
public:
    explicit Cells(PadGrid& owner) : owner_(owner) { setWantsKeyboardFocus(false); }
    void paint(juce::Graphics& g) override { owner_.paintCells(g); }
    void mouseDown(const juce::MouseEvent& e) override { downPad_ = owner_.padAtContent(e.x, e.y); }
    void mouseUp(const juce::MouseEvent& e) override {
        // Released on the pad it was pressed on and not a scroll drag: a tap.
        const int up = owner_.padAtContent(e.x, e.y);
        if (downPad_ >= 0 && up == downPad_ && !e.mouseWasDraggedSinceMouseDown()) owner_.tapped(up);
        downPad_ = -1;
    }

private:
    PadGrid& owner_;
    int downPad_ = -1;
};
/// @endcond

PadGrid::PadGrid(ShowSession& session) : session_(session), cells_(std::make_unique<Cells>(*this)) {
    title_.setFont(juce::FontOptions(20.0f, juce::Font::bold));
    editToggle_.setWantsKeyboardFocus(false);
    editToggle_.onClick = [this] { cells_->repaint(); };
    view_.setViewedComponent(cells_.get(), false);
    view_.setScrollBarsShown(true, true);
    view_.setWantsKeyboardFocus(false);  // a Viewport eats arrow keys
    for (juce::Component* c : {static_cast<juce::Component*>(&title_), static_cast<juce::Component*>(&editToggle_),
                               static_cast<juce::Component*>(&view_)})
        addAndMakeVisible(c);
}

PadGrid::~PadGrid() = default;

void PadGrid::resized() {
    auto r = getLocalBounds().reduced(8, 6);
    auto top = r.removeFromTop(kHeaderHeight);
    editToggle_.setBounds(top.removeFromRight(150));
    title_.setBounds(top);
    view_.setBounds(r.withTrimmedTop(kPadGap));
    layoutCells();
}

void PadGrid::layoutCells() {
    if (!session_.isOpen()) return;
    const auto& s = session_.controller().show().settings;
    rows_ = s.padRows;
    cols_ = s.padCols;
    // Width without the scrollbar's strip, so a scrollbar appearing never changes the cell width.
    const int availW = view_.getWidth() - view_.getScrollBarThickness();
    const int cw = padCellSize(availW, cols_);
    const int ch = padCellSize(view_.getHeight(), rows_);
    cells_->setSize(padContentSize(cw, cols_), padContentSize(ch, rows_));
    cells_->repaint();
}

int PadGrid::padAtContent(int x, int y) const {
    if (rows_ <= 0 || cols_ <= 0) return -1;
    const int availW = view_.getWidth() - view_.getScrollBarThickness();
    return padIndexAt(x, y, padCellSize(availW, cols_), padCellSize(view_.getHeight(), rows_), rows_, cols_);
}

int PadGrid::padAt(int x, int y) const {
    const auto pos = view_.getViewPosition();
    return padAtContent(x - view_.getX() + pos.x, y - view_.getY() + pos.y);
}

int PadGrid::padAtScreen(juce::Point<int> screenPos) const {
    if (!isVisible()) return -1;  // the editor panels replace the grid
    const auto p = getLocalPoint(nullptr, screenPos);
    return getLocalBounds().contains(p) ? padAt(p.x, p.y) : -1;
}

void PadGrid::setDropHighlight(int index) {
    if (index == dropHighlight_) return;
    dropHighlight_ = index;
    cells_->repaint();
}

void PadGrid::refresh(const Snapshot& snap) {
    if (!session_.isOpen()) return;
    snap_ = snap;
    const bool canEdit = allowed(session_.mode(), Action::EditValue);
    editToggle_.setVisible(canEdit);
    if (!canEdit && editToggle_.getToggleState()) editToggle_.setToggleState(false, juce::dontSendNotification);
    const auto& s = session_.controller().show().settings;
    if (s.padRows != rows_ || s.padCols != cols_) layoutCells();  // the grid follows Show settings
    cells_->repaint();  // cheap: at most 24 buttons
}

juce::String PadGrid::labelOf(int index) const {
    const auto& show = session_.controller().show();
    // The list can be shorter than the grid (fromJson): a missing pad is an empty button.
    if (static_cast<size_t>(index) >= show.pads.size()) return {};
    const Pad& pad = show.pads[static_cast<size_t>(index)];
    if (!pad.name.empty()) return text(pad.name);
    if (pad.file)
        for (const auto& f : show.files)
            if (f.id == *pad.file) return juce::File(text(f.path)).getFileNameWithoutExtension();
    return {};
}

void PadGrid::paintCells(juce::Graphics& g) {
    if (!session_.isOpen()) return;
    const auto& show = session_.controller().show();
    const int availW = view_.getWidth() - view_.getScrollBarThickness();
    const int cw = padCellSize(availW, cols_);
    const int ch = padCellSize(view_.getHeight(), rows_);
    for (int i = 0; i < rows_ * cols_; ++i) {
        const int x = kPadGap + (i % cols_) * (cw + kPadGap);
        const int y = kPadGap + (i / cols_) * (ch + kPadGap);
        const juce::Rectangle<int> cell(x, y, cw, ch);
        const Pad* pad = static_cast<size_t>(i) < show.pads.size() ? &show.pads[static_cast<size_t>(i)] : nullptr;
        const bool assigned = pad != nullptr && pad->file.has_value();

        const VoiceState* voice = nullptr;
        for (int v = 0; v < snap_.numVoices; ++v)
            if (snap_.voices[v].key == kPadKeyBase + static_cast<VoiceKey>(i)) voice = &snap_.voices[v];

        juce::String status;
        juce::Colour statusColour = juce::Colours::lightgrey;
        float progress = 0.0f;
        if (assigned) {
            const FileId file = *pad->file;
            if (session_.importing(file)) {
                status = juce::String::fromUTF8("importing\xe2\x80\xa6");
            } else if (!session_.sources().fileUsable(file)) {
                status = session_.importError(file).isNotEmpty() ? "import failed" : "missing file";
                statusColour = kRed;
            } else if (voice != nullptr) {
                const int64_t len = session_.sources().fileLengthFrames(file);
                const MinSec t = splitTime(len - voice->positionFrame);
                status = juce::String::fromUTF8("\xe2\x88\x92") + juce::String(t.minutes) + ":" +
                         juce::String(t.seconds).paddedLeft('0', 2);
                statusColour = juce::Colours::white;
                if (len > 0) progress = std::clamp(static_cast<float>(voice->positionFrame) / static_cast<float>(len), 0.0f, 1.0f);
            }
        }

        g.setColour(voice != nullptr ? kPlaying : assigned ? kIdle : kEmpty);
        g.fillRoundedRectangle(cell.toFloat(), 6.0f);
        if (progress > 0.0f) {
            g.setColour(kProgress);  // thin bar along the bottom: how far the sound is
            g.fillRect(cell.getX() + 4, cell.getBottom() - 8, static_cast<int>(static_cast<float>(cell.getWidth() - 8) * progress), 4);
        }
        if (editToggle_.getToggleState()) {
            g.setColour(kEditRing);
            g.drawRoundedRectangle(cell.toFloat().reduced(1.0f), 6.0f, 2.0f);
        }
        if (i == dropHighlight_) {  // a step dragged from the list would land here
            g.setColour(juce::Colours::white);
            g.drawRoundedRectangle(cell.toFloat().reduced(2.0f), 6.0f, 4.0f);
        }

        auto inner = cell.reduced(6, 4);
        g.setColour(assigned ? juce::Colours::white : juce::Colours::grey);
        g.setFont(juce::FontOptions(16.0f, juce::Font::bold));
        const auto label = labelOf(i);
        g.drawFittedText(label.isNotEmpty() ? label : juce::String("(empty)"), inner.removeFromTop(inner.getHeight() * 2 / 3),
                         juce::Justification::centred, 2, 0.7f);
        if (status.isNotEmpty()) {
            g.setColour(statusColour);
            g.setFont(juce::FontOptions(14.0f));
            g.drawFittedText(status, inner, juce::Justification::centred, 1, 0.7f);
        }
    }
}

void PadGrid::tapped(int index) {
    if (!session_.isOpen()) return;
    // The mode too, not only the toggle: refresh() turns the toggle off only on the next tick,
    // and a tap in Show mode in between must play, not be swallowed by the editor's refusal.
    if (editToggle_.getToggleState() && allowed(session_.mode(), Action::EditValue)) {
        if (onEditPad) onEditPad(index);
        return;
    }
    if (!session_.controller().triggerPad(index)) {
        const auto& pads = session_.controller().show().pads;
        const bool assigned = static_cast<size_t>(index) < pads.size() && pads[static_cast<size_t>(index)].file.has_value();
        session_.flash(assigned ? "Pad cannot play (file not ready or engine busy)"
                                : "Pad is empty: drop an audio file on it in Edit mode");
    }
}

bool PadGrid::isInterestedInFileDrag(const juce::StringArray&) { return true; }

void PadGrid::filesDropped(const juce::StringArray& files, int x, int y) {
    if (!session_.isOpen() || files.isEmpty()) return;
    const int index = padAt(x, y);
    if (index < 0) return;
    if (files.size() > 1) session_.flash("A pad holds one sound: only " + juce::File(files[0]).getFileName() + " was used");
    session_.importToPad(juce::File(files[0]), index);
}

}  // namespace bigtop::app
