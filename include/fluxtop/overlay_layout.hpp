#pragma once
// FluxTop overlay table geometry. All editable spacing/width coefficients live here.
// Values are multipliers of the selected font size unless marked as DIP.
#include <algorithm>
#include <array>

namespace fluxtop {

struct OverlayLayoutConfig {
    // Width of the application-name column, in font-size units.
    float application_width = 10.8f; // Wider heading accommodates foreground FPS.
    float summary_fps_width = 5.2f; // FPS occupies the first header cell in summary mode.
    // Space reserved for the application icon, in font-size units.
    float icon_width = 1.7f;
    // Gap between adjacent VISIBLE columns, in font-size units.
    float column_gap = 0.45f;
    // Left/right padding, in device-independent pixels (DIP), not font units.
    float left_padding = 2.0f;
    float right_padding = 2.0f;
    // CPU, RAM, GPU, GPU RAM, Rx/Tx, in font-size units.
    std::array<float, 5> metric_widths{9.0f, 6.8f, 8.0f, 10.0f, 12.0f};
    float disk_width = 7.0f; // Read/write throughput columns, one font unit narrower.
    float cpu_core_width = 13.0f; // Shared width of all physical-core columns.
};

// THE SINGLE PLACE TO CHANGE OVERLAY WIDTHS AND GAPS.
inline constexpr OverlayLayoutConfig overlay_layout{};

// Width already fits inside the work area including the margins.
constexpr int overlay_fitted_left(int right_anchor, int width, int work_left, int work_right, int margin) noexcept {
    return std::clamp(right_anchor, work_left + margin + width, work_right - margin) - width;
}

inline constexpr std::array<const wchar_t*, 5> metric_names{
    L"CPU", L"RAM", L"GPU", L"GPU RAM", L"Rx/Tx"
};

struct OverlayColumnPositions {
    // 0 = application or summary FPS, 1..5 = five resource metrics.
    std::array<float, 6> left{};
    std::array<float, 6> right{};
    float total_width{};
};

struct OverlayColumnRun {
    float first{}, width{}, gap{};
    constexpr float left(std::size_t index) const noexcept {
        return first + static_cast<float>(index) * (width + gap);
    }
    constexpr float right(std::size_t index) const noexcept { return left(index) + width; }
};
struct OverlayTableGeometry {
    OverlayColumnPositions base;
    OverlayColumnRun disks, cores;
    float total_width{};
};

// Calculate both cell coordinates and window width from the SAME geometry.
// No hard-coded or duplicated per-column minimum widths are applied.
constexpr OverlayColumnPositions calculate_overlay_columns(
    float font_size, unsigned visible_metrics, bool summary_only, bool show_icons) noexcept {
    OverlayColumnPositions result{};
    float cursor = overlay_layout.left_padding;
    if (!summary_only && show_icons) cursor += overlay_layout.icon_width * font_size;
    result.left[0] = cursor;
    cursor += (summary_only?overlay_layout.summary_fps_width:overlay_layout.application_width)*font_size;
    result.right[0] = cursor;
    for (unsigned index = 0; index < overlay_layout.metric_widths.size(); ++index) {
        if ((visible_metrics & (1U << index)) == 0) continue;
        cursor += overlay_layout.column_gap * font_size;
        result.left[index + 1] = cursor;
        cursor += overlay_layout.metric_widths[index] * font_size;
        result.right[index + 1] = cursor;
    }
    result.total_width = cursor + overlay_layout.right_padding;
    return result;
}

constexpr OverlayTableGeometry calculate_overlay_table(float font_size, unsigned visible_metrics,
    bool summary_only, bool show_icons, std::size_t disk_count, std::size_t core_count) noexcept {
    const auto base=calculate_overlay_columns(font_size,visible_metrics,summary_only,show_icons);
    const float gap=overlay_layout.column_gap*font_size;
    const float first=base.total_width-overlay_layout.right_padding+gap;
    const OverlayColumnRun disks{first,overlay_layout.disk_width*font_size,gap};
    const OverlayColumnRun cores{disks.left(disk_count),overlay_layout.cpu_core_width*font_size,gap};
    const float total=core_count?cores.right(core_count-1)+overlay_layout.right_padding:
        disk_count?disks.right(disk_count-1)+overlay_layout.right_padding:base.total_width;
    return {base,disks,cores,total};
}

static_assert(metric_names.size() == overlay_layout.metric_widths.size());
static_assert(overlay_layout.metric_widths[0] > 0.0f);
static_assert(overlay_layout.metric_widths[3] > 0.0f);
static_assert(overlay_layout.cpu_core_width > 0.0f);
static_assert(overlay_layout.summary_fps_width > 0.0f);
static_assert(calculate_overlay_columns(16.0f, 31U, false, true).total_width >
    calculate_overlay_columns(16.0f, 31U, true, false).total_width);

} // namespace fluxtop
