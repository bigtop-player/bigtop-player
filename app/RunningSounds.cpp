// RunningSounds.cpp: rows keyed by voice key, a position bar that seeks on release,
// faders that send only when moved, and the per-sound Fade / Keep / Edit buttons.
#include "RunningSounds.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "Pointer.h"
#include "bigtop/io/Waveform.h"
#include "UiClock.h"

namespace bigtop::app {

namespace {

constexpr double kFaderMaxDb = 12.0;    // the engine's trim ceiling
constexpr double kFaderMinDb = -60.0;   // lowest dB step; below it is the -inf position
constexpr double kFaderInfPos = -61.0;  // slider value of the -inf position

const Step* findStep(const Show& show, StepId id) {
    const auto it = std::find_if(show.steps.begin(), show.steps.end(), [id](const Step& s) { return s.id == id; });
    return it != show.steps.end() ? &*it : nullptr;
}

/// The position bar: a progress track with a playhead line across the row's full height (the touch
/// target). Once the file's waveform summary is read (ShowSession::peaks()), the track is replaced by
/// the waveform of the bar's range, played part blue, the rest grey. Dragging moves only the thumb; the seek is sent once, on release, so the
/// streaming tail reader is asked for one position per gesture (design 7.1).
class PositionBar : public juce::Component {
public:
    /// Called once per gesture with the frame to seek to.
    std::function<void(int64_t)> onSeek;

    /// Sets the range the bar spans (start point to end point, or the loop range).
    void setRange(int64_t start, int64_t end) {
        if (start == start_ && end == end_) return;
        start_ = start;
        end_ = end;
        repaint();  // the waveform under the bar shows the new range
    }

    /// Moves the thumb to a playing position; ignored while a finger is dragging it.
    void setPosition(int64_t frame) {
        if (dragging_ || frame == position_) return;
        position_ = frame;
        repaint();
    }

    /// Sets the file's waveform summary (null = none yet: the plain bar is drawn).
    void setPeaks(std::shared_ptr<const io::Peaks> peaks) {
        if (peaks == peaks_) return;
        peaks_ = std::move(peaks);
        repaint();
    }

    void paint(juce::Graphics& g) override {
        // A progress bar, not a slider: filled track and a thin playhead line, no round knob (the fader keeps that).
        const auto track = getLocalBounds().toFloat().reduced(4.0f, 0.0f).withSizeKeepingCentre(
            static_cast<float>(getWidth()) - 8.0f, 12.0f);
        if (peaks_ == nullptr) {
            g.setColour(juce::Colour(0xff3a3a3a));
            g.fillRect(track);
        }
        float frac = 0.0f;
        if (dragging_ && getWidth() > 0) {
            frac = static_cast<float>(std::clamp(dragX_, 0, getWidth())) / static_cast<float>(getWidth());
        } else if (end_ > start_) {
            frac = static_cast<float>(std::clamp(
                static_cast<double>(position_ - start_) / static_cast<double>(end_ - start_), 0.0, 1.0));
        }
        const float x = track.getX() + frac * track.getWidth();
        float headTop = track.getY() - 8.0f, headBottom = track.getBottom() + 8.0f;
        if (peaks_ != nullptr) {
            // Its own darker panel sets the waveform apart from the row's background.
            g.setColour(juce::Colour(0xff15181b));
            g.fillRoundedRectangle(track.getX(), 0.0f, track.getWidth(), static_cast<float>(getHeight()), 3.0f);
            paintWave(g, track, x);
            // The playhead covers exactly the height the wave uses (paintWave keeps 2 px from the edges).
            headTop = 2.0f;
            headBottom = static_cast<float>(getHeight()) - 2.0f;
        } else {
            g.setColour(juce::Colour(0xff4a9eff));
            g.fillRect(track.withRight(x));
        }
        g.setColour(juce::Colours::white);
        g.fillRect(x - 1.5f, headTop, 3.0f, headBottom - headTop);
    }

    /// Ends a drag without seeking (the sound restarted under the finger).
    void cancelDrag() {
        dragging_ = false;
        repaint();
    }

    void mouseDown(const juce::MouseEvent& e) override {
        if (end_ <= start_ || dragging_) return;  // nothing to seek in; a second finger does not take over
        source_ = pointerIndex(e);
        dragging_ = true;
        dragX_ = e.x;
        repaint();
    }

