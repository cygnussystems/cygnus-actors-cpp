// Fatal-path probe for the failure-mode matrix (INVARIANT 7).
//
// The one deliberately fatal case: a fast_actor created before start() whose
// on_start() throws. That on_start() runs on the actor's own thread, where
// nobody can receive the exception, so the framework reports it and calls
// std::terminate(). This program triggers it; ctest passes when the report
// appears in the output (the process is expected to die).

#include "cas/cas.h"

#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <thread>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <crtdbg.h>
#include <cstdlib>
#endif

namespace {

class fast_throws_in_on_start : public cas::fast_actor {
protected:
    void on_start() override { throw std::runtime_error("probe on_start failure"); }
};

} // namespace

int main() {
#if defined(_WIN32)
    // Never show a modal dialog for the expected abort (see tests/main.cpp)
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
#endif
    cas::system::create<fast_throws_in_on_start>();
    cas::system::start();

    // The dedicated thread reports and terminates the process
    std::this_thread::sleep_for(std::chrono::seconds(5));
    std::printf("probe: process was not terminated\n");
    return 0;
}
