// ShowEdits.cpp: the show edits (add dropped files, remove and move steps) as pure functions on a Show.
#include "bigtop/ShowEdits.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iterator>

#include "bigtop/Engine.h"  // kMaxChanges

namespace bigtop {

namespace {
// Position of a step by id, or end().
std::vector<Step>::iterator findById(Show& show, StepId id) {
    return std::find_if(show.steps.begin(), show.steps.end(), [id](const Step& s) { return s.id == id; });
}
}  // namespace

std::optional<StepId> allocateStepId(Show& show) noexcept {
    uint64_t highest = show.lastStepId;
    for (const Step& s : show.steps) highest = std::max<uint64_t>(highest, s.id);
    // Wider than StepId so the +1 cannot wrap before the range check.
    if (highest + 1 > kMaxStepId) return std::nullopt;
    show.lastStepId = static_cast<StepId>(highest + 1);
    return show.lastStepId;
}

std::optional<FileId> findFileByHash(const Show& show, std::string_view sourceHash, int minImportVersion) noexcept {
    if (sourceHash.empty()) return std::nullopt;
    for (const AudioFile& f : show.files)
        if (f.sourceHash == sourceHash && f.importVersion >= minImportVersion) return f.id;
    return std::nullopt;
}

std::optional<double> checkedEdit(double value, double lo, double hi) noexcept {
    if (!std::isfinite(value)) return std::nullopt;
    return std::clamp(value, lo, hi);
}

bool setChange(Step& step, Change change) {
    if (change.target == 0 || change.target > kMaxStepId) return false;
    if (!std::isfinite(change.levelDb) || !std::isfinite(change.seconds)) return false;
    const auto it = std::find_if(step.changes.begin(), step.changes.end(),
                                 [&](const Change& c) { return c.target == change.target; });
    if (it != step.changes.end()) {
        *it = change;
        return true;
    }
    if (step.changes.size() >= static_cast<size_t>(kMaxChanges)) return false;  // the engine command holds this many
    step.changes.push_back(change);
    return true;
}

void removeChange(Step& step, StepId target) {
    std::erase_if(step.changes, [target](const Change& c) { return c.target == target; });
}

void resizePadGrid(Show& show, int rows, int cols) {
    rows = std::clamp(rows, kMinPadRows, kMaxPadRows);
    cols = std::clamp(cols, kMinPadCols, kMaxPadCols);
    while (rows * cols > kMaxPads) --cols;  // keep the row count the operator asked for
    show.settings.padRows = rows;
    show.settings.padCols = cols;
    if (show.pads.size() < static_cast<size_t>(rows * cols)) show.pads.resize(static_cast<size_t>(rows * cols));
}

double keptLevelDb(double levelDb, double liveTrimDb) noexcept {
    if (!std::isfinite(liveTrimDb)) return levelDb;
    return std::clamp(levelDb + liveTrimDb, kEditLevelMinDb, kEditLevelMaxDb);
}

bool keepIntoShow(Show& show, StepId sound, StepId setter, double trimDb) noexcept {
    if (!std::isfinite(trimDb)) return false;
    auto& steps = show.steps;
    const auto start = std::find_if(steps.begin(), steps.end(), [sound](const Step& s) { return s.id == sound; });
    if (start == steps.end() || !start->sound) return false;
    if (setter != 0 && setter != sound) {
        const auto it = std::find_if(steps.begin(), steps.end(), [setter](const Step& s) { return s.id == setter; });
        if (it != steps.end())
            for (Change& ch : it->changes)
                if (ch.target == sound && ch.kind == ChangeKind::Level) {
                    ch.levelDb = keptLevelDb(ch.levelDb, trimDb);
                    return true;
                }
    }
    // Setter is the start itself, unknown, deleted or lost its change: the start level is the safest slot.
    start->sound->levelDb = keptLevelDb(start->sound->levelDb, trimDb);
    return true;
}

FileId addFile(Show& show, std::string path, std::string sourceHash, int importVersion) {
    FileId id = 0;
    for (const AudioFile& f : show.files) id = std::max(id, f.id);
    ++id;
    show.files.push_back({id, std::move(path), 0.0, std::move(sourceHash), importVersion});
    return id;
}

void insertSteps(Show& show, int index, std::vector<Step> steps) {
    const int at = std::clamp(index, 0, static_cast<int>(show.steps.size()));
    show.steps.insert(show.steps.begin() + at, std::make_move_iterator(steps.begin()),
                      std::make_move_iterator(steps.end()));
}

bool removeStep(Show& show, StepId id) {
    const auto it = findById(show, id);
    if (it == show.steps.end()) return false;
    show.steps.erase(it);
    return true;
}

bool moveStep(Show& show, StepId id, int newIndex) {
    const auto it = findById(show, id);
    if (it == show.steps.end()) return false;
    Step moved = std::move(*it);
    show.steps.erase(it);
    // Clamp against the list without the moved step, so newIndex is the final position.
    const int at = std::clamp(newIndex, 0, static_cast<int>(show.steps.size()));
    show.steps.insert(show.steps.begin() + at, std::move(moved));
    return true;
}

std::vector<DroppedStep> addDroppedFiles(Show& show, int index, const std::vector<DroppedFile>& files,
                                         int importVersion, const std::function<std::string(const DroppedFile&, const Show&)>& newPath) {
    std::vector<DroppedStep> out(files.size());
    std::vector<Step> steps;
    for (size_t i = 0; i < files.size(); ++i) {
        // The id first: once ids run out, no file entry may be left without a step.
        const auto id = allocateStepId(show);
        if (!id) break;  // ids only grow, so every later file fails too
        const DroppedFile& d = files[i];
        DroppedStep& o = out[i];
        o.step = *id;
        // addFile() records the hash, so a repeat later in this drop finds it here.
        if (const auto known = findFileByHash(show, d.sourceHash, importVersion)) {
            o.file = *known;
        } else {
            o.file = addFile(show, newPath(d, show), d.sourceHash, importVersion);
            o.newFile = true;
        }
        Step step = makeStep(*id, show.settings, Sound{.file = o.file, .levelDb = show.settings.newStepLevelDb});
        step.name = d.name;
        steps.push_back(std::move(step));
    }
    insertSteps(show, index, std::move(steps));
    return out;
}

}  // namespace bigtop
