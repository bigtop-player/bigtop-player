// UiClock.cpp: the real counters, or the one ManualClock a test has set.
#include "UiClock.h"

#include <cstdio>
#include <cstdlib>

namespace bigtop::app {

namespace {
const ManualClock* manual = nullptr;  // message thread only, like every reader
}  // namespace

juce::uint32 uiMs() { return manual != nullptr ? manual->now() : juce::Time::getMillisecondCounter(); }

double uiSeconds() {
    return manual != nullptr ? manual->now() / 1000.0 : juce::Time::getMillisecondCounterHiRes() / 1000.0;
}

ManualClock::ManualClock(juce::uint32 startMs) : now_(startMs) {
    // One at a time: a nested clock would take over and its destructor would hand the first one's
    // test the real clock. jassert only logs without a debugger, so end the exe loudly instead.
    if (manual != nullptr) {
        std::fputs("ManualClock: a second ManualClock while one is active; ending with exit code 4\n", stderr);
        std::fflush(stderr);
        std::_Exit(4);  // no dialog, no unwinding into a test that already lost its clock
    }
    manual = this;
}

ManualClock::~ManualClock() { manual = nullptr; }

void ManualClock::advance(juce::uint32 ms) { now_ += ms; }

juce::uint32 ManualClock::now() const { return now_; }

}  // namespace bigtop::app
