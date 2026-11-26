// ShowJson.cpp - JSON (de)serialisation of a Show using nlohmann/json.
// Layout: small typed getters first (each validates one field and reports its
// path), then per-struct helpers, then parse(), then the public toJson/fromJson.
#include "bigtop/ShowJson.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <limits>
#include <set>

#include <nlohmann/json.hpp>

#include "bigtop/Engine.h"  // kMaxChanges

namespace bigtop {
namespace {

using json = nlohmann::json;

// Every validation failure goes through here so the message always starts with
// the path of the bad field (e.g. "steps[2].sound.file: ...").
[[noreturn]] void fail(const std::string& where, const std::string& what) {
    throw ShowFormatError(where + ": " + what);
}

// Returns the member, or nullptr if absent or null. Treating null as absent
// is only for genuinely optional fields; required numbers use getNum instead.
const json* find(const json& o, const char* key) {
    auto it = o.find(key);
    return (it == o.end() || it->is_null()) ? nullptr : &*it;  // null means absent
}

void requireObject(const json& o, const std::string& where) {
    if (!o.is_object()) fail(where, "expected an object");
}

double number(const json& v, const std::string& where) {
    if (!v.is_number()) fail(where, "expected a number");
    double d = v.get<double>();
    // Overflowing literals like 1e999 parse to infinity; a non-finite level or
    // duration would poison the audio maths later, so stop it at the door.
    if (!std::isfinite(d)) fail(where, "number is not finite");
    return d;
}

// A present-but-null number is an error; an absent one takes the default.
double getNum(const json& o, const char* key, double def, const std::string& where) {
    auto it = o.find(key);
    return it == o.end() ? def : number(*it, where + "." + key);
}

std::optional<double> getOptNum(const json& o, const char* key, const std::string& where) {
    const json* v = find(o, key);
    if (!v) return std::nullopt;
    return number(*v, where + "." + key);
}

int64_t integer(const json& v, const std::string& where) {
    if (!v.is_number_integer()) fail(where, "expected an integer");
    return v.get<int64_t>();
}

int getInt(const json& o, const char* key, int def, const std::string& where) {
    auto it = o.find(key);
    if (it == o.end()) return def;
    int64_t i = integer(*it, where + "." + key);
    if (i < std::numeric_limits<int>::min() || i > std::numeric_limits<int>::max())
        fail(where + "." + key, "out of range");
    return static_cast<int>(i);
}

// Required unsigned 32-bit id. Range-checked in 64 bits so that -1 or 2^32
// are rejected instead of wrapping around into a valid-looking id.
uint32_t getId(const json& o, const char* key, const std::string& where) {
    auto it = o.find(key);
    if (it == o.end()) fail(where, std::string("missing ") + key);
    int64_t i = integer(*it, where + "." + key);
    if (i < 0 || i > std::numeric_limits<uint32_t>::max()) fail(where + "." + key, "out of range");
    return static_cast<uint32_t>(i);
}

std::string getStr(const json& o, const char* key, const std::string& where, bool required = false) {
    auto it = o.find(key);
    if (it == o.end()) {
        if (required) fail(where, std::string("missing ") + key);
        return {};
    }
    if (!it->is_string()) fail(where + "." + key, "expected a string");
    return it->get<std::string>();
}

bool getBool(const json& o, const char* key, const std::string& where) {
    auto it = o.find(key);
    if (it == o.end()) return false;
    if (!it->is_boolean()) fail(where + "." + key, "expected true or false");
    return it->get<bool>();
}

// Enum <-> string mapping. Strings (not numbers) keep files readable and
// robust against reordering the enum.
const char* kindName(ChangeKind k) {
    switch (k) {
        case ChangeKind::Keep: return "keep";
        case ChangeKind::Level: return "level";
        case ChangeKind::FadeOut: return "fadeOut";
    }
    return "keep";
}

ChangeKind getKind(const json& o, const std::string& where) {
    std::string s = getStr(o, "kind", where, true);
    for (ChangeKind k : {ChangeKind::Keep, ChangeKind::Level, ChangeKind::FadeOut})
        if (s == kindName(k)) return k;
    fail(where + ".kind", "unknown kind");
}

std::string timecodeText(const Timecode& t) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%02d:%02d:%02d:%02d", t.h, t.m, t.s, t.f);
    return buf;
}

// Parses "HH:MM:SS:FF". Uses from_chars (no locale, no exceptions) and demands
// exactly four non-negative numbers separated by ':' with nothing trailing.
Timecode parseTimecode(const std::string& s, const std::string& where) {
    int parts[4] = {};
    const char* p = s.data();
    const char* end = p + s.size();
    for (int i = 0; i < 4; ++i) {
        auto [next, ec] = std::from_chars(p, end, parts[i]);
        if (ec != std::errc{} || parts[i] < 0) fail(where, "expected HH:MM:SS:FF");
        p = next;
        if (i < 3 && (p == end || *p++ != ':')) fail(where, "expected HH:MM:SS:FF");
    }
    if (p != end) fail(where, "expected HH:MM:SS:FF");
    return {parts[0], parts[1], parts[2], parts[3]};
}

json toJsonValue(const Sound& s) {
    json j{{"file", s.file}, {"levelDb", s.levelDb}, {"fadeInSeconds", s.fadeInSeconds},
           {"startSeconds", s.startSeconds}, {"loop", s.loop},
           {"startDelaySeconds", s.startDelaySeconds}, {"ontimeCue", s.ontimeCue}};
    if (s.endSeconds) j["endSeconds"] = *s.endSeconds;
    if (s.timecodeStart) j["timecodeStart"] = timecodeText(*s.timecodeStart);
    return j;
}

Sound soundFrom(const json& j, const std::string& where) {
    requireObject(j, where);
    Sound s;
    s.file = getId(j, "file", where);
    s.levelDb = getNum(j, "levelDb", s.levelDb, where);
    s.fadeInSeconds = getNum(j, "fadeInSeconds", s.fadeInSeconds, where);
    s.startSeconds = getNum(j, "startSeconds", s.startSeconds, where);
    s.endSeconds = getOptNum(j, "endSeconds", where);
    s.loop = getBool(j, "loop", where);
    s.startDelaySeconds = getNum(j, "startDelaySeconds", s.startDelaySeconds, where);
    if (const json* tc = find(j, "timecodeStart")) {
        if (!tc->is_string()) fail(where + ".timecodeStart", "expected a string");
        s.timecodeStart = parseTimecode(tc->get<std::string>(), where + ".timecodeStart");
    }
    s.ontimeCue = getStr(j, "ontimeCue", where);
    return s;
}

json toJsonValue(const Step& st) {
    json changes = json::array();
    for (const Change& c : st.changes)
        changes.push_back({{"target", c.target}, {"kind", kindName(c.kind)},
                           {"levelDb", c.levelDb}, {"seconds", c.seconds}});
    json j{{"id", st.id}, {"name", st.name}, {"note", st.note}, {"changes", changes},
           {"others", {{"kind", kindName(st.others.kind)}, {"seconds", st.others.seconds}}}};
    if (st.sound) j["sound"] = toJsonValue(*st.sound);
    return j;
}

Step stepFrom(const json& j, const std::string& where) {
    requireObject(j, where);
    Step st;
    st.id = getId(j, "id", where);
    if (st.id == 0) fail(where + ".id", "step id must not be 0");
    // The upper half is the engine's pad key range (see kMaxStepId).
    if (st.id > kMaxStepId) fail(where + ".id", "out of range");
    st.name = getStr(j, "name", where);
    st.note = getStr(j, "note", where);
    if (const json* s = find(j, "sound")) st.sound = soundFrom(*s, where + ".sound");
    if (const json* cs = find(j, "changes")) {
        if (!cs->is_array()) fail(where + ".changes", "expected an array");
        // The engine command carries at most kMaxChanges; reject rather than drop silently.
        if (cs->size() > static_cast<size_t>(kMaxChanges)) fail(where + ".changes", "too many changes (max " + std::to_string(kMaxChanges) + ")");
        for (size_t i = 0; i < cs->size(); ++i) {
            const json& c = (*cs)[i];
            std::string w = where + ".changes[" + std::to_string(i) + "]";
            requireObject(c, w);
            Change ch;
            ch.target = getId(c, "target", w);
            if (ch.target > kMaxStepId) fail(w + ".target", "out of range");
            ch.kind = getKind(c, w);
            ch.levelDb = getNum(c, "levelDb", ch.levelDb, w);
            ch.seconds = getNum(c, "seconds", ch.seconds, w);
            st.changes.push_back(ch);
        }
    }
    if (const json* o = find(j, "others")) {
        std::string w = where + ".others";
        requireObject(*o, w);
        st.others.kind = getKind(*o, w);
        // "Others" has no single level to move to, so only keep/fadeOut make sense.
        if (st.others.kind == ChangeKind::Level) fail(w + ".kind", "only keep or fadeOut allowed");
        st.others.seconds = getNum(*o, "seconds", st.others.seconds, w);
    }
    return st;
}

const json& arrayAt(const json& root, const char* key, const json& empty) {
    const json* v = find(root, key);
    if (!v) return empty;
    if (!v->is_array()) fail(key, "expected an array");
    return *v;
}

// Builds a Show from an already-parsed document, validating as it goes.
// Unknown keys are never looked at, which is what makes them ignored.
Show parse(const json& root) {
    requireObject(root, "show");
    const json* ver = find(root, "formatVersion");
    if (!ver || integer(*ver, "formatVersion") != kFormatVersion)
        fail("formatVersion", "missing or unsupported (expected 1)");
    Show show;

    if (const json* st = find(root, "settings")) {
        requireObject(*st, "settings");
        Settings& s = show.settings;
        s.defaultFadeSeconds = getNum(*st, "defaultFadeSeconds", s.defaultFadeSeconds, "settings");
        s.goGuardSeconds = getNum(*st, "goGuardSeconds", s.goGuardSeconds, "settings");
        s.pauseInShow = getBool(*st, "pauseInShow", "settings");  // missing: false, as before the setting existed
        s.timecodeFps = getInt(*st, "timecodeFps", s.timecodeFps, "settings");
        // Later code divides by the frame rate, so 0 or nonsense must not load.
        if (s.timecodeFps != 24 && s.timecodeFps != 25 && s.timecodeFps != 30)
            fail("settings.timecodeFps", "must be 24, 25 or 30");
        s.loudnessTargetLufs = getNum(*st, "loudnessTargetLufs", s.loudnessTargetLufs, "settings");
        if (find(*st, "artnetTarget")) s.artnetTarget = getStr(*st, "artnetTarget", "settings");
        s.ontimeHost = getStr(*st, "ontimeHost", "settings");
        s.ontimePort = getInt(*st, "ontimePort", s.ontimePort, "settings");
        s.padRows = getInt(*st, "padRows", s.padRows, "settings");
        s.padCols = getInt(*st, "padCols", s.padCols, "settings");
        // The UI lays pads out from these, so out-of-range values must not load.
        if (s.padRows < kMinPadRows || s.padRows > kMaxPadRows) fail("settings.padRows", "must be 1..6");
        if (s.padCols < kMinPadCols || s.padCols > kMaxPadCols) fail("settings.padCols", "must be 1..6");
        if (s.padRows * s.padCols > kMaxPads) fail("settings", "padRows * padCols must not exceed 24");
    }

    const json empty = json::array();
    std::set<FileId> fileIds;
    const json& files = arrayAt(root, "files", empty);
    for (size_t i = 0; i < files.size(); ++i) {
        std::string w = "files[" + std::to_string(i) + "]";
        requireObject(files[i], w);
        AudioFile f;
        f.id = getId(files[i], "id", w);
        f.path = getStr(files[i], "path", w, true);
        f.trimDb = getNum(files[i], "trimDb", f.trimDb, w);
        f.sourceHash = getStr(files[i], "sourceHash", w);
        f.importVersion = getInt(files[i], "importVersion", f.importVersion, w);
        f.sourcePath = getStr(files[i], "sourcePath", w);
        // Duplicate ids would make Sound::file ambiguous.
        if (!fileIds.insert(f.id).second) fail(w + ".id", "duplicate file id");
        show.files.push_back(std::move(f));
    }

    std::set<StepId> stepIds;
    const json& steps = arrayAt(root, "steps", empty);
    for (size_t i = 0; i < steps.size(); ++i) {
        std::string w = "steps[" + std::to_string(i) + "]";
        Step st = stepFrom(steps[i], w);
        // Step ids identify running sounds, so a duplicate would make Changes ambiguous.
        if (!stepIds.insert(st.id).second) fail(w + ".id", "duplicate step id");
        show.steps.push_back(std::move(st));
    }
    // Optional; never lower than an existing id, so a stale value cannot make
    // the editor hand out an id that is already in use.
    if (find(root, "lastStepId")) {
        show.lastStepId = getId(root, "lastStepId", "show");
        // Above kMaxStepId the next allocated id would reach the pad key range.
        if (show.lastStepId > kMaxStepId) fail("show.lastStepId", "out of range");
    }
    for (const Step& st : show.steps) show.lastStepId = std::max(show.lastStepId, st.id);

    const json& pads = arrayAt(root, "pads", empty);
    if (pads.size() > static_cast<size_t>(kMaxPads)) fail("pads", "too many pads");
    // An absent or short array keeps the 8 default empty pads; a longer one grows the vector.
    if (pads.size() > show.pads.size()) show.pads.resize(pads.size());
    for (size_t i = 0; i < pads.size(); ++i) {
        std::string w = "pads[" + std::to_string(i) + "]";
        requireObject(pads[i], w);
        Pad& p = show.pads[i];
        p.name = getStr(pads[i], "name", w);
        if (find(pads[i], "file")) p.file = getId(pads[i], "file", w);
        p.levelDb = getNum(pads[i], "levelDb", p.levelDb, w);
        p.startSeconds = getNum(pads[i], "startSeconds", p.startSeconds, w);  // missing: the whole file, as before
        p.endSeconds = getOptNum(pads[i], "endSeconds", w);
    }
    return show;
}

}  // namespace

