#pragma once
#include <windows.h>
#include <array>

namespace fluxtop {
inline constexpr std::array<COLORREF,5> system_chart_colors{
    RGB(14,132,177),RGB(67,105,181),RGB(184,55,114),RGB(131,54,156),RGB(194,89,178)};
inline constexpr COLORREF cpu_temperature_color=RGB(218,94,0);
inline constexpr COLORREF disk_chart_color=RGB(42,132,108);
inline constexpr COLORREF fps_chart_color=RGB(150,84,37);
} // namespace fluxtop
