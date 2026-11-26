// PadEditor.cpp: the pad editor panel. Fixed fields (a pad has few), re-read
// from the model every tick except the one being typed in.
#include "PadEditor.h"

#include "bigtop/ShowEdits.h"

namespace bigtop::app {

namespace {
constexpr int kRow = NumberField::kHeight;  // every touch target is at least 48 px
constexpr int kGap = 6;
const juce::Colour kPanel(0xff23262b);

juce::String text(const std::string& utf8) { return juce::String::fromUTF8(utf8.c_str()); }
}  // namespace

PadEditor::PadEditor(ShowSession& session)
    : session_(session),
      level_(0.5, kEditLevelMinDb, kEditLevelMaxDb, NumberField::Format::Db),
      trim_(0.5, kEditTrimMinDb, kEditTrimMaxDb, NumberField::Format::Db),
      start_(0.5, 0.0, kEditMaxPositionSeconds, NumberField::Format::MinSec),
      end_(0.5, 0.0, kEditMaxPositionSeconds, NumberField::Format::MinSec) {
    title_.setFont(juce::FontOptions(20.0f, juce::Font::bold));
    title_.setMinimumHorizontalScale(0.6f);
    hint_.setColour(juce::Label::textColourId, juce::Colours::grey);
    hint_.setText("(the trim applies to every step and pad using this file)", juce::dontSendNotification);
    close_.setWantsKeyboardFocus(false);
    clear_.setWantsKeyboardFocus(false);
    close_.onClick = [this] {
        if (onClose) onClose();
    };
    clear_.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff7a2b2b));
    // The audio file stays in the show (like a deleted step's), only the pad forgets it.
    clear_.onClick = [this] {
        // An empty pad cannot be tapped off, so its sound fades first, exactly as a second tap would
        // (a silent pad's key is ignored by the engine). A fade that cannot be sent refuses the Clear:
        // the pad keeps its file, so a tap can still fade it. The mode is checked first so a Clear the
        // mode refuses never fades the pad.
        if (allowed(session_.mode(), Action::AssignPad) &&
            !session_.controller().fadeOutSound(kPadKeyBase + static_cast<VoiceKey>(index_), kPadStopSeconds)) {
            session_.flash("Engine busy, press Clear pad again");
            return;
        }
        edit(Action::AssignPad, [](Show&, Pad& p) {
            p = Pad{};
            return true;
        });
    };

    name_.setInputRestrictions(200);
    name_.onReturnKey = [this] {
        commitName();
        name_.giveAwayKeyboardFocus();
    };
    name_.onEscapeKey = [this] {
        name_.setText(text(pad() != nullptr ? pad()->name : std::string()), false);  // revert; focus loss then commits nothing
        name_.giveAwayKeyboardFocus();
    };
    name_.onFocusLost = [this] { commitName(); };

    // A playing pad hears a stored level or trim at once, ramped (levelPad ignores a silent pad).
    // Its result is ignored: a ramp dropped on a full queue only delays the change to the next start.
    level_.onCommit = [this](double v) {
        if (edit(Action::EditValue, [v](Show&, Pad& p) {
                p.levelDb = v;
                return true;
            }))
            session_.controller().levelPad(index_);
        refresh();
    };
    trim_.onCommit = [this](double v) {
        if (edit(Action::EditValue, [v](Show& show, Pad& p) {
            if (!p.file) return false;
            for (auto& f : show.files)
                if (f.id == *p.file) {
                    f.trimDb = v;  // lives in the file, shared by every use
                    return true;
                }
            return false;
        }))
            session_.controller().levelPad(index_);
        refresh();
    };

    // The pad's range, like a step's start and end: structure, so Edit mode only. A change
    // is heard from the next tap; a pad that plays keeps its range.
    start_.onCommit = [this](double v) {
        const char* refused = nullptr;
        edit(Action::EditStructure, [&](Show&, Pad& p) {
            if (p.endSeconds && v >= *p.endSeconds) {
                refused = "Start must be before End";
                return false;
            }
            p.startSeconds = v;
            return true;
        });
        if (refused != nullptr) session_.flash(refused);
        refresh();
    };
    end_.setAllowEmpty(true);  // empty = play to the end of the file
    end_.onCommit = [this](double v) {
        const char* refused = nullptr;
        edit(Action::EditStructure, [&](Show&, Pad& p) {
            if (v <= p.startSeconds) {
                refused = "End must be after Start";
                return false;
            }
            p.endSeconds = v;
            return true;
        });
        if (refused != nullptr) session_.flash(refused);
        refresh();
    };
    end_.onClear = [this] {
        edit(Action::EditStructure, [](Show&, Pad& p) {
            if (!p.endSeconds) return false;
            p.endSeconds.reset();
            return true;
        });
        refresh();
    };

    for (juce::Component* c : {static_cast<juce::Component*>(&startLabel_), static_cast<juce::Component*>(&endLabel_),
                               static_cast<juce::Component*>(&start_), static_cast<juce::Component*>(&end_),
                               static_cast<juce::Component*>(&title_), static_cast<juce::Component*>(&fileLabel_),
                               static_cast<juce::Component*>(&nameLabel_), static_cast<juce::Component*>(&levelLabel_),
                               static_cast<juce::Component*>(&trimLabel_), static_cast<juce::Component*>(&hint_),
                               static_cast<juce::Component*>(&close_), static_cast<juce::Component*>(&clear_),
                               static_cast<juce::Component*>(&name_), static_cast<juce::Component*>(&level_),
                               static_cast<juce::Component*>(&trim_)})
        addAndMakeVisible(c);
    setVisible(false);
}

