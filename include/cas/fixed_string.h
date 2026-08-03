#ifndef CAS_FIXED_STRING_H
#define CAS_FIXED_STRING_H

#include <cstring>
#include <string>
#include <string_view>
#include <algorithm>
#include <compare>
#include <stdexcept>
#include <ostream>
#include <type_traits>
#include <functional>   // std::hash
#include <version>      // __cpp_lib_format

#if defined(__cpp_lib_format)
#include <format>
#endif

namespace cas {

/// Fixed-capacity string for zero-allocation messaging
/// Use this instead of std::string in message structs for HFT performance
///
/// Example:
///   struct order_msg : cas::message_base {
///       cas::fixed_string<8> symbol;   // No heap allocation
///       cas::fixed_string<32> client;
///       int64_t quantity;
///   };
///
template<size_t Capacity>
class fixed_string {
public:
    // Types
    using value_type = char;
    using size_type = size_t;
    using iterator = char*;
    using const_iterator = const char*;

    /// Default constructor - empty string
    ///
    /// Only the null terminator is written at runtime. Constant evaluation
    /// cannot read an uninitialised array element, so the whole buffer is
    /// zeroed in that case only - zeroing unconditionally would add a
    /// Capacity-byte write to every message construction on the hot path.
    constexpr fixed_string() noexcept : m_size(0) {
        if (std::is_constant_evaluated()) {
            for (size_t i = 0; i <= Capacity; ++i) {
                m_data[i] = '\0';
            }
        } else {
            m_data[0] = '\0';
        }
    }

    /// Construct from C string
    constexpr fixed_string(const char* str) {
        assign(str);
    }

    /// Construct from std::string
    constexpr fixed_string(const std::string& str) {
        assign(str.data(), str.size());
    }

    /// Construct from std::string_view
    constexpr fixed_string(std::string_view sv) {
        assign(sv.data(), sv.size());
    }

    /// Construct from char array with explicit size
    constexpr fixed_string(const char* str, size_t len) {
        assign(str, len);
    }

    // Assignment
    constexpr fixed_string& operator=(const char* str) {
        assign(str);
        return *this;
    }

    constexpr fixed_string& operator=(const std::string& str) {
        assign(str.data(), str.size());
        return *this;
    }

    constexpr fixed_string& operator=(std::string_view sv) {
        assign(sv.data(), sv.size());
        return *this;
    }

    /// Assign from C string
    constexpr void assign(const char* str) {
        if (str == nullptr) {
            clear();
            return;
        }
        assign(str, std::char_traits<char>::length(str));
    }

    /// Assign from buffer with length
    constexpr void assign(const char* str, size_t len) {
        if (len > Capacity) {
            len = Capacity;  // Truncate silently for performance (no exceptions in hot path)
        }
        if (std::is_constant_evaluated()) {
            // Constant evaluation cannot read an uninitialised array element,
            // and copying a fixed_string reads all of m_data - so during
            // constant evaluation the tail past m_size has to be written too.
            // At runtime this is skipped: only len+1 bytes are touched.
            for (size_t i = 0; i <= Capacity; ++i) {
                m_data[i] = (i < len) ? str[i] : '\0';
            }
            m_size = len;
            return;
        }
        std::char_traits<char>::copy(m_data, str, len);
        m_size = len;
        m_data[m_size] = '\0';
    }

    // Capacity
    constexpr size_t capacity() const noexcept { return Capacity; }
    constexpr size_t max_size() const noexcept { return Capacity; }
    constexpr size_t size() const noexcept { return m_size; }
    constexpr size_t length() const noexcept { return m_size; }
    constexpr bool empty() const noexcept { return m_size == 0; }

    // Element access
    constexpr char& operator[](size_t pos) noexcept { return m_data[pos]; }
    constexpr const char& operator[](size_t pos) const noexcept { return m_data[pos]; }

    constexpr char& at(size_t pos) {
        if (pos >= m_size) throw std::out_of_range("fixed_string::at");
        return m_data[pos];
    }

    constexpr const char& at(size_t pos) const {
        if (pos >= m_size) throw std::out_of_range("fixed_string::at");
        return m_data[pos];
    }

    constexpr char& front() noexcept { return m_data[0]; }
    constexpr const char& front() const noexcept { return m_data[0]; }
    constexpr char& back() noexcept { return m_data[m_size - 1]; }
    constexpr const char& back() const noexcept { return m_data[m_size - 1]; }

    // Data access
    constexpr char* data() noexcept { return m_data; }
    constexpr const char* data() const noexcept { return m_data; }
    constexpr const char* c_str() const noexcept { return m_data; }

    // Conversion
    std::string str() const { return std::string(m_data, m_size); }
    constexpr std::string_view view() const noexcept { return std::string_view(m_data, m_size); }
    constexpr operator std::string_view() const noexcept { return view(); }

