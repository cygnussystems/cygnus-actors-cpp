#include "cas/cas.h"
#include <iostream>

struct bench_message : cas::message_base {
    int64_t sequence;
};

struct string_message : cas::message_base {
    std::string symbol;
    std::string client_id;
    int64_t quantity;
    int64_t price;
    int64_t sequence;
};

struct fixed_message : cas::message_base {
    cas::fixed_string<8> symbol;
    cas::fixed_string<16> client_id;
    int64_t quantity;
    int64_t price;
    int64_t sequence;
};

int main() {
    std::cout << "=== Message Sizes ===\n";
    std::cout << "message_base:    " << sizeof(cas::message_base) << " bytes\n";
    std::cout << "bench_message:   " << sizeof(bench_message) << " bytes (minimal)\n";
    std::cout << "string_message:  " << sizeof(string_message) << " bytes (std::string)\n";
    std::cout << "fixed_message:   " << sizeof(fixed_message) << " bytes (fixed_string)\n";
    std::cout << "\n";
    std::cout << "=== String Type Sizes ===\n";
    std::cout << "std::string:        " << sizeof(std::string) << " bytes (+ heap data)\n";
    std::cout << "fixed_string<8>:    " << sizeof(cas::fixed_string<8>) << " bytes (inline)\n";
    std::cout << "fixed_string<16>:   " << sizeof(cas::fixed_string<16>) << " bytes (inline)\n";
    return 0;
}
