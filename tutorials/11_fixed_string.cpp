// Tutorial 11: fixed_string, for messages that do not allocate
//
// TEACHES
//   - why a std::string in a message costs a heap allocation per send
//   - fixed_string<N> as an inline replacement, and its silent truncation
//   - comparison, conversion, hashing, and compile-time use
//
// ASSUMES
//   Tutorials 1-10.
//
// THE IDEA
//   A std::string member keeps its characters on the heap: an allocation on
//   every send, a free on every delivery. fixed_string<N> stores them inside
//   the message instead.

#include "tutorials.h"
#include "cas/cas.h"

#include <atomic>
#include <iostream>
#include <string>
#include <unordered_map>

namespace {

class order_book : public cas::actor {
public:
    // Pick N from the longest value you expect: overshooting costs bytes per
    // message, undershooting silently truncates.
    struct order : cas::message_base {
        cas::fixed_string<8> symbol;    // "AAPL"
        cas::fixed_string<16> trader;   // "desk-emea-01"
        int quantity = 0;
    };

    // fixed_string is constexpr throughout, so it can be a compile-time constant.
    static constexpr cas::fixed_string<8> default_symbol{"AAPL"};

    static inline std::atomic<int> orders{0};            // observed by the tutorial

protected:
    void on_start() override {
        set_name("order_book");
        handler<order>(&order_book::on_order);
    }

private:
    // fixed_string streams and compares like a string: the handler reads no
    // differently from the std::string version.
    void on_order(const order& msg) {
        std::cout << "  order: " << msg.quantity << " " << msg.symbol
                  << " from " << msg.trader << "\n";
        if (msg.symbol == default_symbol && msg.trader == "desk-emea-01") {
            orders.fetch_add(1);
        }
    }
};

// Checked by the compiler: if these were wrong, the file would not build.
static_assert(order_book::default_symbol == "AAPL");
static_assert(order_book::default_symbol.size() == 4);
static_assert(order_book::default_symbol != "MSFT");

} // namespace

namespace tut {

bool run_fixed_string() {
    bool ok = true;

    heading("Basic use");
    cas::fixed_string<16> symbol("AAPL");
    ok &= check(symbol == "AAPL" && "AAPL" == symbol, "compares with a C string on either side");
    ok &= check(symbol.size() == 4, "size() is the content length");
    ok &= check(symbol.capacity() == 16, "capacity() is the declared N");
    // fixed_string<8> and fixed_string<16> are different types, yet compare.
    ok &= check(symbol == cas::fixed_string<8>("AAPL"), "compares equal across capacities");

    heading("Truncation is silent");
    // Too long truncates - no exception, no allocation, no warning. Check the
    // capacity when you do not control the input length.
    cas::fixed_string<4> tiny("ABCDEFGH");
    std::cout << "  fixed_string<4> given \"ABCDEFGH\" holds \"" << tiny << "\"\n";
    ok &= check(tiny == "ABCD" && tiny.size() == 4, "content truncated to the capacity");

    heading("Converting");
    ok &= check(symbol.str() == std::string("AAPL"), "str() copies out to a std::string");
    ok &= check(symbol.view() == "AAPL", "view() gives a string_view, no copy");

    heading("Using it as a map key");
    // Hashes like the equivalent string_view, so equal content hashes equally.
    std::unordered_map<cas::fixed_string<8>, int> position_by_symbol;
    position_by_symbol[cas::fixed_string<8>("AAPL")] = 500;
    position_by_symbol[cas::fixed_string<8>("MSFT")] = 250;
    ok &= check(position_by_symbol[cas::fixed_string<8>("AAPL")] == 500, "lookup by key works");
    ok &= check(position_by_symbol.size() == 2, "two distinct keys stored");

    heading("Sending a message that does not allocate");
    auto book = cas::system::create<order_book>();
    cas::system::start();

    order_book::order o;
    o.symbol = "AAPL";
    o.trader = "desk-emea-01";
    o.quantity = 100;
    book.tell(o);
    ok &= check(wait_until([] { return order_book::orders.load() == 1; }),
                "the order arrived with its strings intact");

    heading("Shut down");
    cas::system::shutdown();
    cas::system::wait_for_shutdown();
    return ok;
}

} // namespace tut
