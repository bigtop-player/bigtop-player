# AGENTS.md

Instructions for AI coding agents working on Bigtop Player. Humans: the same rules are in [CONTRIBUTING.md](CONTRIBUTING.md).

## What this is

A cue-based music player for live shows, C++20. `core/` is the JUCE-free engine and show model (realtime audio thread, lock-free command queue), `io/` is file import, audio devices and Art-Net (JUCE), `app/` is the JUCE GUI. Each has a `tests/` folder (Catch2). Mistakes are heard by an audience: no clicks, no dropouts, no crash during a show.

## Commands

- Configure and build everything: `cmake -B build -DBIGTOP_BUILD_APP=ON` then `cmake --build build --config Debug`
- All tests: `ctest --test-dir build -C Debug --output-on-failure`; GUI tests only: `ctest --test-dir build -C Debug -R "^app:"`
- Release app: `cmake --build build --config Release --target bigtop_app`
- Docs: `doxygen Doxyfile` (any warning is an error)
- Linux (GCC, Clang, ThreadSanitizer): the three `wsl` lines in CONTRIBUTING.md "Before you push"

Before you call work done, run the whole gate from CONTRIBUTING.md: zero warnings (warnings are errors), all tests pass, Doxygen silent, on MSVC and on GCC and Clang.

## Rules

- Never silence a warning; fix it. Never weaken a test to make it pass.
- Bugs: write a test that fails on the current code first, then fix. Keep the test.
- Every public declaration gets a `///` Doxygen comment (`@brief`, `@param`, `@return`); non-trivial code gets comments that explain why. Comments state the reason itself, never "as decided on <date>".
- Nothing on the audio thread allocates, locks or does I/O.
- Tests, screenshots and docs use invented data: IPs from 192.0.2.0/24, 198.51.100.0/24, 203.0.113.0/24; made-up names and paths. Tests open no real audio device and send no real network packet.
- Behaviour or UI change: update the README feature list, `docs/wiki/`, and re-render screenshots with `--docs-screenshots docs/images` in the same change.
- Commits: Conventional Commits title `type(scope): summary`, imperative, at most 72 characters (`feat fix perf refactor test docs build ci chore`). One change per commit. Add a `Co-Authored-By:` or `Assisted-by:` line naming the agent.
