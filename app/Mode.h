/// @file Mode.h
/// @brief The one permission table (design 4): which action may happen in
///        which mode, plus each mode's colour and name.
///
/// Every place that used to ask "are we in Show mode?" asks allowed() instead,
/// so the table below is the only thing to change if a cell is wrong. All of
/// it is constexpr and each cell is pinned by a static_assert, because the app
/// has no test target.
#pragma once

#include <juce_core/juce_core.h>

#include "bigtop/io/ShowFolder.h"

namespace bigtop::app {

using io::ShowMode;

/// @brief Something the operator can try to do; one row (or part of a row) of design table 4.
enum class Action {
    Go,             ///< @brief GO.
    FadeOutAll,     ///< @brief Fade out (Esc).
    Fader,          ///< @brief A live fader of a running sound.
    FadeSound,      ///< @brief Fading one running sound out.
    SelectNext,     ///< @brief Choosing the next step.
    Seek,           ///< @brief Seeking on the position bar.
    Pause,          ///< @brief Pause / resume all.
    EditValue,      ///< @brief Level, time, fade-in, delay, trim, pad level.
    KeepFader,      ///< @brief "Keep": a live fader into the step level.
    EditStructure,  ///< @brief Add fade steps, change kind or target, loop, start/end, timecode, name, note, pad grid.
    DropFiles,      ///< @brief Dropping audio files to add steps.
    Reorder,        ///< @brief Moving a step.
    Delete,         ///< @brief Deleting a step.
    AssignPad,      ///< @brief Assigning a pad.
    ShowSettings,   ///< @brief Show settings.
    AudioSettings,  ///< @brief The audio device settings.
    SwitchShow      ///< @brief "Shows...": opening another show.
    // SwitchShow must stay last: ModeTests' "covers every Action" loop counts 0..SwitchShow.
};

/// @brief Whether an action may happen in a mode.
/// @param mode The current mode.
/// @param action What is attempted.
/// @return True when design table 4 allows it. Pause is false in Show mode
///         because its button is hidden there, not because it would refuse.
constexpr bool allowed(ShowMode mode, Action action) noexcept {
    switch (action) {
        // Playing the show: always available.
        case Action::Go:
        case Action::FadeOutAll:
        case Action::Fader:
        case Action::FadeSound:
        case Action::SelectNext:
        case Action::Seek: return true;
        // Hidden in Show mode (design 4).
        case Action::Pause: return mode != ShowMode::Show;
        // Levels and times: Edit and Rehearsal.
        case Action::EditValue:
        case Action::KeepFader: return mode != ShowMode::Show;
        // Structure, files, settings: Edit only.
        case Action::EditStructure:
        case Action::DropFiles:
        case Action::Reorder:
        case Action::Delete:
        case Action::AssignPad:
        case Action::ShowSettings:
        case Action::AudioSettings:
        case Action::SwitchShow: return mode == ShowMode::Edit;
    }
    return false;  // unreachable; a new Action must be added above (-Wswitch flags it)
}

/// @brief Whether Pause (button and P) is available, with the show's "Pause in Show mode" setting.
/// @param mode The current mode.
/// @param pauseInShow The show's Settings::pauseInShow.
/// @return allowed(mode, Action::Pause), or true in Show mode when the show allows Pause there.
constexpr bool pauseAllowed(ShowMode mode, bool pauseInShow) noexcept {
    return allowed(mode, Action::Pause) || (mode == ShowMode::Show && pauseInShow);
}

/// @brief The colour of a mode's strip and of the GO button's border.
/// @param mode The mode.
/// @return Opaque ARGB: amber (Edit), blue (Rehearsal), green (Show). All are
///         light enough for dark text and clear on the dark theme.
constexpr juce::uint32 modeColour(ShowMode mode) noexcept {
    return mode == ShowMode::Show ? 0xff3ddc84u : mode == ShowMode::Rehearsal ? 0xff5ab0ffu : 0xffffb300u;
}

/// @brief The mode's name as shown in capitals on the strip.
/// @param mode The mode.
/// @return "EDIT", "REHEARSAL" or "SHOW".
constexpr const char* modeName(ShowMode mode) noexcept {
    return mode == ShowMode::Show ? "SHOW" : mode == ShowMode::Rehearsal ? "REHEARSAL" : "EDIT";
}

static_assert(modeColour(ShowMode::Edit) != modeColour(ShowMode::Rehearsal) &&
              modeColour(ShowMode::Rehearsal) != modeColour(ShowMode::Show) &&
              modeColour(ShowMode::Edit) != modeColour(ShowMode::Show));
static_assert(modeName(ShowMode::Edit)[0] == 'E' && modeName(ShowMode::Rehearsal)[0] == 'R' &&
              modeName(ShowMode::Show)[0] == 'S');


/// @cond INTERNAL
// The table, one assert per cell. They are the specification the function above has to satisfy.
#define BIGTOP_ROW(action, edit, rehearsal, show)           \
    static_assert(allowed(ShowMode::Edit, Action::action) == (edit));            \
    static_assert(allowed(ShowMode::Rehearsal, Action::action) == (rehearsal));  \
    static_assert(allowed(ShowMode::Show, Action::action) == (show))

BIGTOP_ROW(Go, true, true, true);
BIGTOP_ROW(FadeOutAll, true, true, true);
BIGTOP_ROW(Fader, true, true, true);
BIGTOP_ROW(FadeSound, true, true, true);
BIGTOP_ROW(SelectNext, true, true, true);
BIGTOP_ROW(Seek, true, true, true);
BIGTOP_ROW(Pause, true, true, false);
BIGTOP_ROW(EditValue, true, true, false);
BIGTOP_ROW(KeepFader, true, true, false);
BIGTOP_ROW(EditStructure, true, false, false);
BIGTOP_ROW(DropFiles, true, false, false);
BIGTOP_ROW(Reorder, true, false, false);
BIGTOP_ROW(Delete, true, false, false);
BIGTOP_ROW(AssignPad, true, false, false);
BIGTOP_ROW(ShowSettings, true, false, false);
BIGTOP_ROW(AudioSettings, true, false, false);
BIGTOP_ROW(SwitchShow, true, false, false);

#undef BIGTOP_ROW
/// @endcond

}  // namespace bigtop::app