    void mouseDrag(const juce::MouseEvent& e) override {
        if (!dragging_ || pointerIndex(e) != source_) return;
        dragX_ = e.x;
        repaint();
    }

    void mouseUp(const juce::MouseEvent& e) override {
        if (!dragging_ || pointerIndex(e) != source_) return;
        dragging_ = false;
        // A release outside the bar still seeks (clamped to the ends); the thumb must not snap back first.
        const int64_t frame = seekFrameForX(e.x, getWidth(), start_, end_);
        position_ = frame;
        if (onSeek) onSeek(frame);
        repaint();
    }

private:
    // The waveform across the bar's full height, one min/max line per pixel column of the
    // track: blue left of the playhead (played), grey right of it.
    void paintWave(juce::Graphics& g, juce::Rectangle<float> track, float playX) {
        const int cols = static_cast<int>(track.getWidth());
        // A 30 Hz repaint only moves the playhead: the columns are sliced again only when what they show changes.
        if (cols != cachedCols_ || start_ != cachedStart_ || end_ != cachedEnd_ || peaks_ != cachedPeaks_) {
            io::peakRange(*peaks_, start_, end_, cols, lo_, hi_);
            cachedCols_ = cols;
            cachedStart_ = start_;
            cachedEnd_ = end_;
            cachedPeaks_ = peaks_;
        }
        const float mid = static_cast<float>(getHeight()) / 2.0f, half = mid - 2.0f;
        for (int c = 0; c < cols; ++c) {
            const float x = track.getX() + static_cast<float>(c);
            const float top = mid - hi_[static_cast<size_t>(c)] * half;
            const float bottom = mid - lo_[static_cast<size_t>(c)] * half;
            g.setColour(x < playX ? juce::Colour(0xff4a9eff) : juce::Colour(0xff8c959c));
            g.fillRect(x, top, 1.0f, std::max(bottom - top, 1.0f));  // silence still shows as a thin line
        }
    }

    std::shared_ptr<const io::Peaks> peaks_;
    // Cache of the columns drawn: valid for this width, range and summary.
    std::shared_ptr<const io::Peaks> cachedPeaks_;
    int cachedCols_ = -1;
    int64_t cachedStart_ = 0, cachedEnd_ = 0;
    std::vector<float> lo_, hi_;
    int64_t start_ = 0, end_ = 0, position_ = 0;
    bool dragging_ = false;
    int dragX_ = 0;
    int source_ = 0;  // input source (finger / mouse) that owns the drag
};

}  // namespace

juce::String formatMinutesSeconds(int64_t frames) {
    const int64_t seconds = std::max<int64_t>(frames, 0) / static_cast<int64_t>(io::kEngineSampleRate);
    return juce::String(seconds / 60) + ":" + juce::String(seconds % 60).paddedLeft('0', 2);
}

/// One running sound: name, position bar, times, live fader and its buttons.
class RunningSounds::Row : public juce::Component {
public:
    /// @brief A row at 0 dB for one voice.
    /// @param session Gives the controller (fader, seek, fade), mode and show; must outlive the row.
    /// @param key The voice (step id) this row shows.
    /// @param onEdit The owner's Edit callback (read when tapped); must outlive the row.
    Row(ShowSession& session, VoiceKey key, const std::function<void(StepId)>& onEdit)
        : session_(session), controller_(session.controller()), key_(key) {
        // IDs only name the parts (the app tests find them); nothing in the app reads them.
        setComponentID(juce::String(key));
        name_.setComponentID("name");
        bar_.setComponentID("bar");
        fader_.setComponentID("fader");
        fade_.setComponentID("fade");
        keep_.setComponentID("keep");
        edit_.setComponentID("edit");
        // 15 px: two lines fit the name's part of a row, the second ending in an ellipsis when even that is too short.
        name_.setFont(juce::FontOptions(15.0f));
        name_.setMinimumHorizontalScale(1.0f);  // wrap rather than squeeze the letters
        tc_.setFont(juce::FontOptions(12.0f));
        tc_.setColour(juce::Label::textColourId, juce::Colour(0xffffb74a));
        remaining_.setFont(juce::FontOptions(24.0f, juce::Font::bold));
        remaining_.setJustificationType(juce::Justification::centredRight);
        elapsed_.setFont(juce::FontOptions(12.0f));
        elapsed_.setJustificationType(juce::Justification::centredRight);
        for (auto* l : {&name_, &tc_, &remaining_, &elapsed_}) {
            l->setInterceptsMouseClicks(false, false);
            addAndMakeVisible(l);
        }
        bar_.onSeek = [this](int64_t frame) {
            if (!controller_.seek(key_, static_cast<double>(frame) / io::kEngineSampleRate))
                session_.flash("Engine busy, press again");
        };
        addAndMakeVisible(bar_);
        fader_.setSliderStyle(juce::Slider::LinearHorizontal);
        fader_.setRange(kFaderInfPos, kFaderMaxDb, 0.5);
        fader_.setValue(0.0, juce::dontSendNotification);
        fader_.setDoubleClickReturnValue(true, 0.0);
        // A stray tap must not jump a running sound's level: drags move it relative.
        fader_.setSliderSnapsToMousePosition(false);
        fader_.setTextBoxStyle(juce::Slider::TextBoxRight, true, 64, 24);
        fader_.textFromValueFunction = [](double v) {
            return v < kFaderMinDb ? juce::String("-inf dB") : juce::String(v, 1) + " dB";
        };
        fader_.updateText();
        fader_.setWantsKeyboardFocus(false);
        // Only user movement sends (setValue below never notifies).
        fader_.onValueChange = [this] {
            const double v = fader_.getValue();
            send(v < kFaderMinDb ? kSilenceDb : v);
        };
        addAndMakeVisible(fader_);
        for (auto* b : {&fade_, &keep_, &edit_}) {
            b->setWantsKeyboardFocus(false);
            addChildComponent(b);
        }
        fade_.setVisible(true);
        fade_.onClick = [this] { fadeTapped(); };
        keep_.onClick = [this] {
            // The engine and the show change together (ShowSession::keepFader); the fader follows the engine's 0 dB.
            // The stored level must equal what is heard: not while a finger is on the fader or a move is unsent.
            if (pointerDown(fader_) || unsent_) {
                session_.flash("Fader still moving, press Keep again");
                return;
            }
            if (session_.keepFader(key_)) resetFader();
        };
        edit_.onClick = [&onEdit, this] {
            if (onEdit) onEdit(key_);
        };
    }

