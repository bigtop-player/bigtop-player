# Top-level build for Bigtop Player. core/ is the engine library plus its
# tests (always built); io/ is the JUCE-based audio/file layer (opt-in,
# BIGTOP_BUILD_IO); app/ is the JUCE desktop app (opt-in, BIGTOP_BUILD_APP,
# which needs io). Third-party dependencies are pinned to exact tags and
# fetched at configure time; JUCE and r8brain only when io is built.
# MSVC debug info goes into each .obj (/Z7) instead of one shared PDB (/Zi): a compiler
# cache (sccache in CI) can only reuse objects that carry their own debug info. The policy
# default is set before cmake_minimum_required, which is where CMake reads it; the format
# is set before project(), so CMake's own compiler checks use it too. Fetched projects
# such as JUCE inherit it.
set(CMAKE_POLICY_DEFAULT_CMP0141 NEW)
cmake_minimum_required(VERSION 3.20)
set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<$<CONFIG:Debug,RelWithDebInfo>:Embedded>")
project(BigtopPlayer VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Static MSVC runtime: the exe runs on a clean laptop with no Visual C++
# redistributable. Set before any target (incl. fetched JUCE/Catch2) so all agree.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

option(BIGTOP_BUILD_IO "Build the JUCE-based io library and its tests" OFF)
option(BIGTOP_BUILD_APP "Build the JUCE app (forces BIGTOP_BUILD_IO on)" OFF)
option(BIGTOP_TSAN "Build core and its tests with ThreadSanitizer (GCC/Clang only)" OFF)
# A compiler cache such as ccache or sccache, run in front of every compile of our targets
# and the code they build from Catch2 and JUCE. An option rather than CMake's launcher
# variables, because those also reach the compiler checks and JUCE's nested juceaide
# build, which compile in ways a cache refuses (see the JUCE fetch below). The environment
# variable of the same name gives the first value, which is how CI sets it.
set(BIGTOP_COMPILER_LAUNCHER "$ENV{BIGTOP_COMPILER_LAUNCHER}" CACHE STRING "Compiler cache to run compiles through, e.g. ccache or sccache")
if(BIGTOP_COMPILER_LAUNCHER)
    set(CMAKE_C_COMPILER_LAUNCHER "${BIGTOP_COMPILER_LAUNCHER}")
    set(CMAKE_CXX_COMPILER_LAUNCHER "${BIGTOP_COMPILER_LAUNCHER}")
endif()
if(BIGTOP_BUILD_APP)
    # Normal variable, not the cache: turning the app off again restores the
    # user's own BIGTOP_BUILD_IO choice.
    set(BIGTOP_BUILD_IO ON)
endif()
if(BIGTOP_TSAN AND MSVC)
    message(STATUS "BIGTOP_TSAN is ignored on MSVC (no ThreadSanitizer); the TSan job runs on Linux/clang in CI.")
endif()

# Warnings are errors for every target we own. Applied per target (never
# globally) so third-party code keeps its own flags.
function(bigtop_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
    endif()
endfunction()

include(FetchContent)
FetchContent_Declare(Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG v3.7.1)
FetchContent_Declare(nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3)
FetchContent_MakeAvailable(Catch2 nlohmann_json)

enable_testing()
include(${catch2_SOURCE_DIR}/extras/Catch.cmake)

add_subdirectory(core)

if(BIGTOP_BUILD_IO)
    if(CMAKE_VERSION VERSION_LESS 3.22)
        message(FATAL_ERROR "JUCE 9 needs CMake 3.22 or newer (found ${CMAKE_VERSION}).")
    endif()
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG 9.0.5)
    # r8brain-free-src: offline resampler for imports (MIT). Plain sources, no
    # CMake project of its own is used; io/CMakeLists.txt compiles r8bbase.cpp.
    FetchContent_Declare(r8brain
        GIT_REPOSITORY https://github.com/avaneev/r8brain-free-src.git
        GIT_TAG version-6.5)
    # JUCE configures and builds its juceaide helper in a nested CMake run, passing the
    # launcher on but not the /Z7 setting, so sccache would refuse that build. juceaide
    # builds without the cache; JUCE's modules compile inside our own targets and are cached.
    set(bigtop_c_launcher "${CMAKE_C_COMPILER_LAUNCHER}")
    set(bigtop_cxx_launcher "${CMAKE_CXX_COMPILER_LAUNCHER}")
    set(CMAKE_C_COMPILER_LAUNCHER "")
    set(CMAKE_CXX_COMPILER_LAUNCHER "")
    FetchContent_MakeAvailable(JUCE r8brain)
    set(CMAKE_C_COMPILER_LAUNCHER "${bigtop_c_launcher}")
    set(CMAKE_CXX_COMPILER_LAUNCHER "${bigtop_cxx_launcher}")
    add_subdirectory(io)
endif()

if(BIGTOP_BUILD_APP)
    add_subdirectory(app)
endif()