std::string toJson(const Show& show) {
    // All pads are always written (unassigned ones just omit "file") so the
    // array index is the pad number. Optional sound/file members are omitted
    // rather than written as null, matching how fromJson reads them.
    json files = json::array(), steps = json::array(), pads = json::array();
    for (const AudioFile& f : show.files) {
        json j{{"id", f.id}, {"path", f.path}, {"trimDb", f.trimDb}, {"sourceHash", f.sourceHash},
               {"importVersion", f.importVersion}};
        if (!f.sourcePath.empty()) j["sourcePath"] = f.sourcePath;  // optional: older builds never wrote it
        files.push_back(std::move(j));
    }
    for (const Step& s : show.steps) steps.push_back(toJsonValue(s));
    for (const Pad& p : show.pads) {
        json j{{"name", p.name}, {"levelDb", p.levelDb}, {"startSeconds", p.startSeconds}};
        if (p.file) j["file"] = *p.file;
        if (p.endSeconds) j["endSeconds"] = *p.endSeconds;
        pads.push_back(j);
    }
    const Settings& s = show.settings;
    json root{{"formatVersion", show.formatVersion},
              {"settings", {{"defaultFadeSeconds", s.defaultFadeSeconds}, {"goGuardSeconds", s.goGuardSeconds},
                            {"pauseInShow", s.pauseInShow},
                            {"timecodeFps", s.timecodeFps}, {"loudnessTargetLufs", s.loudnessTargetLufs},
                            {"artnetTarget", s.artnetTarget}, {"ontimeHost", s.ontimeHost},
                            {"ontimePort", s.ontimePort},
                            {"padRows", s.padRows}, {"padCols", s.padCols}}},
              {"files", files}, {"steps", steps}, {"pads", pads}, {"lastStepId", show.lastStepId}};
    return root.dump(2);
}

Show fromJson(std::string_view text) {
    // allow_exceptions=false: a corrupt file is an expected situation, not an
    // exceptional one, and we want our own error type either way.
    json root = json::parse(text.begin(), text.end(), nullptr, false);
    if (root.is_discarded()) throw ShowFormatError("show file is not valid JSON");
    // Callers (and the UI) should only have to know ShowFormatError, never
    // nlohmann's exception types. Our own checks already throw it; the catch
    // below is a net for any type error from nlohmann we did not check for.
    try {
        return parse(root);
    } catch (const json::exception& e) {
        throw ShowFormatError(std::string("bad show file: ") + e.what());
    }
}

}  // namespace bigtop
