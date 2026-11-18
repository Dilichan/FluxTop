#pragma once
#include <cstdint>
#include <windows.h>
#include <stdexcept>
#include <string>
#include <utility>

namespace fluxtop {
class Handle {
public:
    explicit Handle(HANDLE value = nullptr) : value_(value) {}
    ~Handle() { if (valid()) CloseHandle(value_); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept : value_(std::exchange(other.value_, nullptr)) {}
    HANDLE get() const { return value_; }
    bool valid() const { return value_ && value_ != INVALID_HANDLE_VALUE; }
private:
    HANDLE value_;
};
inline std::uint64_t filetime_ticks(FILETIME time) {
    return (static_cast<std::uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
}
inline std::runtime_error windows_error(const char* operation) {
    return std::runtime_error(std::string(operation) + " failed (Windows error " + std::to_string(GetLastError()) + ")");
}
inline std::string utf8(const wchar_t* value) {
    const int size = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 0) throw windows_error("WideCharToMultiByte");
    std::string result(static_cast<std::size_t>(size), '\0');
    if (!WideCharToMultiByte(CP_UTF8, 0, value, -1, result.data(), size, nullptr, nullptr))
        throw windows_error("WideCharToMultiByte");
    result.pop_back();
    return result;
}
} // namespace fluxtop
