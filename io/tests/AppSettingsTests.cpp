// AppSettingsTests.cpp: remembered last show and audio device state.
#include <catch2/catch_test_macros.hpp>

#include "bigtop/io/AppSettings.h"

using bigtop::io::AppSettings;

TEST_CASE("AppSettings remembers last show and forgets a vanished folder") {
    const auto root = juce::File::getSpecialLocation(juce::File::tempDirectory)
                          .getChildFile("bigtop_settings_" + juce::String(juce::Random::getSystemRandom().nextInt64()));
    const auto show = root.getChildFile("myshow");
    show.createDirectory();
    const auto file = root.getChildFile("sub").getChildFile("settings.xml");  // parent created on write

    AppSettings s(file);
    CHECK_FALSE(s.lastShow().has_value());
    CHECK(s.audioDeviceState() == nullptr);

    s.setLastShow(show);
    juce::XmlElement dev("DEVICESETUP");
    dev.setAttribute("deviceType", "ASIO");
    s.setAudioDeviceState(dev);

    AppSettings again(file);  // a new instance reads it back from disk
    REQUIRE(again.lastShow().has_value());
    CHECK(*again.lastShow() == show);
    const auto state = again.audioDeviceState();
    REQUIRE(state != nullptr);
    CHECK(state->getStringAttribute("deviceType") == "ASIO");
    CHECK(again.lastShow().has_value());  // device write did not drop it

    show.deleteRecursively();
    CHECK_FALSE(again.lastShow().has_value());
    root.deleteRecursively();
}

TEST_CASE("AppSettings round-trips the Art-Net adapter without disturbing the device state") {
    const auto file = juce::File::getSpecialLocation(juce::File::tempDirectory)
                          .getChildFile("bigtop_settings_" + juce::String(juce::Random::getSystemRandom().nextInt64()) + ".xml");
    AppSettings s(file);
    CHECK(s.artNetAdapter() == "");  // unset = Default
    juce::XmlElement dev("DEVICESETUP");
    s.setAudioDeviceState(dev);
    s.setArtNetAdapter("192.168.1.20");
    AppSettings again(file);
    CHECK(again.artNetAdapter() == "192.168.1.20");
    CHECK(again.audioDeviceState() != nullptr);  // adapter write did not drop it
    again.setAudioDeviceState(dev);
    CHECK(again.artNetAdapter() == "192.168.1.20");  // nor the other way round
    again.setArtNetAdapter("");
    CHECK(again.artNetAdapter() == "");
    file.deleteFile();
}

TEST_CASE("AppSettings remembers the step list's Details choice, off by default") {
    const auto file = juce::File::getSpecialLocation(juce::File::tempDirectory)
                          .getChildFile("bigtop_settings_" + juce::String(juce::Random::getSystemRandom().nextInt64()) + ".xml");
    AppSettings s(file);
    CHECK_FALSE(s.stepDetails());
    s.setArtNetAdapter("192.0.2.15");
    s.setStepDetails(true);
    AppSettings again(file);
    CHECK(again.stepDetails());
    CHECK(again.artNetAdapter() == "192.0.2.15");  // the other settings stay
    again.setStepDetails(false);
    CHECK_FALSE(AppSettings(file).stepDetails());
    file.deleteFile();
}
TEST_CASE("The stored last-show path is kept when the folder is gone") {
    const auto dir = juce::File::createTempFile("bigtop-settings");
    REQUIRE(dir.createDirectory());
    const auto show = dir.getChildFile("Winter gala");
    REQUIRE(show.createDirectory());
    bigtop::io::AppSettings s(dir.getChildFile("settings.xml"));
    s.setLastShow(show);
    REQUIRE(show.deleteRecursively());
    CHECK_FALSE(s.lastShow().has_value());
    CHECK(s.lastShowPath() == show.getFullPathName());
    dir.deleteRecursively();
}
