#pragma once
// FluxTop overlay table geometry. All editable spacing/width coefficients live here.
// Values are multipliers of the selected font size unless marked as DIP.
#include <array>

namespace fluxtop {

struct OverlayLayoutConfig {
    // Width of the application-name column, in font-size units.
    float application_width = 10.8f; // Wider heading accommodates foreground FPS.
    // Space reserved for the application icon, in font-size units.
    float icon_width = 1.7f;
    // Gap between adjacent VISIBLE columns, in font-size units.
    float column_gap = 0.45f;
    // Left/right padding, in device-independent pixels (DIP), not font units.
    float left_padding = 2.0f;
    float right_padding = 2.0f;
    // CPU, RAM, GPU, GPU RAM, Rx/Tx, in font-size units.
    std::array<float, 5> metric_widths{8.0f, 5.8f, 8.0f, 9.0f, 12.0f};
    float disk_width = 9.0f; // Per-volume system activity; dynamic columns follow the existing five.
};

// THE SINGLE PLACE TO CHANGE OVERLAY WIDTHS AND GAPS.
inline constexpr OverlayLayoutConfig overlay_layout{};
inline constexpr std::array<const wchar_t*, 5> metric_names{
    L"CPU", L"RAM", L"GPU", L"GPU RAM", L"Rx/Tx"
};

struct OverlayColumnPositions {
    // 0 = application, 1..5 = five resource metrics.
    std::array<float, 6> left{};
    std::array<float, 6> right{};
    float total_width{};
};

// Calculate both cell coordinates and window width from the SAME geometry.
// No hard-coded or duplicated per-column minimum widths are applied.
constexpr OverlayColumnPositions calculate_overlay_columns(
    float font_size, unsigned visible_metrics, bool summary_only, bool show_icons) noexcept {
    OverlayColumnPositions result{};
    float cursor = overlay_layout.left_padding;
    bool previous_visible = false;
    if (!summary_only) {
        if (show_icons) cursor += overlay_layout.icon_width * font_size;
        result.left[0] = cursor;
        cursor += overlay_layout.application_width * font_size;
        result.right[0] = cursor;
        previous_visible = true;
    }
    for (unsigned index = 0; index < overlay_layout.metric_widths.size(); ++index) {
        if ((visible_metrics & (1U << index)) == 0) continue;
        if (previous_visible) cursor += overlay_layout.column_gap * font_size;
        result.left[index + 1] = cursor;
        cursor += overlay_layout.metric_widths[index] * font_size;
        result.right[index + 1] = cursor;
        previous_visible = true;
    }
    result.total_width = cursor + overlay_layout.right_padding;
    return result;
}

static_assert(metric_names.size() == overlay_layout.metric_widths.size());
static_assert(overlay_layout.metric_widths[0] > 0.0f);
static_assert(overlay_layout.metric_widths[3] > 0.0f);
static_assert(calculate_overlay_columns(16.0f, 31U, false, true).total_width >
    calculate_overlay_columns(16.0f, 31U, true, false).total_width);

} // namespace fluxtop
