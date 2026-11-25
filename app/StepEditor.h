/// @file StepEditor.h
/// @brief The side panel that edits one step: name and note, the sound's
///        level, fades, delay, loop, start / end, timecode start and file trim,
///        the changes to sounds expected to be running, "everything else", and
///        Delete step (design 6).
///
/// Edit mode shows every field editable. Rehearsal mode shows the same panel
/// with only the value fields (design table 4: levels and times) editable and
/// the rest greyed out. In Show mode it never opens. Every field commit is one
/// ShowSession::edit() (one verified save); running sounds are never touched.
#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

#include "NumberField.h"
#include "ShowSession.h"

namespace bigtop::app {

/// @brief The step editor panel.
/// @note Message thread. It follows the step it was opened for by id, not NEXT,
///       and never changes NEXT.
class StepEditor : public juce::Component {
public:
    /// @brief Builds a closed (hidden) panel.
    /// @param session The session whose open show is edited; must outlive this.
    explicit StepEditor(ShowSession& session);

    /// @brief Destroys the fields.
    ~StepEditor() override;

    /// @brief Shows the panel for a step.
    /// @param id The step's id. Does nothing to NEXT.
    void open(StepId id);

    /// @brief Brings the panel up to date; call from the 30 Hz UI timer.
    /// @note Rebuilds the fields when the layout changed (mode, the sounds
    ///       expected here, a change's kind, ...), otherwise only refreshes
    ///       values that are not being typed. Closes (onClose) when the step
    ///       is gone, e.g. deleted. Cheap when nothing changed.
    void refresh();

    /// @brief Called when the panel wants to go away: Close was tapped, the step was deleted.
    std::function<void()> onClose;

    /// @brief Paints the panel background (JUCE callback).
    /// @param g Graphics context.
    void paint(juce::Graphics& g) override;
    /// @brief Lays out the title bar and the scrolling fields (JUCE callback).
    void resized() override;

private:
    /// App test hook (app/tests/AppTestSupport.h): finds a field by its label.
    friend struct AppTestAccess;

    struct Spec;
    const Step* step() const;
    int stepIndex() const;
    juce::String signature() const;
    void rebuild(const juce::String& signature);
    void edit(Action action, const std::function<bool(Show&, Step&)>& change);
    void addNumber(const juce::String& label, const Spec& spec, int x = 0, int labelWidth = 120, int fieldWidth = 220);
    void addChangeRow(StepId target, int row);
    void radioRow(const std::vector<std::pair<juce::String, int>>& items, int selected, int group, bool enabled,
                  const std::function<void(int)>& pick, juce::TextButton** first = nullptr);
    juce::Label& labelAt(const juce::String& text, int x, int width, juce::Colour colour = juce::Colours::white);
    void pullAll(bool force);

    ShowSession& session_;
    StepId id_ = 0;
    juce::Label title_;
    juce::TextButton close_{"Close"};
    juce::Viewport view_;
    juce::Component content_;
    std::vector<std::unique_ptr<juce::Component>> owned_;       // every field of the current build
    std::vector<std::function<void(bool)>> pulls_;              // re-read one field from the model; bool = even if being typed
    juce::String builtSignature_;
    int builtWidth_ = -1;
    int y_ = 0;  // layout cursor while building
};

}  // namespace bigtop::app
