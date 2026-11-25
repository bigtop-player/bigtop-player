# bigtop_core: the engine library. Pure C++20 with no JUCE or OS headers, so it
# can be unit-tested on any machine. Warnings are errors (bigtop_warnings in the
# top-level CMakeLists.txt). nlohmann_json is PUBLIC because the tests include
# <nlohmann/json.hpp> through bigtop_core; our own headers do not expose it.
add_library(bigtop_core STATIC src/Engine.cpp src/Gain.cpp src/OutputLimiter.cpp src/Show.cpp src/ShowController.cpp src/ShowEdits.cpp src/ShowJson.cpp src/StepDetails.cpp src/Timecode.cpp)
target_include_directories(bigtop_core PUBLIC include)
target_link_libraries(bigtop_core PUBLIC nlohmann_json::nlohmann_json)
target_compile_features(bigtop_core PUBLIC cxx_std_20)
bigtop_warnings(bigtop_core)
# PUBLIC so the tests (and anything else linking core) are instrumented too;
# TSan only reports races in instrumented code.
if(BIGTOP_TSAN AND NOT MSVC)
    target_compile_options(bigtop_core PUBLIC -fsanitize=thread)
    target_link_options(bigtop_core PUBLIC -fsanitize=thread)
endif()

# Catch2 tests for the core library; each TEST_CASE is registered with CTest.
add_executable(bigtop_core_tests tests/EngineClickTests.cpp tests/EnginePauseTests.cpp tests/EngineSeekTests.cpp tests/EngineStartTests.cpp tests/EngineStepTests.cpp tests/EngineThreadTests.cpp tests/GainTests.cpp tests/OutputLimiterTests.cpp tests/ShowControllerTests.cpp tests/ShowEditsTests.cpp tests/ShowJsonTests.cpp
    tests/SpecExampleTests.cpp tests/SpscQueueTests.cpp tests/StepDetailsTests.cpp tests/TimecodeTests.cpp)
target_link_libraries(bigtop_core_tests PRIVATE bigtop_core Catch2::Catch2WithMain)
bigtop_warnings(bigtop_core_tests)
catch_discover_tests(bigtop_core_tests)