    /// @brief The voice this row shows.
    /// @return Its key (step id).
    VoiceKey key() const noexcept { return key_; }

    /// @brief Moves the fader with notification, so the trim is sent like a drag would.
    /// @param db New position in dB.
    void moveFader(double db) { fader_.setValue(db, juce::sendNotificationSync); }

    /// @brief The sound restarted: forget the fade (the new sound is not fading) and any bar drag.
    void restart() {
        fadeUntilMs_ = 0;
        bar_.cancelDrag();
    }

    /// @brief Puts the fader back to 0 dB without sending (the engine already reset the trim).
    void resetFader() {
        fader_.setValue(0.0, juce::dontSendNotification);
        unsent_.reset();  // the engine just set 0 dB; an older value must not follow
    }

    /// @brief Refreshes name, bar, times, timecode and buttons, and resends a refused trim.
    /// @param snap This tick's snapshot (timecode driver).
    /// @param v This row's voice in @p snap.
    void update(const Snapshot& snap, const VoiceState& v) {
        // A move refused by a full engine queue is retried each tick, so the
        // sound ends at the fader's last position even if the operator stopped moving.
        if (unsent_) send(*unsent_);
        // A deleted step plays on (design 8.3): the row keeps the last copy it saw,
        // so name, range and waveform stay until the sound ends.
        if (const Step* now = findStep(controller_.show(), key_)) lastStep_ = *now;
        const Step* step = lastStep_ ? &*lastStep_ : nullptr;
        name_.setText(step != nullptr ? juce::String::fromUTF8(step->name.c_str()) : juce::String("(removed step)"),
                      juce::dontSendNotification);
        // The bar spans start point to end point (the loop range for a loop). A removed
        // step has no range, so its bar is inert and the times count from the file start.
        // Read from the current show: a start/end edit while the sound plays makes the bar show
        // the new range, not the one the sound started with (accepted, documented in design 6).
        int64_t start = 0, end = 0;
        bool loop = false;
        std::shared_ptr<const io::Peaks> peaks;
        if (step != nullptr && step->sound) {
            const Sound& s = *step->sound;
            peaks = session_.peaks(s.file);
            start = secondsToFrames(s.startSeconds, io::kEngineSampleRate);
            end = session_.sources().fileLengthFrames(s.file);
            if (s.endSeconds) end = std::min(end, secondsToFrames(*s.endSeconds, io::kEngineSampleRate));
            loop = s.loop;
        }
        bar_.setRange(start, end);
        bar_.setPeaks(std::move(peaks));
        bar_.setPosition(v.positionFrame);
        elapsed_.setText(formatMinutesSeconds(v.positionFrame - start), juce::dontSendNotification);
        if (v.pending) {
            remaining_.setText("delay", juce::dontSendNotification);
        } else if (loop) {
            remaining_.setText("loop", juce::dontSendNotification);
        } else if (end > start) {
            const MinSec t = splitTime(end - v.positionFrame);
            remaining_.setText(juce::String::fromUTF8("\xe2\x88\x92") + juce::String(t.minutes) + ":" +
                                   juce::String(t.seconds).paddedLeft('0', 2),
                               juce::dontSendNotification);
        } else {
            remaining_.setText({}, juce::dontSendNotification);
        }
        // Timecode of the sound that drives it, read from the controller so the fps rules stay in one place.
        juce::String tcText;
        if (snap.timecodeKey == key_)
            if (const auto tc = controller_.currentTimecode())
                tcText = "TC " + juce::String::formatted("%02d:%02d:%02d:%02d", tc->h, tc->m, tc->s, tc->f);
        tc_.setText(tcText, juce::dontSendNotification);
        // A second tap during the fade stops at once (mirrors Fade out all), so the button says what the tap does.
        fade_.setButtonText(fadeActive() ? "Stop" : "Fade");
        // Keep only makes sense with the fader away from 0 dB (and not at the -inf stop, which cannot be stored);
        // Edit follows the mode table and needs a step to edit.
        const ShowMode mode = session_.mode();
        const double f = fader_.getValue();
        // The Keep slot depends on mode and step only, never on the fader: Keep appearing while the
        // fader is dragged must not move the fader under the finger (owner bug, 2026-11-22).
        const bool keepSlot = bigtop::app::allowed(mode, Action::KeepFader) && step != nullptr && step->sound;
        const bool showKeep = keepSlot && std::abs(f) >= 0.05 && f >= kFaderMinDb;
        const bool showEdit = step != nullptr && bigtop::app::allowed(mode, Action::EditValue);
        // Greyed out, not hidden, at 0 dB: the button stays where the finger expects it.
        keep_.setEnabled(showKeep);
        if (keepSlot != keepSlot_ || showEdit != edit_.isVisible()) {
            keepSlot_ = keepSlot;
            keep_.setVisible(keepSlot);
            edit_.setVisible(showEdit);
            resized();
        }
    }

