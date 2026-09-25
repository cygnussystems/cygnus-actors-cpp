#include "../test_common.h"

// Level 5: ask() argument and return type handling
//
// Regression tests for ask() reading its argument tuple through the handler's
// parameter types. The request used to store std::tuple<CallerArgs...> - with
// references for lvalues - and the handler cast it to
// std::tuple<HandlerParams...>, so an lvalue or a converting argument made the
// handler read garbage. Arguments are now stored decayed and any mismatch is
// rejected with cas::ask_type_error.

namespace {

struct typed_add_op {};
struct typed_half_op {};
struct typed_concat_op {};
struct typed_length_op {};

class typed_calculator : public cas::actor {
protected:
    void on_start() override {
        ask_handler<int, typed_add_op>(&typed_calculator::add);
        ask_handler<double, typed_half_op>(&typed_calculator::half);
        ask_handler<std::string, typed_concat_op>(&typed_calculator::concat);
        ask_handler<size_t, typed_length_op>(&typed_calculator::length);
    }

    int add(int a, int b) { return a + b; }
    double half(double x) { return x / 2.0; }
    std::string concat(std::string a, std::string b) { return a + b; }
    size_t length(const std::string& s) { return s.size(); }
};

} // namespace

TEST_CASE("Ask accepts lvalue arguments", "[05_ask_pattern][types]") {
    CAS_TEST_GUARD();
    auto calc = cas::system::create<typed_calculator>();
    cas::system::start();

    int a = 2;
    const int b = 3;
    REQUIRE(calc.ask<int>(typed_add_op{}, a, b) == 5);
    REQUIRE(calc.ask<int>(typed_add_op{}, a, 40) == 42);

    // Arguments are copied: the caller's variables are untouched
    REQUIRE(a == 2);
}

TEST_CASE("Ask with timeout accepts lvalue arguments", "[05_ask_pattern][types]") {
    CAS_TEST_GUARD();
    auto calc = cas::system::create<typed_calculator>();
    cas::system::start();

    int a = 20;
    int b = 22;
    auto result = calc.ask<int>(typed_add_op{}, std::chrono::milliseconds(2000), a, b);
    REQUIRE(result.has_value());
    REQUIRE(*result == 42);
}

TEST_CASE("Ask passes class-type arguments by value and by const reference", "[05_ask_pattern][types]") {
    CAS_TEST_GUARD();
    auto calc = cas::system::create<typed_calculator>();
    cas::system::start();

    std::string left = "foo";
    const std::string right = "bar";
    REQUIRE(calc.ask<std::string>(typed_concat_op{}, left, right) == "foobar");
    REQUIRE(calc.ask<std::string>(typed_concat_op{}, std::string("x"), std::string("y")) == "xy");

    // const std::string& parameter: caller's std::string lvalue decays to match
    REQUIRE(calc.ask<size_t>(typed_length_op{}, left) == 3);
}

TEST_CASE("Ask rejects an argument that would need conversion", "[05_ask_pattern][types]") {
    CAS_TEST_GUARD();
    auto calc = cas::system::create<typed_calculator>();
    cas::system::start();

    // int for a double parameter
    REQUIRE_THROWS_AS(calc.ask<double>(typed_half_op{}, 10), cas::ask_type_error);
    // string literal (const char*) for a std::string parameter
    REQUIRE_THROWS_AS(calc.ask<size_t>(typed_length_op{}, "abc"), cas::ask_type_error);
    // wrong arity
    REQUIRE_THROWS_AS(calc.ask<int>(typed_add_op{}, 1), cas::ask_type_error);

    // Exact types still work, and the actor is unaffected by the rejections
    REQUIRE(calc.ask<double>(typed_half_op{}, 10.0) == 5.0);
}

TEST_CASE("Ask rejects a return type that differs from the handler's", "[05_ask_pattern][types]") {
    CAS_TEST_GUARD();
    auto calc = cas::system::create<typed_calculator>();
    cas::system::start();

    // Handler returns int; asking for double used to reinterpret the int result
    REQUIRE_THROWS_AS(calc.ask<double>(typed_add_op{}, 1, 2), cas::ask_type_error);
}

TEST_CASE("Ask with timeout reports a type mismatch as an exception, not a timeout", "[05_ask_pattern][types]") {
    CAS_TEST_GUARD();
    auto calc = cas::system::create<typed_calculator>();
    cas::system::start();

    REQUIRE_THROWS_AS(
        calc.ask<double>(typed_half_op{}, std::chrono::milliseconds(2000), 10),
        cas::ask_type_error);
}
