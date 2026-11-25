# io: everything that touches audio files, the show folder and the sound card.
# Built only with BIGTOP_BUILD_IO (forced on by BIGTOP_BUILD_APP).

# bigtop_juce: the one target that compiles JUCE, for io and the app alike,
# with JUCE's own recommended flags so our warnings-as-errors apply only to
# our sources. JUCE modules are INTERFACE libraries that carry their .cpp
# files, so they are linked PRIVATE (compiled here, not again in every
# consumer) and their include paths and definitions are re-exported by hand,
# as JUCE's CMake API docs show for a shared static library. Every executable
# must get its JUCE code from this target only, or modules are compiled twice
# (duplicate symbols). io code itself never uses the GUI modules; they are
# added only for the app, so io-only builds (Linux CI) need no GUI packages.
add_library(bigtop_juce STATIC)
target_link_libraries(bigtop_juce
    PRIVATE juce::juce_audio_devices juce::juce_audio_formats juce::juce_audio_basics juce::juce_cryptography juce::juce_core
    PUBLIC juce::juce_recommended_config_flags)
if(BIGTOP_BUILD_APP)
    # juce_audio_utils: the app's device selector (AudioDeviceSelectorComponent).
    target_link_libraries(bigtop_juce PRIVATE juce::juce_gui_basics juce::juce_audio_utils)
endif()
target_compile_definitions(bigtop_juce
    PUBLIC JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_USE_MP3AUDIOFORMAT=1
    INTERFACE $<TARGET_PROPERTY:bigtop_juce,COMPILE_DEFINITIONS>)
target_include_directories(bigtop_juce
    INTERFACE $<TARGET_PROPERTY:bigtop_juce,INCLUDE_DIRECTORIES>)

# bigtop_r8brain: r8brain's one source file, in its own target because it
# warns under /W4 (C4201, nameless unions); the compiler's default warning
# level applies here. Its headers are SYSTEM includes for the same reason.
add_library(bigtop_r8brain STATIC ${r8brain_SOURCE_DIR}/r8bbase.cpp)
target_include_directories(bigtop_r8brain SYSTEM PUBLIC ${r8brain_SOURCE_DIR})
# r8brain includes <windows.h>; without NOMINMAX its min/max macros break
# every later std::min/max (and JUCE's headers) in the including file.
if(WIN32)
    target_compile_definitions(bigtop_r8brain PUBLIC NOMINMAX)
endif()

# bigtop_io: our io code, on top of core, JUCE and r8brain.
add_library(bigtop_io STATIC src/AudioOutput.cpp src/ShowFolder.cpp src/AppSettings.cpp src/Importer.cpp src/StreamingSource.cpp src/ArtNetSender.cpp src/ShowArchive.cpp src/Waveform.cpp)
target_include_directories(bigtop_io PUBLIC include)
target_link_libraries(bigtop_io PUBLIC bigtop_core bigtop_juce bigtop_r8brain)
# GetAdaptersAddresses (real adapter names and prefix lengths) lives in iphlpapi.
if(MSVC)
    target_link_libraries(bigtop_io PUBLIC iphlpapi ws2_32)
endif()
bigtop_warnings(bigtop_io)

# Catch2 tests for io; each TEST_CASE is registered with CTest.
add_executable(bigtop_io_tests tests/SmokeTests.cpp tests/ShowFolderTests.cpp tests/AppSettingsTests.cpp tests/ImporterTests.cpp tests/StreamingSourceTests.cpp tests/AudioOutputTests.cpp tests/ArtNetSenderTests.cpp tests/ShowArchiveTests.cpp tests/WaveformTests.cpp)
target_link_libraries(bigtop_io_tests PRIVATE bigtop_io Catch2::Catch2WithMain)
bigtop_warnings(bigtop_io_tests)
# Checked-in test inputs (e.g. the MP3 that needs an encoder to make).
target_compile_definitions(bigtop_io_tests PRIVATE BIGTOP_IO_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
catch_discover_tests(bigtop_io_tests)
