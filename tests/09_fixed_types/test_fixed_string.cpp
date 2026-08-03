#include "../test_common.h"
#include "cas/fixed_string.h"
#include <unordered_map>

// Compile-time checks. These fail the BUILD, not the test run, if the
// constexpr support in fixed_string regresses.
namespace {

constexpr cas::fixed_string<8> make_symbol() {
    cas::fixed_string<8> s("AAP");
    s.push_back('L');
    return s;
}

static_assert(cas::fixed_string<8>("AAPL") == "AAPL");
static_assert(cas::fixed_string<8>("AAPL") != "MSFT");
static_assert(cas::fixed_string<8>("AAPL") < "MSFT");
static_assert(make_symbol() == "AAPL");
static_assert(make_symbol().size() == 4);
static_assert(cas::fixed_string<16>().empty());

// Mixed-capacity comparison must also work at compile time.
static_assert(cas::fixed_string<32>("hello") == cas::fixed_string<16>("hello"));
static_assert(cas::fixed_string<32>("apple") < cas::fixed_string<16>("world"));

// Truncation is silent and happens at compile time too.
static_assert(cas::fixed_string<4>("abcdefgh") == "abcd");

} // namespace

TEST_CASE("fixed_string basic operations", "[09_fixed_types][fixed_string]") {
    SECTION("Default construction is empty") {
        cas::fixed_string<32> str;
        REQUIRE(str.empty());
        REQUIRE(str.size() == 0);
        REQUIRE(str.capacity() == 32);
    }

    SECTION("Construct from C string") {
        cas::fixed_string<32> str("hello");
        REQUIRE(str.size() == 5);
        REQUIRE(str == "hello");
        REQUIRE(std::string(str.c_str()) == "hello");
    }

    SECTION("Construct from std::string") {
        std::string s = "world";
        cas::fixed_string<32> str(s);
        REQUIRE(str == "world");
    }

    SECTION("Construct from string_view") {
        std::string_view sv = "test";
        cas::fixed_string<32> str(sv);
        REQUIRE(str == "test");
    }

    SECTION("Assignment operators") {
        cas::fixed_string<32> str;

        str = "hello";
        REQUIRE(str == "hello");

        str = std::string("world");
        REQUIRE(str == "world");

        str = std::string_view("foo");
        REQUIRE(str == "foo");
    }

    SECTION("Truncation on overflow") {
        cas::fixed_string<5> str("hello world");  // Only 5 chars fit
        REQUIRE(str.size() == 5);
        REQUIRE(str == "hello");
    }
}

TEST_CASE("fixed_string element access", "[09_fixed_types][fixed_string]") {
    cas::fixed_string<32> str("hello");

    SECTION("operator[]") {
        REQUIRE(str[0] == 'h');
        REQUIRE(str[4] == 'o');
        str[0] = 'H';
        REQUIRE(str == "Hello");
    }

    SECTION("at() with bounds checking") {
        REQUIRE(str.at(0) == 'h');
        REQUIRE_THROWS_AS(str.at(100), std::out_of_range);
    }

    SECTION("front() and back()") {
        REQUIRE(str.front() == 'h');
        REQUIRE(str.back() == 'o');
    }

    SECTION("data() and c_str()") {
        REQUIRE(std::strcmp(str.data(), "hello") == 0);
        REQUIRE(std::strcmp(str.c_str(), "hello") == 0);
    }
}

TEST_CASE("fixed_string modifiers", "[09_fixed_types][fixed_string]") {
    SECTION("clear()") {
        cas::fixed_string<32> str("hello");
        str.clear();
        REQUIRE(str.empty());
        REQUIRE(str.size() == 0);
    }

    SECTION("push_back() and pop_back()") {
        cas::fixed_string<32> str;
        str.push_back('a');
        str.push_back('b');
        REQUIRE(str == "ab");

        str.pop_back();
        REQUIRE(str == "a");
    }

    SECTION("append()") {
        cas::fixed_string<32> str("hello");
        str.append(" world");
        REQUIRE(str == "hello world");
    }

    SECTION("operator+=") {
        cas::fixed_string<32> str("hi");
        str += '!';
        REQUIRE(str == "hi!");

        str += " there";
        REQUIRE(str == "hi! there");
    }
}

