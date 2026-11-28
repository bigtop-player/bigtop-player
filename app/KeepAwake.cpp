// KeepAwake.cpp: see KeepAwake.h.
#include "KeepAwake.h"

#include <juce_gui_basics/juce_gui_basics.h>

#if JUCE_WINDOWS
#include <windows.h>
#endif

namespace bigtop::app {

void setKeepAwake(bool on) {
#if JUCE_WINDOWS
    // JUCE's screensaver switch only nudges the mouse; this also stops system sleep.
    SetThreadExecutionState(on ? (ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED) : ES_CONTINUOUS);
#else
    juce::Desktop::setScreenSaverEnabled(!on);
#endif
}

}  // namespace bigtop::app
