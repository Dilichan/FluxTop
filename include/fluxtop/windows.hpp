#pragma once
#include <cstdint>
#include <windows.h>
#include <stdexcept>
#include <string>
#include <string_view>
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
inline std::wstring process_image_path(HANDLE process) {
    wchar_t buffer[512]{};
    DWORD length=static_cast<DWORD>(_countof(buffer));
    if(QueryFullProcessImageNameW(process,0,buffer,&length))return {buffer,length};
    if(GetLastError()!=ERROR_INSUFFICIENT_BUFFER)return {};
    std::wstring path(32768,L'\0');
    length=static_cast<DWORD>(path.size());
    if(!QueryFullProcessImageNameW(process,0,path.data(),&length))return {};
    path.resize(length);
    return path;
}
inline std::runtime_error windows_error(const char* operation) {
    return std::runtime_error(std::string(operation) + " failed (Windows error " + std::to_string(GetLastError()) + ")");
}
// Display text may contain invalid UTF-8 from third-party processes; render a
// replacement marker rather than failing the entire overlay. Git parsing uses
// its own strict conversion because corrupt Git output must fail explicitly.
inline std::wstring utf8_to_wide(std::string_view value,bool sanitize_controls=false) {
    if(value.empty())return {};
    const int count=MultiByteToWideChar(CP_UTF8,0,value.data(),
        static_cast<int>(value.size()),nullptr,0);
    if(count<=0)return L"?";
    std::wstring text(static_cast<std::size_t>(count),L'\0');
    if(!MultiByteToWideChar(CP_UTF8,0,value.data(),
        static_cast<int>(value.size()),text.data(),count))return L"?";
    if(sanitize_controls)for(auto& c:text)if(c<32)c=L' ';
    return text;
}
// Keep class registration and ERROR_CLASS_ALREADY_EXISTS handling in one spot.
inline void register_window_class(const WNDCLASSEXW& descriptor,const char* operation) {
    if(!RegisterClassExW(&descriptor) && GetLastError()!=ERROR_CLASS_ALREADY_EXISTS)
        throw windows_error(operation);
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
