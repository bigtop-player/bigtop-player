// AppTestMain.cpp: the app test exe's main. Before Catch2 runs it makes sure no failure can
// open a modal dialog (that would hang the quality gate and CI), then starts JUCE's GUI
// subsystems (no window is ever opened; components are only built and driven).
//
// Hidden flags, each checked by a test that runs this exe as a child process:
// `--trigger-crt-assert` fires one MSVC debug-CRT assert and returns 0 ("a CRT assert ends the test
// exe instead of opening a dialog"); `--trigger-second-clock` builds two ManualClocks at once
// ("a second ManualClock ends the test exe").
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>

#include <catch2/catch_session.hpp>
#include <juce_gui_basics/juce_gui_basics.h>

#include "UiClock.h"

#if JUCE_WINDOWS
#include <crtdbg.h>
#include <windows.h>
#endif

namespace {

#if JUCE_WINDOWS && defined(_DEBUG)
// A debug-CRT report (an STL index check, _ASSERTE, a heap check): print it and end the process
// with exit code 3, the code the smoke test expects. Warnings are printed and the run goes on.
int handled(int type, int* returnValue) {
    std::fflush(stderr);
    if (type == _CRT_WARN) {
        *returnValue = 0;  // no debugger break
        return TRUE;       // handled: no dialog
    }
    std::_Exit(3);
}

// Two hooks: _ASSERTE and the STL report through the wide-character path, the rest through the
// narrow one, and each path calls only the hook installed for it.
int narrowHook(int type, char* message, int* returnValue) {
    std::fputs(message != nullptr ? message : "CRT report\n", stderr);
    return handled(type, returnValue);
}

int wideHook(int type, wchar_t* message, int* returnValue) {
    std::fputws(message != nullptr ? message : L"CRT report\n", stderr);
    return handled(type, returnValue);
}
#endif

void installNoDialogHooks() {
#if JUCE_WINDOWS
#if defined(_DEBUG)
    for (const int type : {_CRT_ASSERT, _CRT_ERROR, _CRT_WARN}) {
        _CrtSetReportMode(type, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(type, _CRTDBG_FILE_STDERR);
    }
    _CrtSetReportHook2(_CRT_RPTHOOK_INSTALL, narrowHook);
    _CrtSetReportHookW2(_CRT_RPTHOOK_INSTALL, wideHook);
#endif
    // No Windows error box for a crash or a missing drive, and abort() neither prints a box nor
    // calls Windows Error Reporting: the process just ends with a non-zero code.
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
}

// For the hidden flags: the child ends with _Exit, statics are never destroyed, and the debug CRT's
// leak dump (about 35 KB) would land in the test output after the one line that matters.
void noLeakDump() {
#if JUCE_WINDOWS && defined(_DEBUG)
    _CrtSetDbgFlag(_CrtSetDbgFlag(_CRTDBG_REPORT_FLAG) & ~_CRTDBG_LEAK_CHECK_DF);
#endif
}

}  // namespace

int main(int argc, char* argv[]) {
    installNoDialogHooks();
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--trigger-crt-assert") == 0) {
            noLeakDump();
#if JUCE_WINDOWS
            _ASSERTE(false);  // Debug: the hook ends the process with 3; Release: compiled out
#endif
            return 0;
        }
        if (std::strcmp(argv[i], "--trigger-second-clock") == 0) {
            noLeakDump();
            const bigtop::app::ManualClock first;
            const bigtop::app::ManualClock second;  // ends the process with exit code 4
            return 0;
        }
    }
    const juce::ScopedJuceInitialiser_GUI juce;
    return Catch::Session().run(argc, argv);
}
