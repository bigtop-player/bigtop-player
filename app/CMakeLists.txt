# The JUCE desktop app. Only added when BIGTOP_BUILD_APP is ON (see top-level
# CMakeLists.txt) so core builds and tests don't need to fetch JUCE.
# All JUCE code, GUI modules (juce_gui_basics, juce_audio_utils) included,
# comes through bigtop_io from the shared bigtop_juce target
# (io/CMakeLists.txt); linking a juce:: module here would compile it a second
# time. Warnings are errors here too, for our sources only.
#
# bigtop_app_lib holds every app source but Main.cpp (the only one with
# START_JUCE_APPLICATION), so the app and its test exe link the same code.
add_library(bigtop_app_lib STATIC
    MainComponent.cpp ShowSession.cpp StartPanel.cpp StatusBar.cpp LevelMeter.cpp SettingsWindow.cpp
    StepList.cpp RunningSounds.cpp ModeBar.cpp
    NumberField.cpp StepEditor.cpp ShowSettingsPanel.cpp PadGrid.cpp PadEditor.cpp Screenshots.cpp
    DemoAudio.cpp Pointer.cpp UiClock.cpp AlertBanner.cpp ShowCheckPanel.cpp KeepAwake.cpp Confirm.cpp AboutPanel.cpp)
target_include_directories(bigtop_app_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(bigtop_app_lib PUBLIC bigtop_io)
target_compile_definitions(bigtop_app_lib PUBLIC BIGTOP_VERSION="${PROJECT_VERSION}${BIGTOP_VERSION_SUFFIX}")
bigtop_warnings(bigtop_app_lib)

juce_add_gui_app(bigtop_app PRODUCT_NAME "Bigtop Player")
target_sources(bigtop_app PRIVATE Main.cpp)
target_link_libraries(bigtop_app PRIVATE bigtop_app_lib)
bigtop_warnings(bigtop_app)

# Windowless GUI tests: MainComponent over ShowSession::Offline, driven by direct calls.
# Own main (AppTestMain.cpp): it turns MSVC debug-CRT asserts into exit code 3 instead of a dialog.
add_executable(bigtop_app_tests tests/AppTestMain.cpp tests/AppTestSupport.cpp tests/SmokeTests.cpp
                                tests/RunningTests.cpp tests/LayoutTests.cpp tests/StepListTests.cpp
                                tests/ModeTests.cpp tests/EditorTests.cpp tests/PadTests.cpp
                                tests/AccessibilityTests.cpp tests/StatusTests.cpp)
target_link_libraries(bigtop_app_tests PRIVATE bigtop_app_lib Catch2::Catch2)
bigtop_warnings(bigtop_app_tests)
# The "app:" prefix lets `ctest -R "^app:"` (CI) pick exactly these.
catch_discover_tests(bigtop_app_tests TEST_PREFIX "app:")
