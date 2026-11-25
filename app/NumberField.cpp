// NumberField.cpp: parsing and showing of the editor's number boxes.
#include "NumberField.h"

#include <cmath>

#include "bigtop/ShowEdits.h"
#include "bigtop/StepDetails.h"

namespace bigtop::app {

namespace {

// A token of digits only (no sign, no spaces): the parts of m:ss and hh:mm:ss:ff.
bool digitsOnly(const juce::String& t) { return t.isNotEmpty() && t.containsOnly("0123456789"); }

}  // namespace

NumberField::NumberField(double step, double lo, double hi, Format f) : step_(step), lo_(lo), hi_(hi), format_(f) {
    box_.setJustification(juce::Justification::centred);
    box_.setSelectAllWhenFocused(true);
    box_.setInputRestrictions(24);  // no sensible number is longer
    box_.onReturnKey = [this] {
        commitText();
        box_.giveAwayKeyboardFocus();  // the main window takes the keys (Space = GO) back
    };
    box_.onEscapeKey = [this] {
        redraw();  // drop what was typed
        box_.giveAwayKeyboardFocus();
    };
    box_.onFocusLost = [this] { commitText(); };
    addAndMakeVisible(box_);

    unit_.setText(f == Format::Db ? "dB" : f == Format::Seconds ? "s" : "", juce::dontSendNotification);
    unit_.setColour(juce::Label::textColourId, juce::Colours::grey);
    unit_.setInterceptsMouseClicks(false, false);
    addAndMakeVisible(unit_);

    minus_.onClick = [this] {
        box_.giveAwayKeyboardFocus();  // commits what was typed first, so -/+ start from it
        commitValue(value_.value_or(lo_) - step_);
    };
    plus_.onClick = [this] {
        box_.giveAwayKeyboardFocus();
        commitValue(value_.value_or(lo_) + step_);
    };
    for (auto* b : {&minus_, &plus_}) {
        b->setWantsKeyboardFocus(false);
        addAndMakeVisible(b);
    }
    redraw();
}

void NumberField::setValue(double v) {
    value_ = v;
    redraw();
}

void NumberField::setEmpty() {
    value_.reset();
    redraw();
}

void NumberField::setFps(int fps) noexcept { fps_ = fps; }

void NumberField::setEditable(bool editable) { setEnabled(editable); }

bool NumberField::isEditing() const { return box_.hasKeyboardFocus(false); }

juce::String NumberField::format(double v) const {
    switch (format_) {
        case Format::Db:
        case Format::Seconds: return formatDecimal(v);  // shared with the step list's Details lines
        case Format::Count: return juce::String(juce::roundToInt(v));
        case Format::MinSec: return formatMinSec(v);
        case Format::Timecode: {
            const auto total = static_cast<long long>(std::llround(v));
            const long long perSecond = fps_;
            return juce::String::formatted("%02lld:%02lld:%02lld:%02lld", total / perSecond / 3600,
                                           total / perSecond / 60 % 60, total / perSecond % 60, total % perSecond);
        }
    }
    return {};
}

std::optional<double> NumberField::parse(const juce::String& text) const {
    const auto t = text.trim();
    if (format_ == Format::Timecode) {
        const auto parts = juce::StringArray::fromTokens(t, ":;", "");
        if (parts.size() != 4) return std::nullopt;
        for (const auto& p : parts)
            if (!digitsOnly(p) || p.length() > 3) return std::nullopt;
        const int h = parts[0].getIntValue(), m = parts[1].getIntValue(), s = parts[2].getIntValue(),
                  f = parts[3].getIntValue();
        if (m > 59 || s > 59 || f >= fps_) return std::nullopt;  // frames are checked against the frame rate
        return static_cast<double>(((h * 60LL + m) * 60 + s) * fps_ + f);
    }
    if (format_ == Format::MinSec && t.contains(":")) {
        const auto parts = juce::StringArray::fromTokens(t, ":", "");
        if (parts.size() != 2 || !digitsOnly(parts[0]) || parts[0].length() > 4) return std::nullopt;
        const auto seconds = parseNumber(parts[1].toStdString());
        if (!seconds || *seconds < 0.0) return std::nullopt;
        return parts[0].getIntValue() * 60.0 + *seconds;
    }
    return parseNumber(t.toStdString());
}

void NumberField::redraw() { box_.setText(value_ ? format(*value_) : juce::String(), false); }

void NumberField::commitText() {
    const auto text = box_.getText().trim();
    if (text.isEmpty()) {
        if (allowEmpty_ && value_) {
            value_.reset();
            if (onClear) onClear();
        }
        redraw();  // a field that must have a value shows it again
        return;
    }
    // Unchanged from what was shown: the display may be rounder than the stored value, so
    // re-parsing it would rewrite (and save) a finer value as the rounded one.
    if (value_ && text == format(*value_)) {
        redraw();
        return;
    }
    const auto parsed = parse(text);
    // checkedEdit clamps and rejects NaN / infinity; anything it refuses leaves the stored value.
    const auto checked = parsed ? checkedEdit(*parsed, lo_, hi_) : std::nullopt;
    if (!checked || (value_ && *value_ == *checked)) {
        redraw();
        return;
    }
    commitValue(*checked);
}

void NumberField::commitValue(double v) {
    const auto checked = checkedEdit(v, lo_, hi_);
    if (!checked || (value_ && *value_ == *checked)) return;  // at the limit already: nothing changed
    value_ = *checked;
    redraw();  // shows the clamped value, not what was typed
    if (onCommit) onCommit(*checked);
}

void NumberField::resized() {
    auto r = getLocalBounds();
    minus_.setBounds(r.removeFromLeft(kHeight));
    plus_.setBounds(r.removeFromRight(kHeight));
    box_.setBounds(r.reduced(2, 0));
    // The unit sits inside the box's right end, where it does not take width from the digits.
    unit_.setBounds(r.removeFromRight(34));
}

}  // namespace bigtop::app
