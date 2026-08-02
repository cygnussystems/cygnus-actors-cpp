# Runs isolation_probe and asserts it fails in exactly the expected way.
#
# The probe binary contains one deliberately-failing test followed by four
# healthy ones. If test isolation works, exactly one test case fails. If a
# failure corrupts the process-wide actor system, the healthy tests fail too
# and the count is higher.
#
# Invoked by CTest as the "test_isolation" test. Requires PROBE_EXE to be set.

if(NOT DEFINED PROBE_EXE)
    message(FATAL_ERROR "PROBE_EXE not set")
endif()

execute_process(
    COMMAND "${PROBE_EXE}" --reporter compact
    OUTPUT_VARIABLE probe_output
    ERROR_VARIABLE  probe_error
    RESULT_VARIABLE probe_result
    TIMEOUT 120
)

set(combined "${probe_output}${probe_error}")
message(STATUS "isolation_probe output:\n${combined}")

# Catch2's compact reporter ends with either:
#   "Failed 1 test case, failed 1 assertion."
#   "Failed N test cases, failed M assertions."
if(combined MATCHES "Failed ([0-9]+) test case")
    set(failed_cases "${CMAKE_MATCH_1}")
else()
    message(FATAL_ERROR
        "Could not parse a failure count from isolation_probe output.\n"
        "The probe is expected to fail exactly once; if it passed outright, "
        "the deliberately-failing assertion is no longer failing.\n"
        "Output:\n${combined}")
endif()

if(NOT failed_cases EQUAL 1)
    message(FATAL_ERROR
        "TEST ISOLATION REGRESSED: expected exactly 1 failing test case, got "
        "${failed_cases}.\n"
        "A single failing test is corrupting the tests that follow it. This "
        "usually means a test that starts the actor system is missing its "
        "CAS_TEST_GUARD() / CAS_CONFIG_GUARD() declaration, so cleanup is "
        "skipped when a REQUIRE throws and the singleton is left running.\n"
        "See doc/INVARIANTS.md, 'Test hygiene'.\n"
        "Output:\n${combined}")
endif()

message(STATUS "Test isolation OK: exactly 1 failing case, as expected.")
