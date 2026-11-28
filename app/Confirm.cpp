// Confirm.cpp: see Confirm.h.
#include "Confirm.h"

namespace bigtop::app {

void confirmAsync(juce::Component* parent, const juce::String& message, const juce::String& okText,
                  const juce::String& cancelText, std::function<void()> onOk) {
    auto* w = new juce::AlertWindow("Bigtop Player", message, juce::MessageBoxIconType::QuestionIcon, parent);
    // Cancel takes both keys: a stray Return or Escape must never confirm.
    w->addButton(cancelText, 0, juce::KeyPress(juce::KeyPress::escapeKey), juce::KeyPress(juce::KeyPress::returnKey));
    w->addButton(okText, 1);
    w->setHelpText(message);  // the question, readable for screen readers and tests
    w->enterModalState(true, juce::ModalCallbackFunction::create([ok = std::move(onOk)](int result) {
                           if (result == 1 && ok) ok();
                       }),
                       true);
}

}  // namespace bigtop::app
