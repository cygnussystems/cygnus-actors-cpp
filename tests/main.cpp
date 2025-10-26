// Main test runner for Cygnus Actor Framework
// All test files are compiled into this single executable

#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>

// Test execution will run in order:
// - 00_basic: Fundamental operations (creation, naming, registry)
// - 01_simple: Simple operations (messages, handlers)
// - 02_interactions: Actor communication
// - 03_lifecycle: Lifecycle management
// - 04_advanced: Complex scenarios