    /// @brief Lays out name, times, bar, fader and buttons (JUCE callback).
    /// @note The widths come from rowParts(): the fader (with its dB readout) and the bar
    ///       are never dropped, Keep and Edit never hide the fader. Keep is shown
    ///       (greyed out at 0 dB) whenever it can apply, so the fader never shifts while it is dragged.
    void resized() override {
        auto r = getLocalBounds().reduced(6, 2);
        int shown = 0;
        // Buttons are as high as the row allows (>= 48 px touch targets), from the right.
        for (auto* b : {&edit_, &keep_, &fade_})
            if (b->isVisible()) {
                b->setBounds(r.removeFromRight(kRowButtonWidth).reduced(1, 0));
                ++shown;
            }
        const RowParts parts = rowParts(getWidth(), juce::jmax(1, shown));
        tc_.setVisible(parts.secondary);
        elapsed_.setVisible(parts.secondary);
        auto nameCol = r.removeFromLeft(parts.nameW);
        auto timeCol = r.removeFromLeft(parts.timeW);
        if (!parts.secondary) {  // one line each: the secondary lines are dropped
            name_.setBounds(nameCol);
            remaining_.setBounds(timeCol);
        } else {
            name_.setBounds(nameCol.removeFromTop(nameCol.getHeight() * 5 / 8));
            tc_.setBounds(nameCol);
            remaining_.setBounds(timeCol.removeFromTop(timeCol.getHeight() * 5 / 8));
            elapsed_.setBounds(timeCol);
        }
        fader_.setTextBoxStyle(juce::Slider::TextBoxRight, true, kRowFaderBox, 24);
        fader_.setBounds(r.removeFromRight(parts.faderW));
        bar_.setBounds(r.getX(), 0, juce::jmax(0, r.getWidth()), getHeight());  // the touch area spans the row height
    }

private:
    bool fadeActive() const { return uiMs() < fadeUntilMs_; }

