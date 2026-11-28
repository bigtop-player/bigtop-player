#pragma once
/// @file KeepAwake.h
/// @brief Keeps the computer and its display awake during a show, so sleep or
///        a screensaver never interrupts it.

namespace bigtop::app {

/// @brief Turns the keep-awake request on or off.
/// @param on True: the system and display must not sleep; false: normal power settings.
/// @note Windows: SetThreadExecutionState from the message thread, which holds
///       until cleared. Elsewhere: JUCE's screensaver switch. Message thread.
void setKeepAwake(bool on);

}  // namespace bigtop::app
