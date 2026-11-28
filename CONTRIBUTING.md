# Contributing to Bigtop Player

Thanks for helping. Bug reports and ideas go in the issue tracker; changes come as pull requests. Bigtop Player runs live shows, so the rules below are about one thing: nothing may break or click in front of an audience.

## Build and test

See "Build from source" in the [README](README.md). The app is built with `-DBIGTOP_BUILD_APP=ON`.

## Before you push

The quality gate: a clean configure with everything on, a full build with zero warnings, all tests passing (including the `[stress]` thread test), and Doxygen without warnings, on Windows (MSVC) and on Linux (GCC, Clang, and the ThreadSanitizer build), as CI runs it. GCC and Clang warn about things MSVC does not, so a Windows-only gate is not enough. macOS (ARM64) is supported too, but CI does not build it yet: when you change build files or platform code, check the build and the app's start-up on macOS by hand and say so in the pull request.

Windows, from the repo root:

```bash
cmake -B build -DBIGTOP_BUILD_APP=ON
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
cmake --build build --config Release --target bigtop_app
doxygen Doxyfile
```

The `ctest` line also runs the windowless GUI tests (`bigtop_app_tests`, names starting `app:`; `ctest -R "^app:"` runs only them); they open no window, audio device or network socket.

CI builds on Windows with Ninja instead of the Visual Studio generator, because only Ninja lets a compiler cache run in front of MSVC. Locally the Visual Studio generator stays the default. To build the way CI does, run the same lines from a "Developer PowerShell for VS" with `-G "Ninja Multi-Config"` added to the first one. `--config` and `ctest -C` work unchanged, and the app lands in the same `build/app/bigtop_app_artefacts/<config>/` folder. To use a compiler cache yourself, add `-DBIGTOP_COMPILER_LAUNCHER=ccache` (or `sccache`) to the first line; this works with Ninja and Makefiles, not with the Visual Studio generator. Use this option rather than CMake's own launcher variables: the build keeps the cache away from compiles it cannot handle.

Linux, through WSL Ubuntu from the repo root on Windows (WSL starts in the current folder; needs cmake, g++, clang, pkg-config, `libasound2-dev`, `libfreetype-dev`, `libfontconfig1-dev`). Build dirs stay in `~/bt`, outside the Windows checkout. Warnings are errors, so a build that finishes has none.

```bash
wsl -d Ubuntu -- bash -lc 'cmake -S . -B ~/bt/gcc -DCMAKE_BUILD_TYPE=Debug -DBIGTOP_BUILD_IO=ON && cmake --build ~/bt/gcc -j16 && ctest --test-dir ~/bt/gcc --output-on-failure'
wsl -d Ubuntu -- bash -lc 'cmake -S . -B ~/bt/clang -DCMAKE_BUILD_TYPE=Debug -DBIGTOP_BUILD_IO=ON -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ && cmake --build ~/bt/clang -j16 && ctest --test-dir ~/bt/clang --output-on-failure'
wsl -d Ubuntu -- bash -lc 'CC=clang CXX=clang++ cmake -S . -B ~/bt/tsan -DCMAKE_BUILD_TYPE=Debug -DBIGTOP_TSAN=ON && cmake --build ~/bt/tsan -j16 --target bigtop_core_tests && ~/bt/tsan/core/bigtop_core_tests'
```

The last line is the CI `tsan` job: any race report makes it exit non-zero. CI first runs `sudo sysctl vm.mmap_rnd_bits=28` because the GitHub runner's randomisation makes TSan abort at startup; the WSL kernel does not need it.

Docs: when behaviour or the UI changed, re-render the docs screenshots (`--docs-screenshots docs/images`, see "Running the app" in the README) and update the feature list and the wiki in `docs/wiki/`. CI only checks that the render runs; it does not compare the images, so a stale picture is caught by you, not by CI.

## Documentation

Run `doxygen Doxyfile` from the repo root; the HTML lands in `build/docs/index.html`. Every public declaration needs a Doxygen comment (`@brief`, `@param`, `@return`): undocumented public API produces a warning, and warnings fail CI.

## Code rules

- Every public declaration has a Doxygen comment (`///`, with `@brief`, `@param`, `@return`); non-trivial code has comments that say why, not what. Doxygen warnings fail the build.
- A bug fix starts with a test that fails on the old code; the test stays as a guard.
- Tests, screenshots and docs use invented data only: IP addresses from the documentation ranges (192.0.2.0/24, 198.51.100.0/24, 203.0.113.0/24), made-up adapter, show and file names, no real paths or user names. Tests and screenshot runs never send real network packets or open a real audio device.
- When behaviour or the UI changes, the same pull request updates the README feature list, the pages in `docs/wiki/`, and the screenshots (re-rendered with `--docs-screenshots docs/images`, never edited by hand).

## Commits and pull requests

- One pull request is one feature or one fix. Bigger work is split into several pull requests.
- Pull requests are squash-merged, so the pull request title becomes the commit title: [Conventional Commits](https://www.conventionalcommits.org/), `type(scope): summary`, imperative, at most 72 characters. Types: `feat`, `fix`, `perf`, `refactor`, `test`, `docs`, `build`, `ci`, `chore`. The scope is optional (`engine`, `ui`, `io`, `pads`, ...).
- The description says what changes for the person running a show and why, in plain words. `Fixes #<n>` closes an issue.
- Maintainers label issues and pull requests (`bug`, `idea`, `question`, `documentation`, ...); the issue templates set the first label.

## AI tools

You may use AI coding tools. You are responsible for every line you submit: read it, test it, and be able to explain it. When an AI tool wrote non-trivial parts of a change, add an `Assisted-by: <tool and model>` line to the commit message and tick the box in the pull request template. Most of Bigtop Player itself is written with Claude; see "How this is built" in the README.

## How a pull request is merged

Maintainers apply accepted pull requests in the upstream repository, where the full test matrix runs, and publish them here with you as the author (or co-author, when a maintainer changed it). Your pull request is then closed with a link to the commit. Collaborators with access to the upstream repository open their pull request there instead; a maintainer reviews it and squash-merges it (Conventional Commits title, and an `Assisted-by:` line when an AI tool wrote non-trivial parts).