TEST_CASE("fixed_string comparison", "[09_fixed_types][fixed_string]") {
    cas::fixed_string<32> str("hello");

    SECTION("Equality with string_view") {
        REQUIRE(str == "hello");
        REQUIRE(str != "world");
    }

    SECTION("Ordering") {
        REQUIRE(str < "world");
        REQUIRE(str > "apple");
        REQUIRE(str <= "hello");
        REQUIRE(str >= "hello");
    }

    SECTION("Compare with another fixed_string") {
        cas::fixed_string<16> other("hello");
        REQUIRE(str == other);

        cas::fixed_string<16> different("world");
        REQUIRE(str != different);
    }

    // Ordering between two fixed_strings of DIFFERENT capacities. Documented
    // in README but previously untested - and the case that is ambiguous if
    // the mixed-capacity operators are dropped in favour of the implicit
    // string_view conversion. See the comment in fixed_string.h.
    SECTION("Ordering against another fixed_string, different capacity") {
        cas::fixed_string<16> apple("apple");
        cas::fixed_string<16> world("world");

        REQUIRE(str > apple);
        REQUIRE(str < world);
        REQUIRE(str >= apple);
        REQUIRE(str <= world);
    }

    SECTION("Ordering against another fixed_string, same capacity") {
        cas::fixed_string<32> apple("apple");
        cas::fixed_string<32> same("hello");

        REQUIRE(str > apple);
        REQUIRE(str >= same);
        REQUIRE(str <= same);
        REQUIRE_FALSE(str < apple);
    }

    // C++20 synthesises the reversed forms from the operators above. Under
    // C++17 these did not compile at all, since the comparisons were members.
    SECTION("Reversed operand order") {
        REQUIRE("hello" == str);
        REQUIRE("world" != str);
        REQUIRE("apple" < str);
        REQUIRE("world" > str);

        std::string_view sv("hello");
        REQUIRE(sv == str);
        REQUIRE(sv <= str);
    }

    // Guards against anyone "simplifying" the comparisons to = default, which
    // would compare the whole m_data array including uninitialised bytes past
    // m_size. Both strings below hold "ab" but have different residual bytes.
    SECTION("Equality is by content, not by buffer residue") {
        cas::fixed_string<32> a("a much longer initial value");
        a = "ab";

        cas::fixed_string<32> b("ab");

        REQUIRE(a == b);
        REQUIRE(a.size() == b.size());
        REQUIRE((a <=> b) == std::strong_ordering::equal);
    }
}

TEST_CASE("fixed_string hashing", "[09_fixed_types][fixed_string]") {
    SECTION("Usable as an unordered_map key") {
        std::unordered_map<cas::fixed_string<8>, int> by_symbol;
        by_symbol[cas::fixed_string<8>("AAPL")] = 42;
        by_symbol[cas::fixed_string<8>("MSFT")] = 7;

        REQUIRE(by_symbol.size() == 2);
        REQUIRE(by_symbol.at(cas::fixed_string<8>("AAPL")) == 42);
        REQUIRE(by_symbol.at(cas::fixed_string<8>("MSFT")) == 7);
    }

    SECTION("Hash agrees with string_view for equal contents") {
        cas::fixed_string<16> fs("AAPL");
        std::string_view sv("AAPL");

        REQUIRE(std::hash<cas::fixed_string<16>>{}(fs) ==
                std::hash<std::string_view>{}(sv));
    }

    SECTION("Equal contents hash equally across capacities") {
        cas::fixed_string<8> small("AAPL");
        cas::fixed_string<32> large("AAPL");

        REQUIRE(small == large);
        REQUIRE(std::hash<cas::fixed_string<8>>{}(small) ==
                std::hash<cas::fixed_string<32>>{}(large));
    }
}

#if defined(__cpp_lib_format)
TEST_CASE("fixed_string formatting", "[09_fixed_types][fixed_string]") {
    cas::fixed_string<16> str("AAPL");

    SECTION("Plain substitution") {
        REQUIRE(std::format("{}", str) == "AAPL");
    }

    SECTION("Inherits the string_view format spec") {
        REQUIRE(std::format("{:>8}", str) == "    AAPL");
        REQUIRE(std::format("{:*<8}", str) == "AAPL****");
    }
}
#endif

TEST_CASE("fixed_string conversion", "[09_fixed_types][fixed_string]") {
    cas::fixed_string<32> str("hello");

    SECTION("str() returns std::string") {
        std::string s = str.str();
        REQUIRE(s == "hello");
    }

    SECTION("view() returns string_view") {
        std::string_view sv = str.view();
        REQUIRE(sv == "hello");
    }

    SECTION("Implicit conversion to string_view") {
        std::string_view sv = str;
        REQUIRE(sv == "hello");
    }
}

TEST_CASE("fixed_string iterators", "[09_fixed_types][fixed_string]") {
    cas::fixed_string<32> str("hello");

    SECTION("Range-based for loop") {
        std::string result;
        for (char c : str) {
            result += c;
        }
        REQUIRE(result == "hello");
    }

    SECTION("begin() and end()") {
        REQUIRE(*str.begin() == 'h');
        REQUIRE(str.end() - str.begin() == 5);
    }
}

TEST_CASE("fixed_string in message struct", "[09_fixed_types][fixed_string][integration]") {
    // This is the primary use case - zero-allocation messages
    struct order_message : cas::message_base {
        cas::fixed_string<8> symbol;
        cas::fixed_string<16> client_id;
        int64_t quantity;
        int64_t price;
    };

    SECTION("Message struct is fixed size") {
        // Verify no dynamic allocation in struct
        order_message msg;
        msg.symbol = "AAPL";
        msg.client_id = "client123";
        msg.quantity = 100;
        msg.price = 15000;

        REQUIRE(msg.symbol == "AAPL");
        REQUIRE(msg.client_id == "client123");

        // Struct size is compile-time known (no heap pointers)
        static_assert(sizeof(order_message) < 200, "Message should be small fixed size");
    }
}
