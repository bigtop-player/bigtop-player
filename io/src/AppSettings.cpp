// AppSettings.cpp: settings.xml with a "lastShow" attribute and the audio
// device state as the root's one child element.
#include "bigtop/io/AppSettings.h"

namespace bigtop::io {

AppSettings::AppSettings(juce::File settingsFile) : file_(std::move(settingsFile)) {}

std::unique_ptr<juce::XmlElement> AppSettings::load() const {
    // A missing or corrupt file yields a fresh root: settings are a
    // convenience, never a reason to fail.
    auto root = juce::XmlDocument::parse(file_);
    if (root == nullptr) root = std::make_unique<juce::XmlElement>("settings");
    return root;
}

void AppSettings::store(const juce::XmlElement& root) const {
    file_.getParentDirectory().createDirectory();
    // Best effort: losing a setting only costs the owner one re-selection.
    root.writeTo(file_);
}

std::optional<juce::File> AppSettings::lastShow() const {
    const auto path = load()->getStringAttribute("lastShow");
    if (path.isEmpty()) return std::nullopt;
    const juce::File dir(path);
    if (!dir.isDirectory()) return std::nullopt;  // moved or deleted since
    return dir;
}

void AppSettings::setLastShow(const juce::File& dir) {
    auto root = load();
    root->setAttribute("lastShow", dir.getFullPathName());
    store(*root);
}

std::unique_ptr<juce::XmlElement> AppSettings::audioDeviceState() const {
    const auto root = load();
    if (const auto* child = root->getFirstChildElement()) return std::make_unique<juce::XmlElement>(*child);
    return nullptr;
}

void AppSettings::setAudioDeviceState(const juce::XmlElement& state) {
    auto root = load();
    root->deleteAllChildElements();
    root->addChildElement(new juce::XmlElement(state));
    store(*root);
}

juce::String AppSettings::artNetAdapter() const { return load()->getStringAttribute("artNetAdapter"); }

void AppSettings::setArtNetAdapter(const juce::String& address) {
    auto root = load();
    // An attribute, like lastShow: the child element is reserved for the audio state.
    root->setAttribute("artNetAdapter", address);
    store(*root);
}

bool AppSettings::stepDetails() const { return load()->getBoolAttribute("stepDetails", false); }

void AppSettings::setStepDetails(bool on) {
    auto root = load();
    root->setAttribute("stepDetails", on);
    store(*root);
}

}  // namespace bigtop::io
