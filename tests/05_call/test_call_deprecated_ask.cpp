// This file deliberately uses the deprecated 1.0 names (ask, ask_handler,
// ask_*_error, ask_queue_high_water_mark) to prove they still build and behave
// exactly like call(), call_handler() and friends. Deprecation warnings are
// expected here and silenced for this file only.
#if defined(_MSC_VER)
#pragma warning(disable : 4996)
#elif defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif

#include "../test_common.h"
#include "cas/ask_message.h"  // deprecated header name must still work

namespace deprecated_ask_test {

struct add_op {};
struct half_op {};

class legacy_calculator : public cas::actor {
protected:
    void on_start() override {
        ask_handler<int, add_op>(&legacy_calculator::add);
        ask_handler<double, half_op>(&legacy_calculator::half);
    }
    int add(int a, int b) { return a + b; }
    double half(double x) { return x / 2.0; }
};

} // namespace deprecated_ask_test

using namespace deprecated_ask_test;

TEST_CASE("Deprecated ask API still works as call", "[05_call][deprecated]") {
    CAS_TEST_GUARD();
    auto calc = cas::system::create<legacy_calculator>();
    cas::system::start();

    REQUIRE(calc.ask<int>(add_op{}, 2, 3) == 5);

    auto timed = calc.ask<int>(add_op{}, std::chrono::milliseconds(2000), 20, 22);
    REQUIRE(timed.has_value());
    REQUIRE(*timed == 42);

    // Old exception name catches the new exception type
    REQUIRE_THROWS_AS(calc.ask<double>(half_op{}, 10), cas::ask_type_error);

    auto& obj = calc.get_checked<legacy_calculator>();
    REQUIRE(obj.ask_queue_high_water_mark() == obj.call_queue_high_water_mark());
}
