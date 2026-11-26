// ShowSettingsPanel.cpp: the show settings side panel.
#include "ShowSettingsPanel.h"

#include "bigtop/ShowEdits.h"
#include "bigtop/io/ArtNetSender.h"

namespace bigtop::app {

namespace {

constexpr int kRow = NumberField::kHeight;
constexpr int kGap = 6;
const juce::Colour kPanel(0xff23262b);
const juce::Colour kAccent(0xff3a6ea5);

// Four decimal numbers 0..255 separated by dots: what an Art-Net target must be.
bool isIpv4(const juce::String& s) {
    const auto parts = juce::StringArray::fromTokens(s, ".", "");
    if (parts.size() != 4 || s.endsWithChar('.') || s.startsWithChar('.')) return false;
    for (const auto& p : parts)
        if (p.isEmpty() || p.length() > 3 || !p.containsOnly("0123456789") || p.getIntValue() > 255) return false;
    return true;
}

template <class T>
T& adopt(juce::Component& parent, std::vector<std::unique_ptr<juce::Component>>& owned, std::unique_ptr<T> c) {
    T& ref = *c;
    parent.addAndMakeVisible(ref);
    owned.push_back(std::move(c));
    return ref;
}

}  // namespace

ShowSettingsPanel::ShowSettingsPanel(ShowSession& session) : session_(session) {
    title_.setText("Show settings", juce::dontSendNotification);
    title_.setFont(juce::FontOptions(20.0f, juce::Font::bold));
    close_.setWantsKeyboardFocus(false);
    close_.onClick = [this] {
        if (onClose) onClose();
    };
    view_.setViewedComponent(&content_, false);
    view_.setScrollBarsShown(true, false);
    view_.setWantsKeyboardFocus(false);
    for (juce::Component* c : {static_cast<juce::Component*>(&title_), static_cast<juce::Component*>(&close_),
                               static_cast<juce::Component*>(&view_)})
        addAndMakeVisible(c);
    setVisible(false);
}

ShowSettingsPanel::~ShowSettingsPanel() = default;

void ShowSettingsPanel::open() {
    view_.setViewPosition(0, 0);
    setVisible(true);
    rebuild();
}

void ShowSettingsPanel::paint(juce::Graphics& g) {
    g.fillAll(kPanel);
    g.setColour(juce::Colours::black);
    g.fillRect(0, 0, 2, getHeight());
}

void ShowSettingsPanel::resized() {
    auto r = getLocalBounds().reduced(8, 6);
    auto top = r.removeFromTop(kRow);
    close_.setBounds(top.removeFromRight(100));
    title_.setBounds(top);
    view_.setBounds(r.withTrimmedTop(kGap));
    builtWidth_ = -1;
}

void ShowSettingsPanel::refresh() {
    if (!isVisible() || !session_.isOpen()) return;
    if (contentWidth() != builtWidth_) {
        rebuild();
        return;
    }
    for (auto& pull : pulls_) pull(false);
    if (status_ != nullptr) status_->setText(session_.sender().status().text, juce::dontSendNotification);
}

void ShowSettingsPanel::rebuild() {
    if (!session_.isOpen()) return;
    if (auto* focused = juce::Component::getCurrentlyFocusedComponent();
        focused != nullptr && content_.isParentOf(focused))
        focused->giveAwayKeyboardFocus();
    const auto scrolled = view_.getViewPosition();  // a rebuild keeps the place (owner bug 2026-11-22)
    pulls_.clear();
    owned_.clear();
    status_ = nullptr;
    builtWidth_ = contentWidth();
    content_.setSize(builtWidth_, content_.getHeight());
    const int width = content_.getWidth();
    int y = 0;
    const bool editable = allowed(session_.mode(), Action::ShowSettings);

    const auto label = [&](const juce::String& t, int x, int w, juce::Colour colour = juce::Colours::white) -> juce::Label& {
        auto& l = adopt(content_, owned_, std::make_unique<juce::Label>());
        l.setText(t, juce::dontSendNotification);
        l.setColour(juce::Label::textColourId, colour);
        l.setBounds(x, y, w, kRow);
        return l;
    };
    // One number field bound to a show setting.
    const auto number = [&](const juce::String& name, NumberField::Format f, double lo, double hi, double step,
                            std::function<double(const Show&)> get, std::function<void(Show&, double)> set) {
        label(name, 0, 150);
        auto& nf = adopt(content_, owned_, std::make_unique<NumberField>(step, lo, hi, f));
        nf.setBounds(150, y, 220, kRow);
        nf.setEditable(editable);
        const auto pull = [this, get, &nf](bool force) {
            if (session_.isOpen() && (force || !nf.isEditing())) nf.setValue(get(session_.controller().show()));
        };
        nf.onCommit = [this, set, pull](double v) {
            session_.edit(Action::ShowSettings, [&](Show& show) {
                const Show before = show;
                set(show, v);
                return show != before;  // e.g. a pad grid that clamps back to the same size: no save
            });
            pull(true);  // e.g. a pad grid reduced to fit 24 pads shows what was kept
        };
        pulls_.push_back(pull);
        pull(true);
        y += kRow + kGap;
    };

    number("Default fade", NumberField::Format::Seconds, 0.0, kEditMaxSeconds, 0.5,
           [](const Show& s) { return s.settings.defaultFadeSeconds; },
           [](Show& s, double v) { s.settings.defaultFadeSeconds = v; });

    // Pause in Show mode: off by default, so Show mode keeps only what is safe in a show.
    auto& pauseInShow = adopt(content_, owned_, std::make_unique<juce::ToggleButton>("Pause in Show mode"));
    pauseInShow.setComponentID("pauseInShow");
    pauseInShow.setBounds(150, y, width - 150, kRow);
    pauseInShow.setEnabled(editable);
    const auto pullPause = [this, &pauseInShow](bool) {
        if (session_.isOpen())
            pauseInShow.setToggleState(session_.controller().show().settings.pauseInShow, juce::dontSendNotification);
    };
    pauseInShow.onClick = [this, &pauseInShow, pullPause] {
        const bool on = pauseInShow.getToggleState();
        session_.edit(Action::ShowSettings, [on](Show& s) {
            if (s.settings.pauseInShow == on) return false;
            s.settings.pauseInShow = on;
            return true;
        });
        pullPause(true);  // a refused edit shows the stored value again
    };
    pulls_.push_back(pullPause);
    pullPause(true);
    y += kRow + kGap;

    // Art-Net target: used only with the "Default" adapter (design 9).
    label("Art-Net target", 0, 150);
    auto& target = adopt(content_, owned_, std::make_unique<juce::TextEditor>());
    target.setBounds(150, y, width - 150, kRow);
    target.setInputRestrictions(15, "0123456789.");
    target.setEnabled(editable);
    const auto pullTarget = [this, &target](bool force) {
        if (session_.isOpen() && (force || !target.hasKeyboardFocus(false)))
            target.setText(juce::String(session_.controller().show().settings.artnetTarget), false);
    };
    const auto commitTarget = [this, &target, pullTarget] {
        const auto typed = target.getText().trim();
        if (session_.isOpen() && typed.toStdString() != session_.controller().show().settings.artnetTarget) {
            if (isIpv4(typed))
                session_.edit(Action::ShowSettings, [&](Show& s) {
                    s.settings.artnetTarget = typed.toStdString();
                    return true;
                });
            else
                session_.flash("Not an IPv4 address (like 255.255.255.255)");
        }
        pullTarget(true);
    };
    target.onReturnKey = [&target, commitTarget] {
        commitTarget();
        target.giveAwayKeyboardFocus();
    };
    target.onEscapeKey = [&target, pullTarget] {
        pullTarget(true);
        target.giveAwayKeyboardFocus();
    };
    target.onFocusLost = commitTarget;
    pulls_.push_back(pullTarget);
    pullTarget(true);
    y += kRow + kGap;

    // Adapter list: "Default" first, then every adapter with an IPv4 address.
    label("Network adapter (the target above applies to Default only)", 0, width, juce::Colours::grey);
    y += kRow;
    const auto stored = session_.artNetAdapter();
    std::vector<std::pair<juce::String, juce::String>> entries{{"Default (the system picks)", ""}};
    bool found = stored.isEmpty();
    for (const auto& a : session_.adapters()) {
        entries.emplace_back(a.label(), a.address.toString());
        found = found || a.address.toString() == stored;
    }
    if (!found) entries.emplace_back(stored + " (not found, using Default)", stored);  // keep the choice visible
    for (const auto& [name, address] : entries) {
        auto& b = adopt(content_, owned_, std::make_unique<juce::TextButton>(name));
        b.setBounds(0, y, width, kRow);
        b.setClickingTogglesState(true);
        b.setRadioGroupId(201);
        b.setColour(juce::TextButton::buttonOnColourId, kAccent);
        b.setWantsKeyboardFocus(false);
        b.setEnabled(editable);
        b.setToggleState(address == stored, juce::dontSendNotification);
        b.onClick = [this, address = address] { session_.setArtNetAdapter(address); };
        y += kRow + kGap;
    }

    // Timecode frame rate.
    label("Timecode frame rate", 0, 190);
    {
        const int fpsValues[] = {24, 25, 30};
        int x = 190;
        const int each = (width - 190) / 3;
        for (const int fps : fpsValues) {
            auto& b = adopt(content_, owned_, std::make_unique<juce::TextButton>(juce::String(fps)));
            b.setBounds(x, y, each - 4, kRow);
            x += each;
            b.setClickingTogglesState(true);
            b.setRadioGroupId(202);
            b.setColour(juce::TextButton::buttonOnColourId, kAccent);
            b.setWantsKeyboardFocus(false);
            b.setEnabled(editable);
            b.onClick = [this, fps] {
                session_.edit(Action::ShowSettings, [&](Show& s) {
                    s.settings.timecodeFps = fps;
                    return true;
                });
            };
            pulls_.push_back([this, &b, fps](bool) {
                if (session_.isOpen())
                    b.setToggleState(session_.controller().show().settings.timecodeFps == fps, juce::dontSendNotification);
            });
            pulls_.back()(true);
        }
        y += kRow + kGap;
    }

    // Pad grid. resizePadGrid clamps and keeps rows * columns within the pad limit.
    number("Pad rows", NumberField::Format::Count, kMinPadRows, kMaxPadRows, 1.0,
           [](const Show& s) { return static_cast<double>(s.settings.padRows); },
           [](Show& s, double v) { resizePadGrid(s, static_cast<int>(v), s.settings.padCols); });
    number("Pad columns", NumberField::Format::Count, kMinPadCols, kMaxPadCols, 1.0,
           [](const Show& s) { return static_cast<double>(s.settings.padCols); },
           [](Show& s, double v) { resizePadGrid(s, s.settings.padRows, static_cast<int>(v)); });
    label("At most " + juce::String(kMaxPads) + " pads in total.", 0, width, juce::Colours::grey);
    y += kRow + kGap;

    status_ = &label("", 0, width, juce::Colours::grey);
    status_->setText(session_.sender().status().text, juce::dontSendNotification);
    y += kRow + kGap;

    content_.setSize(width, y);
    view_.setViewPosition(scrolled);  // clamped by the viewport when the content got shorter
}

int ShowSettingsPanel::contentWidth() const {
    // Always leave room for the scroll bar: with the visible width the content would get narrower when
    // the bar appears, the next refresh would rebuild, and every rebuild reset the scroll position.
    return juce::jmax(0, view_.getWidth() - view_.getScrollBarThickness());
}

}  // namespace bigtop::app
