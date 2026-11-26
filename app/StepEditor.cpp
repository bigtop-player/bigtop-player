// StepEditor.cpp: the step editor panel. The fields are rebuilt only when the
// layout changes (see signature()); values are pulled from the model into the
// fields that are not being typed, so a commit never destroys the field the
// operator's finger is on.
#include "StepEditor.h"

#include <algorithm>

#include "bigtop/ShowEdits.h"

namespace bigtop::app {

namespace {

constexpr int kRow = NumberField::kHeight;  // every touch target is at least 48 px
constexpr int kGap = 6;
constexpr double kMaxPositionSeconds = kEditMaxPositionSeconds;  // start / end inside a file
const juce::Colour kPanel(0xff23262b);
const juce::Colour kAccent(0xff3a6ea5);
const juce::Colour kHint = juce::Colours::grey;

template <class T>
T& adopt(juce::Component& parent, std::vector<std::unique_ptr<juce::Component>>& owned, std::unique_ptr<T> c) {
    T& ref = *c;
    parent.addAndMakeVisible(ref);
    owned.push_back(std::move(c));
    return ref;
}

const Change* findChange(const Step& s, StepId target) {
    const auto it = std::find_if(s.changes.begin(), s.changes.end(), [&](const Change& c) { return c.target == target; });
    return it != s.changes.end() ? &*it : nullptr;
}

// 0 default (no change stored), 1 keep, 2 to level, 3 fade out: the order of the four buttons.
int choiceOf(const Change* c) {
    if (c == nullptr) return 0;
    return c->kind == ChangeKind::Keep ? 1 : c->kind == ChangeKind::Level ? 2 : 3;
}

juce::String text(const std::string& utf8) { return juce::String::fromUTF8(utf8.c_str()); }

juce::String seconds(double v) { return juce::String(v, 1) + " s"; }

juce::String stepLabel(const Show& show, int index) {
    return juce::String(index + 1).paddedLeft('0', 2) + "  " + text(show.steps[static_cast<size_t>(index)].name);
}

int indexOfId(const Show& show, StepId id) {
    for (size_t i = 0; i < show.steps.size(); ++i)
        if (show.steps[i].id == id) return static_cast<int>(i);
    return -1;
}

Timecode toTimecode(long long frames, int fps) {
    const long long total = frames / fps;
    return {static_cast<int>(total / 3600), static_cast<int>(total / 60 % 60), static_cast<int>(total % 60),
            static_cast<int>(frames % fps)};
}

}  // namespace

/// @cond INTERNAL
struct StepEditor::Spec {
    NumberField::Format format;
    double step, lo, hi;
    Action action;
    bool optional;
    std::function<std::optional<double>(const Show&, const Step&)> get;
    // nullptr = done; otherwise the reason it was refused (the edit is dropped and flashed).
    std::function<const char*(Show&, Step&, std::optional<double>)> set;
};
/// @endcond

StepEditor::StepEditor(ShowSession& session) : session_(session) {
    title_.setFont(juce::FontOptions(20.0f, juce::Font::bold));
    title_.setMinimumHorizontalScale(0.6f);
    close_.setWantsKeyboardFocus(false);
    close_.onClick = [this] {
        if (onClose) onClose();
    };
    view_.setViewedComponent(&content_, false);
    view_.setScrollBarsShown(true, false);
    view_.setWantsKeyboardFocus(false);  // a Viewport eats arrow keys
    for (juce::Component* c : {static_cast<juce::Component*>(&title_), static_cast<juce::Component*>(&close_),
                               static_cast<juce::Component*>(&view_)})
        addAndMakeVisible(c);
    setVisible(false);
}

StepEditor::~StepEditor() = default;

const Step* StepEditor::step() const {
    const int i = stepIndex();
    return i < 0 ? nullptr : &session_.controller().show().steps[static_cast<size_t>(i)];
}

int StepEditor::stepIndex() const {
    return session_.isOpen() ? indexOfId(session_.controller().show(), id_) : -1;
}

void StepEditor::open(StepId id) {
    id_ = id;
    builtSignature_.clear();
    builtWidth_ = -1;
    view_.setViewPosition(0, 0);
    setVisible(true);
    refresh();
}

void StepEditor::paint(juce::Graphics& g) {
    g.fillAll(kPanel);
    g.setColour(juce::Colours::black);
    g.fillRect(0, 0, 2, getHeight());
}

void StepEditor::resized() {
    auto r = getLocalBounds().reduced(8, 6);
    auto top = r.removeFromTop(kRow);
    close_.setBounds(top.removeFromRight(100));
    title_.setBounds(top);
    view_.setBounds(r.withTrimmedTop(kGap));
    builtWidth_ = -1;  // the fields are laid out for one width; refresh() rebuilds
}

// What the layout depends on. Values are not in it (they are pulled), so typing
// in one field and then tapping another never rebuilds under the finger.
juce::String StepEditor::signature() const {
    const int index = stepIndex();
    if (index < 0) return "gone";
    const auto& controller = session_.controller();
    const auto& show = controller.show();
    const Step& st = show.steps[static_cast<size_t>(index)];
    juce::String s = juce::String(static_cast<int>(session_.mode())) + "|" + juce::String(st.id) + "|" +
                     (st.sound ? "s" : "-") + "|" + juce::String(static_cast<int>(st.others.kind)) + "|";
    if (st.sound) {
        for (const auto& f : show.files)
            if (f.id == st.sound->file) s += text(f.path);
    }
    const auto expected = controller.expectedRunningAt(index);
    for (const StepId id : expected) {
        const int at = indexOfId(show, id);
        s += "," + juce::String(id) + ":" + juce::String(choiceOf(findChange(st, id))) + (at >= 0 ? stepLabel(show, at) : "?");
    }
    s += "|";
    for (const Change& c : st.changes)
        if (std::find(expected.begin(), expected.end(), c.target) == expected.end()) {
            const int at = indexOfId(show, c.target);
            s += "," + juce::String(c.target) + (at >= 0 ? stepLabel(show, at) : "?");
        }
    return s;
}

void StepEditor::refresh() {
    if (!isVisible()) return;
    if (stepIndex() < 0) {  // deleted (here or elsewhere): nothing left to edit
        if (onClose) onClose();
        return;
    }
    const auto& show = session_.controller().show();
    const int index = stepIndex();
    title_.setText("Step " + juce::String(index + 1) + "  \"" + text(show.steps[static_cast<size_t>(index)].name) + "\"",
                   juce::dontSendNotification);
    const int width = view_.getMaximumVisibleWidth();
    auto sig = signature();
    if (sig != builtSignature_ || width != builtWidth_) {
        // A field being typed in commits first (focus loss), so the rebuild starts from the saved state.
        if (auto* focused = juce::Component::getCurrentlyFocusedComponent();
            focused != nullptr && content_.isParentOf(focused))
            focused->giveAwayKeyboardFocus();
        sig = signature();
        builtWidth_ = width;
        rebuild(sig);
        return;
    }
    pullAll(false);
}

void StepEditor::pullAll(bool force) {
    for (auto& pull : pulls_) pull(force);
}

void StepEditor::edit(Action action, const std::function<bool(Show&, Step&)>& change) {
    const StepId id = id_;
    const bool done = session_.edit(action, [&](Show& show) {
        for (auto& st : show.steps)
            if (st.id == id) return change(show, st);
        return false;
    });
    if (!done) builtSignature_.clear();  // refused: fields that already moved on screen are rebuilt from the model
}

juce::Label& StepEditor::labelAt(const juce::String& t, int x, int width, juce::Colour colour) {
    auto& l = adopt(content_, owned_, std::make_unique<juce::Label>());
    l.setText(t, juce::dontSendNotification);
    l.setColour(juce::Label::textColourId, colour);
    l.setBounds(x, y_, width, kRow);
    return l;
}

void StepEditor::addNumber(const juce::String& label, const Spec& spec, int x, int labelWidth, int fieldWidth) {
    if (label.isNotEmpty()) labelAt(label, x, labelWidth);
    auto& nf = adopt(content_, owned_, std::make_unique<NumberField>(spec.step, spec.lo, spec.hi, spec.format));
    nf.setBounds(x + labelWidth, y_, fieldWidth, kRow);
    nf.setAllowEmpty(spec.optional);
    nf.setEditable(allowed(session_.mode(), spec.action));
    const auto get = spec.get;
    const auto set = spec.set;
    const auto action = spec.action;
    const bool timecode = spec.format == NumberField::Format::Timecode;
    // Reads the model into the field; skipped while typing unless forced (after a commit).
    auto pull = [this, get, timecode, &nf](bool force) {
        if (!force && nf.isEditing()) return;
        const Step* st = step();
        const auto v = st != nullptr ? get(session_.controller().show(), *st) : std::nullopt;
        if (timecode) nf.setFps(session_.controller().show().settings.timecodeFps);
        if (v)
            nf.setValue(*v);
        else
            nf.setEmpty();
    };
    auto apply = [this, set, action, pull](std::optional<double> v) {
        const char* refused = nullptr;
        edit(action, [&](Show& show, Step& st) {
            refused = set(show, st, v);
            return refused == nullptr;
        });
        if (refused != nullptr) session_.flash(refused);
        pull(true);  // the stored value, whether or not the edit went through
    };
    nf.onCommit = [apply](double v) { apply(v); };
    nf.onClear = [apply] { apply(std::nullopt); };
    pulls_.push_back(pull);
    pull(true);
}

void StepEditor::radioRow(const std::vector<std::pair<juce::String, int>>& items, int selected, int group, bool enabled,
                          const std::function<void(int)>& pick, juce::TextButton** first) {
    int total = 0;
    for (const auto& i : items) total += i.second;
    const int width = content_.getWidth();
    int x = 0, used = 0;
    for (size_t n = 0; n < items.size(); ++n) {
        used += items[n].second;
        const int right = width * used / total;
        auto& b = adopt(content_, owned_, std::make_unique<juce::TextButton>(items[n].first));
        b.setBounds(x, y_, right - x - 4, kRow);
        x = right;
        b.setClickingTogglesState(true);
        b.setRadioGroupId(group);
        b.setColour(juce::TextButton::buttonOnColourId, kAccent);
        b.setWantsKeyboardFocus(false);
        b.setToggleState(static_cast<int>(n) == selected, juce::dontSendNotification);
        b.setEnabled(enabled);
        b.onClick = [pick, n] { pick(static_cast<int>(n)); };
        if (n == 0 && first != nullptr) *first = &b;
    }
}

void StepEditor::addChangeRow(StepId target, int row) {
    const auto& show = session_.controller().show();
    const Step& st = *step();
    const int at = indexOfId(show, target);
    const bool structure = allowed(session_.mode(), Action::EditStructure);
    labelAt(at >= 0 ? stepLabel(show, at) : juce::String("(deleted step)"), 0, content_.getWidth());
    y_ += kRow;

    const Change* current = findChange(st, target);
    juce::TextButton* defaultButton = nullptr;
    // The default button names what "everything else" will do to this sound.
    const auto defaultText = [this] {
        const Step* s = step();
        return s != nullptr && s->others.kind == ChangeKind::FadeOut ? "default: fade out " + seconds(s->others.seconds)
                                                                      : juce::String("default: keep");
    };
    radioRow(
        {{defaultText(), 40}, {"keep", 17}, {"to level", 22}, {"fade out", 21}}, choiceOf(current), 100 + row, structure,
        [this, target](int choice) {
            edit(Action::EditStructure, [&](Show& sh, Step& s) {
                const Change* old = findChange(s, target);
                if (choiceOf(old) == choice) return false;  // tapped the choice that is already on
                if (choice == 0) {
                    removeChange(s, target);
                    return true;
                }
                Change c;
                c.target = target;
                c.kind = choice == 1 ? ChangeKind::Keep : choice == 2 ? ChangeKind::Level : ChangeKind::FadeOut;
                if (choice == 2) {
                    // A level change starts at the target's own level, so picking it changes nothing audible yet.
                    const int t = indexOfId(sh, target);
                    const double own = t >= 0 && sh.steps[static_cast<size_t>(t)].sound
                                           ? sh.steps[static_cast<size_t>(t)].sound->levelDb
                                           : 0.0;
                    c.levelDb = std::clamp(old != nullptr ? old->levelDb : own, kEditLevelMinDb, kEditLevelMaxDb);
                }
                if (choice >= 2)
                    c.seconds = std::clamp(old != nullptr && old->kind != ChangeKind::Keep ? old->seconds
                                                                                           : sh.settings.defaultFadeSeconds,
                                           0.0, kEditMaxSeconds);
                if (!setChange(s, c)) {
                    session_.flash("At most " + juce::String(kMaxChanges) + " changes per step");
                    return false;
                }
                return true;
            });
        },
        &defaultButton);
    if (defaultButton != nullptr)
        pulls_.push_back([defaultButton, defaultText](bool) { defaultButton->setButtonText(defaultText()); });
    y_ += kRow + kGap;

    // One field of this change: its level (dB) or its seconds.
    const auto changeField = [target](bool level) {
        return Spec{level ? NumberField::Format::Db : NumberField::Format::Seconds,
                    0.5,
                    level ? kEditLevelMinDb : 0.0,
                    level ? kEditLevelMaxDb : kEditMaxSeconds,
                    Action::EditValue,
                    false,
                    [target, level](const Show&, const Step& s) -> std::optional<double> {
                        const Change* c = findChange(s, target);
                        return c != nullptr ? std::optional<double>(level ? c->levelDb : c->seconds) : std::nullopt;
                    },
                    [target, level](Show&, Step& s, std::optional<double> v) -> const char* {
                        const Change* c = findChange(s, target);
                        if (c == nullptr || !v) return "That sound has no change here any more";
                        Change n = *c;
                        (level ? n.levelDb : n.seconds) = *v;
                        return setChange(s, n) ? nullptr : "The change could not be stored";
                    }};
    };
    if (current != nullptr && current->kind == ChangeKind::Level) {
        addNumber("", changeField(true), 0, 0, 190);
        labelAt("in", 194, 28);
        addNumber("", changeField(false), 224, 0, 190);
        y_ += kRow + kGap;
    } else if (current != nullptr && current->kind == ChangeKind::FadeOut) {
        labelAt("fade out in", 0, 110);
        addNumber("", changeField(false), 112, 0, 190);
        y_ += kRow + kGap;
    }
}

void StepEditor::rebuild(const juce::String& sig) {
    builtSignature_ = sig;
    const int scrollY = view_.getViewPositionY();
    pulls_.clear();
    owned_.clear();
    y_ = 0;
    content_.setSize(view_.getMaximumVisibleWidth(), 10);
    const int width = content_.getWidth();
    const auto mode = session_.mode();
    const bool structure = allowed(mode, Action::EditStructure);
    auto& controller = session_.controller();
    const Show& show = controller.show();
    const int index = stepIndex();
    const Step& st = show.steps[static_cast<size_t>(index)];

    // Name and note.
    const auto textRow = [&](const char* label, std::string Step::*member) {
        labelAt(label, 0, 70);
        auto& e = adopt(content_, owned_, std::make_unique<juce::TextEditor>());
        e.setBounds(72, y_, width - 72, kRow);
        e.setInputRestrictions(200);
        e.setEnabled(structure);
        const auto pull = [this, &e, member](bool force) {
            const Step* s = step();
            if (s != nullptr && (force || !e.hasKeyboardFocus(false))) e.setText(text(s->*member), false);
        };
        const auto commit = [this, &e, member] {
            const auto typed = e.getText().toStdString();
            const Step* s = step();
            if (s == nullptr || s->*member == typed) return;
            edit(Action::EditStructure, [&](Show&, Step& target) {
                target.*member = typed;
                return true;
            });
        };
        e.onReturnKey = [&e, commit] {
            commit();
            e.giveAwayKeyboardFocus();
        };
        e.onEscapeKey = [&e, pull] {
            pull(true);
            e.giveAwayKeyboardFocus();
        };
        e.onFocusLost = commit;
        pulls_.push_back(pull);
        pull(true);
        y_ += kRow + kGap;
    };
    textRow("Name", &Step::name);
    textRow("Note", &Step::note);

    // The sound.
    if (st.sound) {
        juce::String file = "(file missing)";
        for (const auto& f : show.files)
            if (f.id == st.sound->file) file = juce::File(text(f.path)).getFileName();
        labelAt("Sound:  " + file, 0, width);
        y_ += kRow;
        using F = NumberField::Format;
        const auto soundNumber = [&](const char* label, F f, double step, double lo, double hi, Action a, bool optional,
                                     std::function<std::optional<double>(const Sound&, const Show&)> get,
                                     std::function<const char*(Sound&, Show&, std::optional<double>)> set) {
            addNumber(label,
                      Spec{f, step, lo, hi, a, optional,
                           [get](const Show& sh, const Step& s) { return s.sound ? get(*s.sound, sh) : std::nullopt; },
                           [set](Show& sh, Step& s, std::optional<double> v) -> const char* {
                               return s.sound ? set(*s.sound, sh, v) : "This step has no sound";
                           }});
            y_ += kRow + kGap;
        };
        soundNumber(
            "Level", F::Db, 0.5, kEditLevelMinDb, kEditLevelMaxDb, Action::EditValue, false,
            [](const Sound& s, const Show&) { return std::optional<double>(s.levelDb); },
            [](Sound& s, Show&, std::optional<double> v) -> const char* {
                s.levelDb = *v;
                return nullptr;
            });
        soundNumber(
            "Fade in", F::Seconds, 0.5, 0.0, kEditMaxSeconds, Action::EditValue, false,
            [](const Sound& s, const Show&) { return std::optional<double>(s.fadeInSeconds); },
            [](Sound& s, Show&, std::optional<double> v) -> const char* {
                s.fadeInSeconds = *v;
                return nullptr;
            });
        soundNumber(
            "Start delay", F::Seconds, 0.5, 0.0, kEditMaxSeconds, Action::EditValue, false,
            [](const Sound& s, const Show&) { return std::optional<double>(s.startDelaySeconds); },
            [](Sound& s, Show&, std::optional<double> v) -> const char* {
                s.startDelaySeconds = *v;
                return nullptr;
            });

        auto& loop = adopt(content_, owned_, std::make_unique<juce::ToggleButton>("Loop"));
        loop.setBounds(0, y_, 160, kRow);
        loop.setEnabled(structure);
        loop.setWantsKeyboardFocus(false);
        loop.onClick = [this, &loop] {
            const bool on = loop.getToggleState();
            edit(Action::EditStructure, [&](Show&, Step& s) {
                if (!s.sound) return false;
                s.sound->loop = on;
                return true;
            });
        };
        pulls_.push_back([this, &loop](bool) {
            const Step* s = step();
            if (s != nullptr && s->sound) loop.setToggleState(s->sound->loop, juce::dontSendNotification);
        });
        pulls_.back()(true);
        y_ += kRow + kGap;

        soundNumber(
            "Start", F::MinSec, 0.5, 0.0, kMaxPositionSeconds, Action::EditStructure, false,
            [](const Sound& s, const Show&) { return std::optional<double>(s.startSeconds); },
            [](Sound& s, Show&, std::optional<double> v) -> const char* {
                if (s.endSeconds && *v >= *s.endSeconds) return "Start must be before End";
                s.startSeconds = *v;
                return nullptr;
            });
        soundNumber(
            "End", F::MinSec, 0.5, 0.0, kMaxPositionSeconds, Action::EditStructure, true,
            [](const Sound& s, const Show&) { return s.endSeconds; },
            [](Sound& s, Show&, std::optional<double> v) -> const char* {
                if (v && *v <= s.startSeconds) return "End must be after Start";
                s.endSeconds = v;  // empty = play to the end of the file
                return nullptr;
            });
        soundNumber(
            "Timecode", F::Timecode, 1.0, 0.0, 24.0 * 3600.0 * 30.0, Action::EditStructure, true,
            [](const Sound& s, const Show& sh) -> std::optional<double> {
                if (!s.timecodeStart) return std::nullopt;
                const auto& t = *s.timecodeStart;
                return static_cast<double>(((t.h * 60LL + t.m) * 60 + t.s) * sh.settings.timecodeFps + t.f);
            },
            [](Sound& s, Show& sh, std::optional<double> v) -> const char* {
                if (!v) {
                    s.timecodeStart.reset();
                    return nullptr;
                }
                const auto tc = toTimecode(static_cast<long long>(*v), sh.settings.timecodeFps);
                if (tc.h > 23) return "Timecode must be below 24:00:00:00";
                s.timecodeStart = tc;
                return nullptr;
            });
        // File trim lives in the file, shared by every step and pad that uses it.
        addNumber("File trim",
                  Spec{F::Db, 0.5, kEditTrimMinDb, kEditTrimMaxDb, Action::EditValue, false,
                       [](const Show& sh, const Step& s) -> std::optional<double> {
                           if (!s.sound) return std::nullopt;
                           for (const auto& f : sh.files)
                               if (f.id == s.sound->file) return f.trimDb;
                           return std::nullopt;
                       },
                       [](Show& sh, Step& s, std::optional<double> v) -> const char* {
                           if (!s.sound || !v) return "This step has no sound";
                           for (auto& f : sh.files)
                               if (f.id == s.sound->file) {
                                   f.trimDb = *v;
                                   return nullptr;
                               }
                           return "The sound's file is missing";
                       }});
        y_ += kRow;
        labelAt("(applies to every step and pad using this file)", 0, width, kHint);
        y_ += kRow + kGap;
    } else {
        labelAt("No sound: this step only changes other sounds.", 0, width, kHint);
        y_ += kRow + kGap;
    }

    // Changes to sounds expected to be running here.
    const auto expected = controller.expectedRunningAt(index);
    labelAt("Running sounds at this step (expected)", 0, width).setFont(juce::FontOptions(16.0f, juce::Font::bold));
    y_ += kRow;
    const int stored = static_cast<int>(st.changes.size());
    labelAt(juce::String(stored) + " of " + juce::String(kMaxChanges) + " changes" +
                (stored >= kMaxChanges ? " - limit reached" : ""),
            0, width, stored >= kMaxChanges ? juce::Colours::orange : kHint);
    y_ += kRow;
    if (expected.empty()) {
        labelAt("None: nothing is expected to be running before this step.", 0, width, kHint);
        y_ += kRow + kGap;
    }
    int row = 0;
    for (const StepId id : expected) addChangeRow(id, row++);

    // Changes for steps that are not expected here any more; listed so nothing hidden stays in the file.
    for (const Change& c : st.changes) {
        if (std::find(expected.begin(), expected.end(), c.target) != expected.end()) continue;
        const int at = indexOfId(show, c.target);
        const juce::String what = c.kind == ChangeKind::Keep ? "keep" : c.kind == ChangeKind::Level ? "to level" : "fade out";
        labelAt((at >= 0 ? stepLabel(show, at) : juce::String("deleted step")) + ": " + what + " (not running here)", 0,
                width - 110, kHint);
        auto& remove = adopt(content_, owned_, std::make_unique<juce::TextButton>("Remove"));
        remove.setBounds(width - 106, y_, 106, kRow);
        remove.setEnabled(structure);
        remove.setWantsKeyboardFocus(false);
        remove.onClick = [this, target = c.target] {
            edit(Action::EditStructure, [&](Show&, Step& s) {
                removeChange(s, target);
                return true;
            });
        };
        y_ += kRow + kGap;
    }

    // Everything else.
    labelAt("Everything else:", 0, width);
    y_ += kRow;
    radioRow({{"keep", 1}, {"fade out in", 2}}, st.others.kind == ChangeKind::FadeOut ? 1 : 0, 99, structure,
             [this](int choice) {
                 edit(Action::EditStructure, [&](Show& sh, Step& s) {
                     const auto kind = choice == 1 ? ChangeKind::FadeOut : ChangeKind::Keep;
                     if (s.others.kind == kind) return false;
                     s.others.kind = kind;
                     if (kind == ChangeKind::FadeOut && s.others.seconds <= 0.0)
                         s.others.seconds = sh.settings.defaultFadeSeconds;
                     return true;
                 });
             });
    y_ += kRow + kGap;
    if (st.others.kind == ChangeKind::FadeOut) {
        addNumber("",
                  Spec{NumberField::Format::Seconds, 0.5, 0.0, kEditMaxSeconds, Action::EditValue, false,
                       [](const Show&, const Step& s) { return std::optional<double>(s.others.seconds); },
                       [](Show&, Step& s, std::optional<double> v) -> const char* {
                           s.others.seconds = *v;
                           return nullptr;
                       }},
                  0, 0, 190);
        y_ += kRow + kGap;
    }

    // Gap / overlap: when this step's sound starts and when the sounds it replaces are silent.
    auto& overlap = labelAt("", 0, width, kHint);
    overlap.setBounds(0, y_, width, kRow * 2);
    pulls_.push_back([this, &overlap, expected](bool) {
        const Step* s = step();
        if (s == nullptr) return;
        const auto& sh = session_.controller().show();
        juce::StringArray parts;
        if (s->sound) parts.add(text(s->name) + " starts at GO + " + seconds(s->sound->startDelaySeconds));
        for (const StepId id : expected) {
            const int at = indexOfId(sh, id);
            const Change* c = findChange(*s, id);
            const bool fades = c != nullptr ? c->kind == ChangeKind::FadeOut : s->others.kind == ChangeKind::FadeOut;
            if (fades && at >= 0)
                parts.add(text(sh.steps[static_cast<size_t>(at)].name) + " is silent at " +
                          seconds(c != nullptr ? c->seconds : s->others.seconds));
        }
        overlap.setText(parts.joinIntoString(";  "), juce::dontSendNotification);
    });
    pulls_.back()(true);
    y_ += kRow * 2 + kGap;

    // Delete step (Edit mode only; the permission table also guards the call).
    auto& del = adopt(content_, owned_, std::make_unique<juce::TextButton>("Delete step"));
    del.setBounds(width - 160, y_, 160, kRow);
    del.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff7a2b2b));
    del.setEnabled(allowed(mode, Action::Delete));
    del.setWantsKeyboardFocus(false);
    del.onClick = [this] { session_.deleteStep(stepIndex()); };  // refresh() sees it gone and closes
    y_ += kRow + kGap;

    content_.setSize(width, y_);
    view_.setViewPosition(0, scrollY);
}

}  // namespace bigtop::app
