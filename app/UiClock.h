/// @file UiClock.h
/// @brief The one clock the UI reads (hold-to-leave, fades, flashes, the 4 Hz tick, GO times), so
///        tests can freeze and step it.
///
/// A seam only: it chooses where the time comes from, never what the app does with it. The app
/// never sets a ManualClock, so it always reads JUCE's millisecond counters. Save, backup and
/// archive names keep using juce::Time::getCurrentTime() (wall clock, not this).
#pragma once

#include <juce_core/juce_core.h>

namespace bigtop::app {

/// @brief The UI's millisecond counter.
/// @return juce::Time::getMillisecondCounter(), or the ManualClock's now() while one exists.
/// @note Message thread. Wraps like JUCE's counter (about every 49 days): compare differences.
juce::uint32 uiMs();

/// @brief The UI's high-resolution time in seconds (what GO's guard and start times use).
/// @return juce::Time::getMillisecondCounterHiRes() / 1000, or the ManualClock's now() / 1000.
/// @note Message thread.
double uiSeconds();

/// @brief Tests only: replaces the UI clock with one that moves only when told, for its lifetime.
/// @note One at a time: constructing a second one while one exists prints a message to stderr
///       and ends the process with exit code 4 (std::_Exit, no dialog). The destructor restores
///       the real clock, so a test can never leave frozen time to the next. Message thread.
class ManualClock {
public:
    /// @brief Takes over the UI clock.
    /// @param startMs What uiMs() returns until advance() is called.
    explicit ManualClock(juce::uint32 startMs = 1000);

    /// @brief Gives the UI clock back to the real counters.
    ~ManualClock();

    ManualClock(const ManualClock&) = delete;
    ManualClock& operator=(const ManualClock&) = delete;

    /// @brief Moves the time forward.
    /// @param ms Milliseconds to add (wraps like the real counter).
    void advance(juce::uint32 ms);

    /// @brief The current manual time.
    /// @return Milliseconds, as uiMs() returns them.
    juce::uint32 now() const;

private:
    juce::uint32 now_;
};

}  // namespace bigtop::app
