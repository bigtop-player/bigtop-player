/// @file AppSettings.h
/// @brief Per-user app settings (not part of any show): the last opened show,
///        the audio device choice, the Art-Net adapter and the step list's Details choice, kept in one XML file.
#pragma once

#include <memory>
#include <optional>

#include <juce_core/juce_core.h>

namespace bigtop::io {

/// @brief Reads and writes the settings file. Every call re-reads the file,
///        so there is no cached state to go stale; call from one thread.
class AppSettings {
public:
    /// @param settingsFile XML file; created (with its folder) on first write.
    explicit AppSettings(juce::File settingsFile);

    /// @brief The show folder opened last.
    /// @return nullopt when unset or the folder no longer exists.
    std::optional<juce::File> lastShow() const;

    /// @brief Remembers the show folder.
    /// @param dir The show folder just opened.
    void setLastShow(const juce::File& dir);

    /// @brief The saved audio device state (from AudioDeviceManager::createStateXml()).
    /// @return A copy, or null when none is saved.
    std::unique_ptr<juce::XmlElement> audioDeviceState() const;

    /// @brief Remembers the audio device state; replaces any earlier one.
    /// @param state The device state to keep.
    void setAudioDeviceState(const juce::XmlElement& state);

    /// @brief The network adapter Art-Net is sent from (machine-specific, so
    ///        kept here and not in the show).
    /// @return The adapter's IPv4 address, or "" for Default (system choice).
    juce::String artNetAdapter() const;

    /// @brief Remembers the Art-Net adapter; leaves the other settings alone.
    /// @param address Adapter IPv4 address, or "" for Default.
    void setArtNetAdapter(const juce::String& address);

    /// @brief Whether the step list shows its Details lines (a viewing
    ///        preference of this laptop, not part of the show).
    /// @return True when on; false when off or never set.
    bool stepDetails() const;

    /// @brief Remembers the Details choice; leaves the other settings alone.
    /// @param on True to show the Details lines.
    void setStepDetails(bool on);

private:
    juce::File file_;
    std::unique_ptr<juce::XmlElement> load() const;
    void store(const juce::XmlElement& root) const;
};

}  // namespace bigtop::io