    // Iterators
    constexpr iterator begin() noexcept { return m_data; }
    constexpr const_iterator begin() const noexcept { return m_data; }
    constexpr const_iterator cbegin() const noexcept { return m_data; }
    constexpr iterator end() noexcept { return m_data + m_size; }
    constexpr const_iterator end() const noexcept { return m_data + m_size; }
    constexpr const_iterator cend() const noexcept { return m_data + m_size; }

    // Modifiers
    constexpr void clear() noexcept {
        m_size = 0;
        m_data[0] = '\0';
    }

    constexpr void push_back(char c) {
        if (m_size < Capacity) {
            m_data[m_size++] = c;
            m_data[m_size] = '\0';
        }
    }

    constexpr void pop_back() noexcept {
        if (m_size > 0) {
            m_data[--m_size] = '\0';
        }
    }

    constexpr fixed_string& append(const char* str, size_t len) {
        size_t to_copy = std::min(len, Capacity - m_size);
        std::char_traits<char>::copy(m_data + m_size, str, to_copy);
        m_size += to_copy;
        m_data[m_size] = '\0';
        return *this;
    }

    constexpr fixed_string& append(const char* str) {
        return append(str, std::char_traits<char>::length(str));
    }

    constexpr fixed_string& append(std::string_view sv) {
        return append(sv.data(), sv.size());
    }

    constexpr fixed_string& operator+=(char c) {
        push_back(c);
        return *this;
    }

    constexpr fixed_string& operator+=(const char* str) {
        return append(str);
    }

    constexpr fixed_string& operator+=(std::string_view sv) {
        return append(sv);
    }

    // Comparison
    constexpr int compare(std::string_view other) const noexcept {
        return view().compare(other);
    }

    // Comparisons are hidden friends so that both argument orders work
    // (`fs == "x"` and `"x" == fs`). C++20 synthesises !=, <, <=, > and >=
    // from the two operators below, so only these need defining.
    //
    // Do NOT collapse these to a single string_view overload and let the
    // implicit operator std::string_view() handle fixed_string operands: for
    // two different capacities the normal candidate and the C++20 reversed
    // candidate are each better on one argument and worse on the other, which
    // is ambiguous (MSVC C2666). The fixed_string overloads below are exact
    // matches on both arguments, so they outrank that pair and resolve it.
    //
    // Never default these. Defaulted comparisons compare m_data element by
    // element across the whole Capacity+1 array, including the uninitialised
    // bytes past m_size that assign() never writes, so equal strings could
    // compare unequal.
    friend constexpr bool operator==(const fixed_string& lhs, std::string_view rhs) noexcept {
        return lhs.view() == rhs;
    }

    friend constexpr std::strong_ordering operator<=>(const fixed_string& lhs,
                                                      std::string_view rhs) noexcept {
        return lhs.view() <=> rhs;
    }

    // Compare with another fixed_string of any capacity (including this one)
    template<size_t OtherCap>
    friend constexpr bool operator==(const fixed_string& lhs,
                                     const fixed_string<OtherCap>& rhs) noexcept {
        return lhs.view() == rhs.view();
    }

    template<size_t OtherCap>
    friend constexpr std::strong_ordering operator<=>(const fixed_string& lhs,
                                                      const fixed_string<OtherCap>& rhs) noexcept {
        return lhs.view() <=> rhs.view();
    }

private:
    char m_data[Capacity + 1];  // +1 for null terminator
    size_t m_size;
};

// Stream output
template<size_t N>
std::ostream& operator<<(std::ostream& os, const fixed_string<N>& str) {
    return os << str.view();
}

// Concatenation (creates std::string - use sparingly in HFT code)
template<size_t N>
std::string operator+(const fixed_string<N>& lhs, std::string_view rhs) {
    return lhs.str() + std::string(rhs);
}

template<size_t N>
std::string operator+(std::string_view lhs, const fixed_string<N>& rhs) {
    return std::string(lhs) + rhs.str();
}

} // namespace cas

namespace std {

/// Hash support, so fixed_string can be a key in unordered containers.
/// Delegates to hash<string_view> so that a fixed_string and a string_view
/// with the same contents hash equally - which matches the heterogeneous
/// operator== above and is what transparent lookup needs.
template<size_t N>
struct hash<cas::fixed_string<N>> {
    size_t operator()(const cas::fixed_string<N>& s) const noexcept {
        return hash<string_view>{}(s.view());
    }
};

} // namespace std

// std::format support. Guarded because <format> is unavailable on older
// standard libraries that otherwise handle the C++20 used here (notably
// libstdc++ before GCC 13).
#if defined(__cpp_lib_format)

namespace std {

template<size_t N>
struct formatter<cas::fixed_string<N>> : formatter<string_view> {
    template<typename FormatContext>
    auto format(const cas::fixed_string<N>& s, FormatContext& ctx) const {
        return formatter<string_view>::format(s.view(), ctx);
    }
};

} // namespace std

#endif // __cpp_lib_format

#endif // CAS_FIXED_STRING_H
