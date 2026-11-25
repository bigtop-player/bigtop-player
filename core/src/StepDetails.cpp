// StepDetails.cpp: the wording rules of the Details lines, one function per kind of line.
#include "bigtop/StepDetails.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace bigtop {

namespace {

// UTF-8 escapes: the source stays ASCII, as everywhere else in the code.
constexpr const char* kDot = " \xc2\xb7 ";      // " · " between the facts of the sound line
constexpr const char* kArrow = " \xe2\x86\x92 ";  // " → " in a Level line
constexpr const char* kMinus = "\xe2\x88\x92";   // "−", the minus the app shows everywhere
constexpr const char* kDash = "\xe2\x80\x93";    // "–" between start and end

std::string db(double v) {
    const std::string s = formatDecimal(std::abs(v));
    if (s == "0.0") return s + " dB";  // no sign on a level that rounds to 0
    return (v > 0 ? "+" : kMinus) + s + " dB";
}

std::string seconds(double v) { return formatDecimal(v) + " s"; }

// "in 2.0 s", or "immediately" for 0 s (the engine's 10 ms declick is not worth a word).
std::string within(double v) { return v > 0.0 ? "in " + seconds(v) : "immediately"; }

std::string targetName(const Show& show, StepId id) {
    const auto it = std::find_if(show.steps.begin(), show.steps.end(), [&](const Step& s) { return s.id == id; });
    if (it == show.steps.end()) return "a deleted step";  // the editor greys such a change; say what it refers to
    if (it->name.empty()) return "step " + std::to_string(it - show.steps.begin() + 1);  // the number the row shows
    return it->name;
}

DetailLine soundLine(const Show& show, const Sound& sound, bool fileMissing) {
    const auto file = std::find_if(show.files.begin(), show.files.end(), [&](const AudioFile& f) { return f.id == sound.file; });
    DetailLine line{file == show.files.end() ? "unknown file" : file->path.substr(file->path.find_last_of('/') + 1),
                    fileMissing || file == show.files.end()};
    const auto add = [&](const std::string& fact) { line.text += kDot + fact; };
    if (formatDecimal(sound.levelDb) != "0.0") add(db(sound.levelDb));
    if (sound.fadeInSeconds > 0.0) add("fade in " + seconds(sound.fadeInSeconds));
    if (sound.startDelaySeconds > 0.0) add("wait " + seconds(sound.startDelaySeconds));
    if (sound.startSeconds > 0.0 || sound.endSeconds)
        add(formatMinSec(sound.startSeconds) + kDash + (sound.endSeconds ? formatMinSec(*sound.endSeconds) : "end"));
    if (sound.loop) add("loop");
    if (const auto& tc = sound.timecodeStart) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "TC %02d:%02d:%02d:%02d", tc->h, tc->m, tc->s, tc->f);
        add(buf);
    }
    if (!sound.ontimeCue.empty()) add("OnTime \"" + sound.ontimeCue + "\"");
    return line;
}

bool names(const Step& step, StepId id) {
    return std::any_of(step.changes.begin(), step.changes.end(), [&](const Change& c) { return c.target == id; });
}

// The steps whose sounds would still run when step @p index is GO'd, walking the list in order.
// shortcut: a non-looping sound that has ended by itself still counts as running, and a GO out of
// list order is not modelled; upgrade if an everything-else line shows that the operator finds wrong.
std::vector<StepId> runningBefore(const Show& show, std::size_t index) {
    std::vector<StepId> running;
    for (std::size_t j = 0; j < index; ++j) {
        const Step& s = show.steps[j];
        // Everything else ends, except what the step names and its own new sound.
        if (s.others.kind == ChangeKind::FadeOut)
            std::erase_if(running, [&](StepId id) { return id != s.id && !names(s, id); });
        for (const Change& c : s.changes)
            if (c.kind == ChangeKind::FadeOut) std::erase(running, c.target);
        if (s.sound && std::find(running.begin(), running.end(), s.id) == running.end()) running.push_back(s.id);
    }
    return running;
}

}  // namespace

std::string formatDecimal(double v) {
    // Rounded to hundredths first, so a value that shows as zero never keeps its sign.
    const double hundredths = std::round(v * 100.0);
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.2f", hundredths == 0.0 ? 0.0 : hundredths / 100.0);
    std::string s = buf;
    if (s.back() == '0') s.pop_back();  // "2.50" -> "2.5", "3.00" -> "3.0": never a bare integer
    return s;
}

std::string formatMinSec(double seconds) {
    // Whole tenths, so the carry into the minute is exact ("%04.1f" of 59.97 would read 0:60.0).
    const long long tenths = std::llround(std::max(seconds, 0.0) * 10.0);
    char buf[48];
    std::snprintf(buf, sizeof buf, "%lld:%02lld.%lld", tenths / 600, tenths % 600 / 10, tenths % 10);
    return buf;
}

std::vector<DetailLine> describeStep(const Show& show, std::size_t index, bool fileMissing,
                                     const std::vector<StepId>* runningNow) {
    std::vector<DetailLine> lines;
    if (index >= show.steps.size()) return lines;
    const Step& step = show.steps[index];
    if (step.sound) lines.push_back(soundLine(show, *step.sound, fileMissing));
    for (const Change& c : step.changes) {
        if (c.kind == ChangeKind::Level)
            lines.push_back({targetName(show, c.target) + kArrow + db(c.levelDb) + " " + within(c.seconds), false});
        else if (c.kind == ChangeKind::FadeOut)
            lines.push_back({targetName(show, c.target) + " fades out " + within(c.seconds), false});
    }
    // Everything else: only when a sound would really be left to fade (not named, not this step's own).
    const auto running = runningNow ? *runningNow : runningBefore(show, index);
    const bool leftOver = std::any_of(running.begin(), running.end(), [&](StepId id) { return id != step.id && !names(step, id); });
    if (step.others.kind == ChangeKind::FadeOut && leftOver) lines.push_back({"everything else fades out " + within(step.others.seconds), false});
    return lines;
}

}  // namespace bigtop
