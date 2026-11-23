#pragma once
#include <cstdint>
#include <cwctype>
#include <filesystem>
#include <string>
namespace fluxtop {
inline std::wstring launcher_lock_name(const std::filesystem::path& directory,bool custom){
    if(!custom)return L"Local\\FluxTop.Bootstrap.v1";
    auto path=std::filesystem::absolute(directory).lexically_normal().wstring();
    std::uint64_t hash=14695981039346656037ULL;
    for(auto c:path){hash^=static_cast<std::uint64_t>(std::towlower(c));hash*=1099511628211ULL;}
    return L"Local\\FluxTop.Bootstrap.Data."+std::to_wstring(hash);
}
}