PadEditor::~PadEditor() = default;

const Pad* PadEditor::pad() const {
    if (!session_.isOpen()) return nullptr;
    const auto& show = session_.controller().show();
    // A pad the grid hides is not editable; a short list (fromJson) means an empty pad, shown as such.
    if (index_ < 0 || index_ >= show.settings.padRows * show.settings.padCols) return nullptr;
    static const Pad kEmptyPad{};
    return static_cast<size_t>(index_) < show.pads.size() ? &show.pads[static_cast<size_t>(index_)] : &kEmptyPad;
}

bool PadEditor::edit(Action action, const std::function<bool(Show&, Pad&)>& change) {
    const int index = index_;
    return session_.edit(action, [&](Show& show) {
        if (index < 0 || index >= show.settings.padRows * show.settings.padCols) return false;
        if (show.pads.size() <= static_cast<size_t>(index)) show.pads.resize(static_cast<size_t>(index) + 1);
        return change(show, show.pads[static_cast<size_t>(index)]);
    });
}

void PadEditor::commitName() {
    const Pad* p = pad();
    const auto typed = name_.getText().toStdString();
    if (p == nullptr || p->name == typed) return;
    const bool done = edit(Action::AssignPad, [&](Show&, Pad& target) {
        target.name = typed;
        return true;
    });
    // Refused (mode): the box goes back to the stored name at once, not only on a later refresh.
    if (!done)
        if (const Pad* now = pad()) name_.setText(text(now->name), false);
}

void PadEditor::open(int index) {
    if (!session_.isOpen()) return;
    const auto& s = session_.controller().show().settings;
    if (index < 0 || index >= s.padRows * s.padCols) return;
    index_ = index;
    setVisible(true);
    name_.setText(text(pad()->name), false);
    refresh();
}

void PadEditor::paint(juce::Graphics& g) {
    g.fillAll(kPanel);
    g.setColour(juce::Colours::black);
    g.fillRect(0, 0, 2, getHeight());
}

void PadEditor::resized() {
    auto r = getLocalBounds().reduced(8, 6);
    auto top = r.removeFromTop(kRow);
    close_.setBounds(top.removeFromRight(100));
    title_.setBounds(top);
    r.removeFromTop(kGap);
    const auto row = [&r] {
        auto line = r.removeFromTop(kRow);
        r.removeFromTop(kGap);
        return line;
    };
    constexpr int kLabel = 90;
    auto line = row();
    nameLabel_.setBounds(line.removeFromLeft(kLabel));
    name_.setBounds(line);
    fileLabel_.setBounds(row());
    line = row();
    levelLabel_.setBounds(line.removeFromLeft(kLabel));
    level_.setBounds(line.removeFromLeft(220));
    line = row();
    trimLabel_.setBounds(line.removeFromLeft(kLabel));
    trim_.setBounds(line.removeFromLeft(220));
    line = row();
    startLabel_.setBounds(line.removeFromLeft(kLabel));
    start_.setBounds(line.removeFromLeft(220));
    line = row();
    endLabel_.setBounds(line.removeFromLeft(kLabel));
    end_.setBounds(line.removeFromLeft(220));
    hint_.setBounds(row());
    clear_.setBounds(row().removeFromRight(160));
}

void PadEditor::refresh() {
    if (!isVisible()) return;
    const Pad* p = pad();
    if (p == nullptr) {  // grid shrunk, or the show closed
        if (onClose) onClose();
        return;
    }
    const auto& show = session_.controller().show();
    title_.setText("Pad " + juce::String(index_ + 1), juce::dontSendNotification);

    juce::String file = "No sound: drop an audio file on the pad (Edit mode)";
    double trimDb = 0.0;
    if (p->file) {
        file = "(file missing)";
        for (const auto& f : show.files)
            if (f.id == *p->file) {
                file = "Sound:  " + juce::File(text(f.path)).getFileName();
                trimDb = f.trimDb;
            }
    }
    fileLabel_.setText(file, juce::dontSendNotification);

    const auto mode = session_.mode();
    const bool structure = allowed(mode, Action::AssignPad);
    const bool values = allowed(mode, Action::EditValue);
    name_.setEnabled(structure);
    clear_.setEnabled(structure && p->file.has_value());
    level_.setEditable(values);
    trim_.setEditable(values && p->file.has_value());
    if (!name_.hasKeyboardFocus(false)) name_.setText(text(p->name), false);
    if (!level_.isEditing()) level_.setValue(p->levelDb);
    if (!trim_.isEditing()) trim_.setValue(trimDb);
    const bool range = allowed(mode, Action::EditStructure) && p->file.has_value();
    start_.setEditable(range);
    end_.setEditable(range);
    if (!start_.isEditing()) start_.setValue(p->startSeconds);
    if (!end_.isEditing()) {
        if (p->endSeconds) end_.setValue(*p->endSeconds);
        else end_.setEmpty();
    }
}

}  // namespace bigtop::app
