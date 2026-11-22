#pragma once
#include "fluxtop/model.hpp"
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace fluxtop {
namespace display_names_detail {
// Input: text from a Windows process name/path. Output: ASCII case-folded text.
inline std::string fold(std::string_view text) {
    std::string result(text);
    for (char& c : result) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return result;
}
// Input: EXE filename. Output: basename without the EXE extension.
inline std::string base_name(std::string name) {
    if (name.size() >= 4 && fold(std::string_view(name).substr(name.size()-4)) == ".exe")
        name.resize(name.size()-4);
    return name;
}
// Input: Windows EXE path and process stem. Output: nearest meaningful parent directories.
inline std::vector<std::string> directories(std::string_view path, std::string_view stem) {
    std::vector<std::string> parts;
    std::size_t pos = 0;
    while (pos < path.size()) {
        const auto end = path.find_first_of("\\/",pos);
        const auto chunk = path.substr(pos, end == std::string_view::npos ? end : end-pos);
        if (!chunk.empty()) parts.emplace_back(chunk);
        if (end == std::string_view::npos) break;
        pos = end + 1;
    }
    if (!parts.empty()) parts.pop_back(); // Executable itself is not a disambiguator.
    std::vector<std::string> choices;
    const auto lower_stem = fold(stem);
    for (auto it = parts.rbegin(); it != parts.rend(); ++it) {
        const auto lower = fold(*it);
        if (lower == lower_stem || lower == "application" || lower == "app" ||
            lower == "bin" || lower == "release" || lower == "debug" ||
            lower == "x64" || lower == "x86" || lower == "win64" ||
            lower == "program files" || lower == "program files (x86)" ||
            (lower.size() == 2 && lower[1] == ':')) continue;
        choices.push_back(*it);
    }
    return choices;
}
// Input: canonical EXE identity. Output: stable small suffix, never the full path.
inline std::string fingerprint(std::string_view identity) {
    std::uint32_t h = 2166136261u;
    for (char c : identity) { h ^= static_cast<unsigned char>(fold(std::string_view(&c,1))[0]); h *= 16777619u; }
    constexpr char digits[]="0123456789abcdef";
    std::string result(8,'0');
    for (int i=7;i>=0;--i) {result[static_cast<std::size_t>(i)] = digits[h & 15u];h >>= 4;}
    return result;
}
} // namespace display_names_detail

// Input: one grouped application and all grouped applications in the snapshot.
// Output: original name unless another distinct group has the same visible name;
// then a directory-derived label. No grouping, measurements or persisted IDs change.
inline std::string application_display_name(const Process& process,
    const std::vector<Process>& groups) {
    using namespace display_names_detail;
    const auto base=base_name(process.name);
    std::vector<const Process*> collisions;
    for (const auto& other:groups)
        if (fold(base_name(other.name))==fold(base)) collisions.push_back(&other);
    if (collisions.size()<2) return base;
    if (process.exe_path && !process.exe_path->empty()) {
        const auto candidates=directories(*process.exe_path,base);
        for (const auto& candidate:candidates) {
            bool unique=true;
            for (const auto* other:collisions) {
                if(other==&process) continue;
                if (!other->exe_path) continue;
                for(const auto& other_candidate:directories(*other->exe_path,base)) {
                    if(fold(candidate)==fold(other_candidate)) {unique=false;break;}
                }
                if(!unique)break;
            }
            if(unique) return base+" ("+candidate+")";
        }
        return base+" ("+fingerprint(*process.exe_path)+")";
    }
    return base+" (PID "+std::to_string(process.pid)+")";
}
} // namespace fluxtop