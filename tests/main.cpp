// Main test runner for Cygnus Actor Framework
// All test files are compiled into this single executable

#define CATCH_CONFIG_RUNNER
#include <catch2/catch.hpp>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <crtdbg.h>
#include <cstdio>
#include <cstdlib>
#endif

// Test execution will run in order:
// - 00_basic: Fundamental operations (creation, naming, registry)
// - 01_simple: Simple operations (messages, handlers)
// - 02_interactions: Actor communication
// - 03_lifecycle: Lifecycle management
// - 04_advanced: Complex scenarios

#if defined(_WIN32)
// Last-chance handler for crashes (access violations and the like): print the
// exception code and end the process, instead of letting Windows error
// reporting or the Just-In-Time debugger prompt put a window on the desktop.
static LONG WINAPI report_crash_and_exit(EXCEPTION_POINTERS* info) {
    std::fprintf(stderr, "\nCRASH: unhandled exception 0x%08lX at %p - test run aborted\n",
                 info->ExceptionRecord->ExceptionCode,
                 info->ExceptionRecord->ExceptionAddress);
    std::fflush(stderr);
    return EXCEPTION_EXECUTE_HANDLER;  // terminate silently
}
#endif

int main(int argc, char* argv[]) {
#if defined(_WIN32)
    SetUnhandledExceptionFilter(&report_crash_and_exit);
    // Never show a modal dialog. A crashing or aborting test must fail in the
    // console output, not stop the run until someone clicks a window - on a
    // developer's desktop that looks like the machine is crashing, and on CI
    // it hangs the job. Covers the Windows error-reporting dialog, the Debug
    // CRT's "abort() has been called" box, and CRT assertion boxes.
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    _set_abort_behavior(0, _CALL_REPORTFAULT);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
#endif
    return Catch::Session().run(argc, argv);
}