    void fadeTapped() {
        bool ok = false;
        if (fadeActive()) {
            ok = controller_.fadeOutSound(key_, kDeclickSeconds);  // second tap: stop at once
            fadeUntilMs_ = 0;
        } else {
            const double seconds = controller_.show().settings.defaultFadeSeconds;
            ok = controller_.fadeOutSound(key_, seconds);
            if (ok) fadeUntilMs_ = uiMs() + static_cast<juce::uint32>(seconds * 1000.0);
        }
        if (!ok) session_.flash("Engine busy, press again");
    }

    void send(double db) {
        if (controller_.setLiveTrimDb(key_, db))
            unsent_.reset();
        else
            unsent_ = db;
    }

    ShowSession& session_;
    ShowController& controller_;
    VoiceKey key_;
    std::optional<double> unsent_;  // last value the engine queue refused
    juce::uint32 fadeUntilMs_ = 0;  // when this row's own fade ends (counter ms); 0 = not fading
    bool keepSlot_ = false;         // Keep could apply here (mode, sound step): shown, greyed out at 0 dB
    juce::Label name_, tc_, remaining_, elapsed_;
    std::optional<Step> lastStep_;  // the step as last seen in the show
    PositionBar bar_;
    juce::Slider fader_;
    juce::TextButton fade_{"Fade"}, keep_{"Keep"}, edit_{"Edit"};
};

RunningSounds::RunningSounds(ShowSession& session) : session_(session) {}

RunningSounds::~RunningSounds() = default;

void RunningSounds::refresh(const Snapshot& snap) {
    const auto* end = snap.voices + snap.numVoices;
    const auto voiceFor = [&](VoiceKey key) {
        return std::find_if(snap.voices, end, [key](const VoiceState& v) { return v.key == key; });
    };
    const size_t before = rows_.size();
    std::erase_if(rows_, [&](const auto& row) { return voiceFor(row->key()) == end; });
    bool added = false;
    for (const auto* v = snap.voices; v != end; ++v) {
        if (v->key >= kPadKeyBase) continue;  // pads are not step sounds
        const bool known = std::any_of(rows_.begin(), rows_.end(), [&](const auto& row) { return row->key() == v->key; });
        if (known) continue;
        auto row = std::make_unique<Row>(session_, v->key, onEdit);
        addAndMakeVisible(*row);
        rows_.insert(rows_.begin(), std::move(row));  // newest on top
        added = true;
    }
    for (auto& row : rows_) row->update(snap, *voiceFor(row->key()));
    if (added || rows_.size() != before) {
        setSize(getWidth(), rows_.empty() ? kRunningEmptyHeight : rows() * kRowHeight);
        repaint();
        resized();  // setSize skips it when only the order changed
    }
}

void RunningSounds::paint(juce::Graphics& g) {
    if (!rows_.empty()) return;
    g.setColour(juce::Colours::grey);
    g.setFont(juce::FontOptions(16.0f));
    g.drawText("Nothing playing", getLocalBounds().withTrimmedLeft(12), juce::Justification::centredLeft);
}

void RunningSounds::moveFaderForScreenshot(VoiceKey key, double db) {
    const auto it = std::find_if(rows_.begin(), rows_.end(), [key](const auto& row) { return row->key() == key; });
    if (it != rows_.end()) (*it)->moveFader(db);
}

void RunningSounds::restarted(VoiceKey key) {
    const auto it = std::find_if(rows_.begin(), rows_.end(), [key](const auto& row) { return row->key() == key; });
    if (it == rows_.end()) return;  // a new row starts at 0 dB anyway
    (*it)->resetFader();
    (*it)->restart();
    std::rotate(rows_.begin(), it, it + 1);
    resized();
}

void RunningSounds::resized() {
    int y = 0;
    for (auto& row : rows_) {
        row->setBounds(0, y, getWidth(), kRowHeight);
        y += kRowHeight;
    }
}

}  // namespace bigtop::app
