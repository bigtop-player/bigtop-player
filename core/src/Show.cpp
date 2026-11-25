#include "bigtop/Show.h"

#include <utility>

namespace bigtop {

// A step that starts a sound fades everything else out by default (the usual
// "next song replaces the last"); one that starts nothing leaves things alone.
Step makeStep(StepId id, const Settings& settings, std::optional<Sound> sound) {
    Step step;
    step.id = id;
    step.others = sound ? Others{ChangeKind::FadeOut, settings.defaultFadeSeconds} : Others{};
    step.sound = std::move(sound);
    return step;
}

}  // namespace bigtop
