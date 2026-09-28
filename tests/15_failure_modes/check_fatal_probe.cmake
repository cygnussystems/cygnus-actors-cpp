# Runs fatal_probe and asserts it dies in exactly the expected way.
#
# The probe creates a fast_actor whose on_start() throws, before start(). The
# framework must report the exception and then terminate the process. How a
# terminated process looks differs by platform - an exit code on Windows, a
# SIGABRT on POSIX (which CTest would count as a failure by itself) - so the
# probe is run from here and judged by its output.
#
# Invoked by CTest as the "test_fatal_on_start" test. Requires PROBE_EXE.

if(NOT DEFINED PROBE_EXE)
    message(FATAL_ERROR "PROBE_EXE not set")
endif()

execute_process(
    COMMAND "${PROBE_EXE}"
    OUTPUT_VARIABLE probe_output
    ERROR_VARIABLE  probe_error
    RESULT_VARIABLE probe_result
    TIMEOUT 30
)

set(combined "${probe_output}${probe_error}")
message(STATUS "fatal_probe result: ${probe_result}\n${combined}")

if(NOT combined MATCHES "exception in on_start of actor .*probe on_start failure - fatal, terminating")
    message(FATAL_ERROR "fatal_probe did not report the on_start() exception before terminating")
endif()

if(combined MATCHES "process was not terminated" OR probe_result EQUAL 0)
    message(FATAL_ERROR "fatal_probe kept running after on_start() threw")
endif()

message(STATUS "Fatal on_start path OK: reported, then terminated.")
