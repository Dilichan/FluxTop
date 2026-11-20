#pragma once
#include <windows.h>
#include <string>

namespace fluxtop {
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
