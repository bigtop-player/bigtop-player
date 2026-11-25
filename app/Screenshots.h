/// @file Screenshots.h
/// @brief The `--screenshots <out-dir>` start option: renders the app's main
///        states to PNG files, so the GUI can be checked without driving the
///        native window.
///
/// A demo show (30 steps, real short FLAC tones, a fade step with changes,
/// pads) is generated in a temporary folder; a MainComponent over it is put
/// through fixed states with the same private actions the buttons call, and
/// each state is written as `<nn>-<state>-<width>.png` at 1024x768 and
/// 1366x768. No window opens, no audio device plays, and the owner's settings
/// and last show are never touched (a temporary settings file is used).
#pragma once

#include <juce_core/juce_core.h>

namespace bigtop::app {

/// @brief Renders every screenshot state at both widths into @p outDir.
/// @param outDir Folder for the PNGs; created when missing.
/// @return 0 when every PNG was written; non-zero on any failure (the reason
///         goes to stderr). Message thread, blocking (a few seconds).
/// @note The engine is driven here with silent blocks, so the running area
///       shows sounds without any audio device.
int renderScreenshots(const juce::File& outDir);

/// @brief Renders the curated documentation set into @p outDir (`--docs-screenshots <dir>`).
/// @param outDir Folder for the PNGs (normally `docs/images`); created when missing.
/// @return 0 when every PNG was written; non-zero on any failure (reason on stderr).
/// @note Each state is laid out at 1920x1080, the size users get, then written at
///       1280x720 under a stable name (`edit-mode.png`, `running-sounds.png`, ...),
///       so the README and wiki links never change. Same invented demo show and
///       adapters as renderScreenshots(); nothing of the machine is shown.
int renderDocsScreenshots(const juce::File& outDir);

}  // namespace bigtop::app
