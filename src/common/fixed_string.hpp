#pragma once

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <functional>
#include <string_view>

// 定长字符串模板类，用于共享内存结构
// 不使用 std::string 以避免动态内存分配
template <std::size_t N>
struct FixedString {
    char data[N]{};

    constexpr FixedString() = default;

    FixedString(std::string_view sv) { assign(sv); }

    void assign(std::string_view sv) {
        std::size_t len = std::min(sv.size(), N - 1);
        std::memcpy(data, sv.data(), len);
        data[len] = '\0';
    }

    void clear() { std::memset(data, 0, N); }

    std::string_view view() const { return std::string_view(data); }

    const char *c_str() const { return data; }

    std::size_t size() const { return std::strlen(data); }

    std::size_t capacity() const { return N - 1; }

    bool empty() const { return data[0] == '\0'; }

    bool operator==(const FixedString &other) const { return std::strcmp(data, other.data) == 0; }

    bool operator==(std::string_view sv) const { return view() == sv; }

    bool operator!=(const FixedString &other) const { return !(*this == other); }

    bool operator<(const FixedString &other) const { return std::strcmp(data, other.data) < 0; }
};

namespace std {

template <std::size_t N>
struct hash<FixedString<N>> {
    std::size_t operator()(const FixedString<N>& value) const noexcept {
        return std::hash<std::string_view>{}(value.view());
    }
};

}  // namespace std
