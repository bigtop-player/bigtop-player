// Pointer.cpp: the event's own source index and the real held state, or a test's overrides.
#include "Pointer.h"

namespace bigtop::app {

namespace {
int overrideIndex = -1;  // -1 = none; message thread only
const juce::Component* heldOverride = nullptr;  // nullptr = none; message thread only
}  // namespace

int pointerIndex(const juce::MouseEvent& e) { return overrideIndex >= 0 ? overrideIndex : e.source.getIndex(); }

PointerOverride::PointerOverride(int index) : previous_(overrideIndex) { overrideIndex = index; }

PointerOverride::~PointerOverride() { overrideIndex = previous_; }

bool pointerDown(const juce::Component& c) { return heldOverride != nullptr ? heldOverride == &c : c.isMouseButtonDown(); }

PointerHeld::PointerHeld(const juce::Component& c) : previous_(heldOverride) { heldOverride = &c; }

PointerHeld::~PointerHeld() { heldOverride = previous_; }

}  // namespace bigtop::app
