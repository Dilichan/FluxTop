#pragma once
#include <windows.h>
#include <string>

namespace fluxtop {
inline std::wstring ini_overlay_read_text(const std::wstring& path,const std::wstring& key) {
    std::wstring text(32768,L'\0');
    text.resize(GetPrivateProfileStringW(L"overlay",key.c_str(),L"",text.data(),static_cast<DWORD>(text.size()),path.c_str()));
    return text;
}
inline void ini_overlay_write_text(const std::wstring& path,const std::wstring& key,const std::wstring& text) {
    WritePrivateProfileStringW(L"overlay",key.c_str(),text.c_str(),path.c_str());
}
// One persistence boundary for the shared [overlay] INI section. Call sites
// supply only the key and typed value; no private Win32 profile API wrappers.
inline UINT ini_overlay_read(const std::wstring& path,const wchar_t* key,UINT fallback) noexcept {
    return GetPrivateProfileIntW(L"overlay",key,fallback,path.c_str());
}
inline int ini_overlay_read_int(const std::wstring& path,const wchar_t* key,int fallback) noexcept {
    return static_cast<int>(ini_overlay_read(path,key,static_cast<UINT>(fallback)));
}
inline void ini_overlay_write(const std::wstring& path,const wchar_t* key,int value) {
    const auto text=std::to_wstring(value);
    WritePrivateProfileStringW(L"overlay",key,text.c_str(),path.c_str());
}
inline void ini_overlay_write(const std::wstring& path,const wchar_t* key,bool value) noexcept {
    WritePrivateProfileStringW(L"overlay",key,value?L"1":L"0",path.c_str());
}
} // namespace fluxtop
