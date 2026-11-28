#pragma once
/// @file Confirm.h
/// @brief A two-button question that never blocks the message loop, so audio
///        state, timers and the meters keep running while it is open.

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace bigtop::app {

/// @brief Asks a yes/no question in an async modal window.
/// @param parent The component the window is centred over (may be null).
/// @param message The question, one sentence.
/// @param okText The button that does it (result 1).
/// @param cancelText The safe button (result 0); Return and Escape press it.
/// @param onOk Called on the message thread when @p okText is pressed; never otherwise.
/// @note The window deletes itself when dismissed. Message thread.
void confirmAsync(juce::Component* parent, const juce::String& message, const juce::String& okText,
                  const juce::String& cancelText, std::function<void()> onOk);

}  // namespace bigtop::app
