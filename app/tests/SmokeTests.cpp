// SmokeTests.cpp: the app test exe's own guards (no dialog on a CRT assert, the clock and finger
// seams) and one GO through the whole windowless app.
#include <catch2/catch_test_macros.hpp>

#include "AboutPanel.h"
#include "AppTestSupport.h"
#include "Pointer.h"
#include "UiClock.h"

namespace bigtop::app {

TEST_CASE("a CRT assert ends the test exe instead of opening a dialog") {
    juce::ChildProcess child;
    const auto self = juce::File::getSpecialLocation(juce::File::currentExecutableFile).getFullPathName();
    // No pipes (flags 0): an unread pipe would fill up and block the child, which looks like a hang.
    REQUIRE(child.start(juce::StringArray{self, "--trigger-crt-assert"}, 0));
    REQUIRE(child.waitForProcessToFinish(10000));  // a dialog would hold it open: this fails instead of hanging
#if JUCE_WINDOWS && defined(_DEBUG)
    REQUIRE(child.getExitCode() == 3);
#else
    REQUIRE(child.getExitCode() == 0);  // _ASSERTE is compiled out
#endif
}

TEST_CASE("a second ManualClock ends the test exe") {
    juce::ChildProcess child;
    const auto self = juce::File::getSpecialLocation(juce::File::currentExecutableFile).getFullPathName();
    REQUIRE(child.start(juce::StringArray{self, "--trigger-second-clock"}, 0));  // no pipes, as above
    REQUIRE(child.waitForProcessToFinish(10000));
    REQUIRE(child.getExitCode() == 4);  // in every configuration: not a jassert, which only logs
}

TEST_CASE("UiClock is real again after a ManualClock goes") {
    {
        ManualClock clock(5000);
        REQUIRE(uiMs() == 5000);
        clock.advance(500);
        REQUIRE(uiMs() == 5500);
        REQUIRE(uiSeconds() == 5.5);
    }
    const auto first = uiMs();
    juce::Thread::sleep(20);  // the one real sleep: proves the real counter is back
    REQUIRE(uiMs() != first);
}

TEST_CASE("the app opens a show offline and GO plays it") {
    const ManualClock clock;
    TestApp app(threeSoundSteps());
    app.waitHeads();
    requireHittable(AppTestAccess::goButton(app.main()));
    REQUIRE(app.session().snapshot().numVoices == 0);
    REQUIRE(app.session().controller().nextIndex() == 0);

    AppTestAccess::go(app.main());
    app.audio(10);

    REQUIRE(app.session().snapshot().numVoices == 1);
    REQUIRE(app.session().controller().nextIndex() == 1);
}

TEST_CASE("finger index comes from PointerOverride") {
    juce::Component c;
    c.setBounds(0, 0, 10, 10);
    const juce::MouseEvent e(juce::Desktop::getInstance().getMainMouseSource(), {1.0f, 1.0f}, {}, 1.0f, 0.0f, 0.0f, 0.0f,
                             0.0f, &c, &c, {}, {1.0f, 1.0f}, {}, 1, false);
    REQUIRE(pointerIndex(e) == 0);
    {
        const PointerOverride second(1);
        REQUIRE(pointerIndex(e) == 1);
        {
            const PointerOverride third(2);
            REQUIRE(pointerIndex(e) == 2);
        }
        REQUIRE(pointerIndex(e) == 1);
    }
    REQUIRE(pointerIndex(e) == 0);
}

}  // namespace bigtop::app

TEST_CASE("About names the build's version, the licence and the project page") {
    bigtop::app::AboutPanel about;
    const auto text = about.text();
    CHECK(text.contains(juce::String("Version ") + BIGTOP_VERSION));
    CHECK(text.contains("GNU Affero General Public License v3"));
    CHECK(text.contains("https://github.com/bigtop-player/bigtop-player"));
    CHECK(juce::String(BIGTOP_VERSION).startsWith("0.2.0"));
}
