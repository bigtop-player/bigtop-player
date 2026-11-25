/// @file NumberField.h
/// @brief A touch-friendly number entry: a [-] button, a text box and a [+]
///        button, with the number parsers it uses.
///
/// Rules (design 6): a value is committed on Enter, on focus loss and on every
/// -/+ tap, never per keystroke; only a value that passed checkedEdit() is
/// committed (clamped to the field's range); anything else (letters, "1e999",
/// an empty box on a field that must have a value) puts the stored value back.
/// The text box accepts '.' and ',' as the decimal separator.
#pragma once

#include <cfloat>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>

#include <juce_gui_basics/juce_gui_basics.h>

namespace bigtop::app {

/// @brief Reads a decimal number the way an operator types it.
/// @param text [in] Optional sign, digits with at most one '.' or ',' as the
///        decimal separator, optional exponent ("1e2"); spaces around are ignored.
/// @return The value; nullopt for anything else, including an empty text, a lone
///         sign, trailing junk and values that overflow a double ("1e999").
/// @note No exceptions, no allocation, usable in constant expressions. Any thread.
constexpr std::optional<double> parseNumber(std::string_view text) noexcept {
    while (!text.empty() && text.front() == ' ') text.remove_prefix(1);
    while (!text.empty() && text.back() == ' ') text.remove_suffix(1);
    std::size_t i = 0;
    const std::size_t n = text.size();
    bool negative = false;
    if (i < n && (text[i] == '+' || text[i] == '-')) negative = text[i++] == '-';

    double mantissa = 0.0;
    int digits = 0, fractionDigits = 0;
    bool point = false;
    for (; i < n; ++i) {
        const char c = text[i];
        if (c >= '0' && c <= '9') {
            if (mantissa > 1e300) return std::nullopt;  // more digits than a double can hold
            mantissa = mantissa * 10.0 + (c - '0');
            ++digits;
            if (point) ++fractionDigits;
        } else if ((c == '.' || c == ',') && !point) {
            point = true;
        } else {
            break;
        }
    }
    if (digits == 0) return std::nullopt;

    int exponent = 0;
    if (i < n && (text[i] == 'e' || text[i] == 'E')) {
        ++i;
        bool expNegative = false;
        if (i < n && (text[i] == '+' || text[i] == '-')) expNegative = text[i++] == '-';
        int expDigits = 0;
        for (; i < n && text[i] >= '0' && text[i] <= '9'; ++i, ++expDigits)
            if (exponent < 10000) exponent = exponent * 10 + (text[i] - '0');
        if (expDigits == 0) return std::nullopt;
        if (expNegative) exponent = -exponent;
    }
    if (i != n) return std::nullopt;  // junk after the number

    // Scaling by repeated *10 or /10 keeps short decimals exact ("2,5" is 25 / 10)
    // and lets the overflow check run before the multiplication that would overflow
    // (an overflow is not allowed in a constant expression).
    double value = mantissa;
    for (int e = exponent - fractionDigits; e > 0; --e) {
        if (value > DBL_MAX / 10.0) return std::nullopt;
        value *= 10.0;
    }
    for (int e = exponent - fractionDigits; e < 0 && value != 0.0; ++e) value /= 10.0;
    return negative ? -value : value;
}

static_assert(parseNumber("2.5") == 2.5);
static_assert(parseNumber("2,5") == 2.5);       // a German touch keyboard types a comma
static_assert(parseNumber("-12") == -12.0);
static_assert(parseNumber("+3") == 3.0);
static_assert(parseNumber(" 7 ") == 7.0);
static_assert(parseNumber("1e2") == 100.0);
static_assert(parseNumber("1,5e1") == 15.0);
static_assert(parseNumber("0.1") == 0.1);
static_assert(!parseNumber("-"));
static_assert(!parseNumber(""));
static_assert(!parseNumber("   "));
static_assert(!parseNumber("."));
static_assert(!parseNumber("abc"));
static_assert(!parseNumber("2.5x"));
static_assert(!parseNumber("1.2.3"));
static_assert(!parseNumber("--1"));
static_assert(!parseNumber("1e"));
static_assert(!parseNumber("1e999"));   // would be infinity
static_assert(!parseNumber("-1e999"));

/// @brief A number entry with -/+ buttons.
/// @note Message thread. Buttons and box never take the main window's keyboard
///       shortcuts away except while the box is being edited.
class NumberField : public juce::Component {
public:
    /// @brief How the value is shown and typed.
    enum class Format {
        Db,       ///< @brief Decibels, "-15.0" plus a "dB" label.
        Seconds,  ///< @brief Seconds, "2.5" plus an "s" label.
        MinSec,   ///< @brief A position in seconds shown and typed as m:ss.d (also plain seconds).
        Timecode, ///< @brief hh:mm:ss:ff; the value is the total frame count at setFps().
        Count     ///< @brief A whole number without unit (pad rows and columns).
    };

    /// @brief Touch target size: buttons and box height, px.
    static constexpr int kHeight = 48;

    /// @param step Amount one -/+ tap changes the value by (0.5 dB, 0.5 s, 1 frame, 1).
    /// @param lo Lowest accepted value (typed values below are clamped to it).
    /// @param hi Highest accepted value.
    /// @param f Display and entry format.
    NumberField(double step, double lo, double hi, Format f);

    /// @brief Shows a value without committing it.
    /// @param v The stored value; not range-checked (the owner's model decides).
    void setValue(double v);
    /// @brief Shows an empty box (an optional value that is unset: "end", no timecode).
    void setEmpty();
    /// @brief Whether the box accepts "no value" (empty text commits through onClear).
    /// @param allow True for optional values.
    void setAllowEmpty(bool allow) noexcept { allowEmpty_ = allow; }
    /// @brief Frame rate for the Timecode format.
    /// @param fps 24, 25 or 30; frames typed at or above it are refused.
    void setFps(int fps) noexcept;
    /// @brief Enables or disables typing and the buttons (disabled fields stay visible).
    /// @param editable False greys the field out.
    void setEditable(bool editable);
    /// @brief Whether the operator is typing in the box right now.
    /// @return True while the box has keyboard focus; the owner must not overwrite it then.
    bool isEditing() const;

    /// @brief Fired with a checked, clamped value on Enter, focus loss and each -/+ tap.
    std::function<void(double)> onCommit;
    /// @brief Fired when an empty box was committed on an allowEmpty field.
    std::function<void()> onClear;

    /// @brief Lays out [-] box unit [+] (JUCE callback).
    void resized() override;

private:
    /// App test hook (app/tests/AppTestSupport.h): types into the box and taps -/+.
    friend struct AppTestAccess;

    juce::String format(double v) const;
    std::optional<double> parse(const juce::String& text) const;
    void commitText();
    void commitValue(double v);
    void redraw();

    double step_, lo_, hi_;
    Format format_;
    int fps_ = 25;
    bool allowEmpty_ = false;
    std::optional<double> value_;  // nullopt = shown empty
    juce::TextEditor box_;
    juce::Label unit_;
    juce::TextButton minus_{juce::String::fromUTF8("\xe2\x88\x92")}, plus_{"+"};  // U+2212, not the ASCII hyphen
};

}  // namespace bigtop::app
