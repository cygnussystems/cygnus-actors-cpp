#ifndef CAS_FIXED_STRING_H
#define CAS_FIXED_STRING_H

#include <cstring>
#include <string>
#include <string_view>
#include <algorithm>
#include <stdexcept>
#include <ostream>

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
    constexpr fixed_string() noexcept : m_size(0) {
        m_data[0] = '\0';
    }

    /// Construct from C string
    fixed_string(const char* str) {
        assign(str);
    }

    /// Construct from std::string
    fixed_string(const std::string& str) {
        assign(str.data(), str.size());
    }

    /// Construct from std::string_view
    fixed_string(std::string_view sv) {
        assign(sv.data(), sv.size());
    }

    /// Construct from char array with explicit size
    fixed_string(const char* str, size_t len) {
        assign(str, len);
    }

    // Assignment
    fixed_string& operator=(const char* str) {
        assign(str);
        return *this;
    }

    fixed_string& operator=(const std::string& str) {
        assign(str.data(), str.size());
        return *this;
    }

    fixed_string& operator=(std::string_view sv) {
        assign(sv.data(), sv.size());
        return *this;
    }

    /// Assign from C string
    void assign(const char* str) {
        if (str == nullptr) {
            clear();
            return;
        }
        assign(str, std::strlen(str));
    }

    /// Assign from buffer with length
    void assign(const char* str, size_t len) {
        if (len > Capacity) {
            len = Capacity;  // Truncate silently for performance (no exceptions in hot path)
        }
        std::memcpy(m_data, str, len);
        m_size = len;
        m_data[m_size] = '\0';
    }

    // Capacity
    constexpr size_t capacity() const noexcept { return Capacity; }
    constexpr size_t max_size() const noexcept { return Capacity; }
    size_t size() const noexcept { return m_size; }
    size_t length() const noexcept { return m_size; }
    bool empty() const noexcept { return m_size == 0; }

    // Element access
    char& operator[](size_t pos) noexcept { return m_data[pos]; }
    const char& operator[](size_t pos) const noexcept { return m_data[pos]; }

    char& at(size_t pos) {
        if (pos >= m_size) throw std::out_of_range("fixed_string::at");
        return m_data[pos];
    }

    const char& at(size_t pos) const {
        if (pos >= m_size) throw std::out_of_range("fixed_string::at");
        return m_data[pos];
    }

    char& front() noexcept { return m_data[0]; }
    const char& front() const noexcept { return m_data[0]; }
    char& back() noexcept { return m_data[m_size - 1]; }
    const char& back() const noexcept { return m_data[m_size - 1]; }

    // Data access
    char* data() noexcept { return m_data; }
    const char* data() const noexcept { return m_data; }
    const char* c_str() const noexcept { return m_data; }

    // Conversion
    std::string str() const { return std::string(m_data, m_size); }
    std::string_view view() const noexcept { return std::string_view(m_data, m_size); }
    operator std::string_view() const noexcept { return view(); }

    // Iterators
    iterator begin() noexcept { return m_data; }
    const_iterator begin() const noexcept { return m_data; }
    const_iterator cbegin() const noexcept { return m_data; }
    iterator end() noexcept { return m_data + m_size; }
    const_iterator end() const noexcept { return m_data + m_size; }
    const_iterator cend() const noexcept { return m_data + m_size; }

    // Modifiers
    void clear() noexcept {
        m_size = 0;
        m_data[0] = '\0';
    }

    void push_back(char c) {
        if (m_size < Capacity) {
            m_data[m_size++] = c;
            m_data[m_size] = '\0';
        }
    }

    void pop_back() noexcept {
        if (m_size > 0) {
            m_data[--m_size] = '\0';
        }
    }

    fixed_string& append(const char* str, size_t len) {
        size_t to_copy = std::min(len, Capacity - m_size);
        std::memcpy(m_data + m_size, str, to_copy);
        m_size += to_copy;
        m_data[m_size] = '\0';
        return *this;
    }

    fixed_string& append(const char* str) {
        return append(str, std::strlen(str));
    }

    fixed_string& append(std::string_view sv) {
        return append(sv.data(), sv.size());
    }

    fixed_string& operator+=(char c) {
        push_back(c);
        return *this;
    }

    fixed_string& operator+=(const char* str) {
        return append(str);
    }

    fixed_string& operator+=(std::string_view sv) {
        return append(sv);
    }

    // Comparison
    int compare(std::string_view other) const noexcept {
        return view().compare(other);
    }

    bool operator==(std::string_view other) const noexcept { return view() == other; }
    bool operator!=(std::string_view other) const noexcept { return view() != other; }
    bool operator<(std::string_view other) const noexcept { return view() < other; }
    bool operator<=(std::string_view other) const noexcept { return view() <= other; }
    bool operator>(std::string_view other) const noexcept { return view() > other; }
    bool operator>=(std::string_view other) const noexcept { return view() >= other; }

    // Compare with another fixed_string
    template<size_t OtherCap>
    bool operator==(const fixed_string<OtherCap>& other) const noexcept {
        return view() == other.view();
    }

    template<size_t OtherCap>
    bool operator!=(const fixed_string<OtherCap>& other) const noexcept {
        return view() != other.view();
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

#endif // CAS_FIXED_STRING_H
