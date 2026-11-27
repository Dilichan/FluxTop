#pragma once
#include <windows.h>
#include <algorithm>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fluxtop {
class AppIconCache;
inline constexpr UINT blacklist_icons_ready=WM_APP+120;
inline constexpr int scene_position_units=10000;
struct OverlayScenePosition {
    int x{}, y{};
    std::wstring display;
};
constexpr int scene_axis_range(int length,int extent,int margin) noexcept {
    return std::max(0,length-extent-2*margin);
}
constexpr int scene_axis_offset(int length,int extent,int margin,int position) noexcept {
    return margin+static_cast<int>((static_cast<long long>(scene_axis_range(length,extent,margin))*
        std::clamp(position,0,scene_position_units)+scene_position_units/2)/scene_position_units);
}
constexpr int scene_axis_position(int length,int extent,int margin,int offset) noexcept {
    const auto range=scene_axis_range(length,extent,margin);
    if(!range)return 0;
    const auto distance=std::clamp(offset-margin,0,range);
    return static_cast<int>((static_cast<long long>(distance)*scene_position_units+range/2)/range);
}

struct OverlayApplication {
    std::wstring executable,name,icon_path;
    bool blocked{};
};
inline std::wstring_view overlay_executable_name(std::wstring_view value) noexcept {
    const auto separator=value.find_last_of(L"\\/");
    return separator==value.npos?value:value.substr(separator+1);
}
inline std::optional<std::size_t> overlay_application_index(const std::vector<OverlayApplication>& apps,
    std::wstring_view executable) noexcept {
    if(executable.empty())return {};
    const auto found=std::find_if(apps.begin(),apps.end(),[&](const OverlayApplication& app){
        return CompareStringOrdinal(app.executable.data(),static_cast<int>(app.executable.size()),
            executable.data(),static_cast<int>(executable.size()),TRUE)==CSTR_EQUAL;
    });
    return found==apps.end()?std::nullopt:std::optional{static_cast<std::size_t>(found-apps.begin())};
}
constexpr bool overlay_should_show(bool running,bool always,bool learned,bool blocked) noexcept {
    return running && (always || (learned && !blocked));
}
HMONITOR overlay_display_monitor(std::wstring_view display,HMONITOR fallback);
std::optional<OverlayScenePosition> edit_overlay_scene(HWND owner,HWND overlay,
    std::optional<OverlayScenePosition> initial,int margin);
void edit_overlay_blacklist(HWND owner,const std::vector<OverlayApplication>& apps,AppIconCache& icons,HWND& dialog,
    const std::function<void(std::size_t,bool)>& changed);
} // namespace fluxtop
