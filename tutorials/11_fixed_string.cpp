// Tutorial 11: fixed_string, for messages that do not allocate
//
// TEACHES
//   - why std::string in a message costs a heap allocation per send
//   - fixed_string<N> as an inline replacement
//   - silent truncation, and how to avoid being caught by it
//   - comparison, conversion, hashing, and compile-time use
//
// ASSUMES
//   Tutorials 1-10.
//
// THE IDEA
//   Everything so far has used std::string in messages, which is fine until
//   throughput matters. A std::string member allocates on the heap, so every
//   message carrying one costs an allocation on send and a free on delivery.
//   fixed_string<N> stores its characters inside the message instead.

#include "tutorials.h"
#include "cas/cas.h"

#include <atomic>
#include <iostream>
#include <string>
#include <unordered_map>

namespace {

// --- 1. A message that does not allocate -------------------------------------
//
// Pick N from the longest value you actually expect. The cost of overshooting
// is bytes per message; the cost of undershooting is silent truncation.

struct order : cas::message_base {
    cas::fixed_string<8> symbol;    // "AAPL", "MSFT"
    cas::fixed_string<16> trader;   // "desk-emea-01"
    int quantity = 0;
};

std::atomic<int> g_orders_handled{0};

class order_book : public cas::actor {
protected:
    void on_start() override {
        set_name("order_book");
        handler<order>(&order_book::on_order);
    }

    void on_order(const order& msg) {
        // fixed_string streams and compares like a string, so handler code
        // reads no differently from the std::string version.
        std::cout << "  order: " << msg.quantity << " " << msg.symbol
                  << " from " << msg.trader << "\n";
        g_orders_handled.fetch_add(1);
    }
};

// --- 2. Compile-time use -----------------------------------------------------
//
// fixed_string is constexpr throughout, so constants can be built and compared
// at compile time. These are checked by the compiler - if they were wrong, this
// file would not build.

constexpr cas::fixed_string<8> k_default_symbol("AAPL");
static_assert(k_default_symbol == "AAPL");
static_assert(k_default_symbol.size() == 4);
static_assert(k_default_symbol != "MSFT");

} // namespace

namespace tut {

bool run_fixed_string() {
    bool ok = true;

    section("Basic use");

    cas::fixed_string<16> symbol("AAPL");
    ok &= check(symbol == "AAPL", "compares equal to a C string");
    ok &= check(symbol.size() == 4, "size() is the content length, not the capacity");
    ok &= check(symbol.capacity() == 16, "capacity() is the N it was declared with");

    // Comparison works in either direction and across different capacities -
    // useful because a fixed_string<8> and a fixed_string<16> are different
    // types.
    cas::fixed_string<8> shorter("AAPL");
    ok &= check(symbol == shorter, "compares equal across capacities");
    ok &= check("AAPL" == symbol, "compares with the C string on the left");

    // --- 3. Truncation -------------------------------------------------------
    //
    // Assigning more than N characters truncates rather than throwing or
    // allocating. That keeps the hot path free of exceptions, but it means an
    // undersized capacity corrupts data quietly - check capacity when the
    // input length is not something you control.

    section("Truncation is silent");
    cas::fixed_string<4> tiny("ABCDEFGH");
    std::cout << "  fixed_string<4> given \"ABCDEFGH\" holds \"" << tiny << "\"\n";
    ok &= check(tiny == "ABCD", "content was truncated to the capacity");
    ok &= check(tiny.size() == 4, "size() reflects the truncated length");

    // --- 4. Conversion -------------------------------------------------------

    section("Converting");
    std::string as_string = symbol.str();
    ok &= check(as_string == "AAPL", "str() copies out to a std::string");

    std::string_view as_view = symbol.view();
    ok &= check(as_view == "AAPL", "view() gives a view with no copy");

    // --- 5. As a map key -----------------------------------------------------
    //
    // fixed_string hashes consistently with string_view, so equal content
    // always hashes equally - including across different capacities.

    section("Using it as a map key");
    std::unordered_map<cas::fixed_string<8>, int> position_by_symbol;
    position_by_symbol[cas::fixed_string<8>("AAPL")] = 500;
    position_by_symbol[cas::fixed_string<8>("MSFT")] = 250;

    ok &= check(position_by_symbol[cas::fixed_string<8>("AAPL")] == 500,
                "lookup by fixed_string key works");
    ok &= check(position_by_symbol.size() == 2, "two distinct keys stored");

    // --- 6. In an actual message ---------------------------------------------

    section("Sending a message that does not allocate");
    auto book = cas::system::create<order_book>();
    cas::system::start();

    order o;
    o.symbol = "AAPL";
    o.trader = "desk-emea-01";
    o.quantity = 100;
    book.tell(o);

    ok &= check(wait_until([] { return g_orders_handled.load() == 1; }),
                "the order was delivered with its strings intact");

    section("Shutting down");
    cas::system::shutdown();
    cas::system::wait_for_shutdown();

    return ok;
}

} // namespace tut
