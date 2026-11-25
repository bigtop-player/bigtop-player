/// @file Pointer.h
/// @brief Which finger a mouse event came from and whether a press is still held, with seams so
///        tests can play several fingers and a finger that stays down.
///
/// JUCE's main mouse source always has index 0, and tests cannot create touch sources, so the
/// components ask pointerIndex() instead of e.source.getIndex(). Synthetic test events never
/// move JUCE's mouse sources either, so a held press is asked through pointerDown() instead of
/// Component::isMouseButtonDown(). The app never sets an override.
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace bigtop::app {

/// @brief The finger (input source index) of a mouse event.
/// @param e The event.
/// @return The PointerOverride's index while one exists, else e.source.getIndex().
/// @note Message thread.
int pointerIndex(const juce::MouseEvent& e);

/// @brief Tests only: makes pointerIndex() return @p index for this object's lifetime.
/// @note Nests: the destructor restores the previous value. Message thread.
class PointerOverride {
public:
    /// @brief Starts the override.
    /// @param index The finger index every event reports (0 = the first finger).
    explicit PointerOverride(int index);

    /// @brief Restores the previous override (or none).
    ~PointerOverride();

    PointerOverride(const PointerOverride&) = delete;
    PointerOverride& operator=(const PointerOverride&) = delete;

private:
    int previous_;
};

/// @brief Whether a mouse button or finger is down on a component right now.
/// @param c The component.
/// @return While a PointerHeld exists, whether it holds @p c; else c.isMouseButtonDown().
/// @note Message thread.
bool pointerDown(const juce::Component& c);

/// @brief Tests only: makes pointerDown() report a press held on one component (and no other)
///        for this object's lifetime.
/// @note Nests: the destructor restores the previous value. Ending it without a release is a
///       release that never came (window deactivated, capture lost). Message thread.
class PointerHeld {
public:
    /// @brief Starts the override.
    /// @param c The component the press stays down on; must outlive this object.
    explicit PointerHeld(const juce::Component& c);

    /// @brief Restores the previous override (or none).
    ~PointerHeld();

    PointerHeld(const PointerHeld&) = delete;
    PointerHeld& operator=(const PointerHeld&) = delete;

private:
    const juce::Component* previous_;
};

}  // namespace bigtop::app
