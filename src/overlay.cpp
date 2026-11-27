#include "fluxtop/overlay.hpp"
#include "fluxtop/application_display_name.hpp"
#include "fluxtop/diagnostics.hpp"
#include "fluxtop/overlay_layout.hpp"
#include "fluxtop/overlay_charts.hpp"
#include "fluxtop/overlay_scene.hpp"
#include "fluxtop/cpu_view.hpp"

#include "fluxtop/statistics_page.hpp"
#include "fluxtop/statistics_colors.hpp"
#include "fluxtop/app_icons.hpp"
#include "fluxtop/autostart.hpp"
#include "fluxtop/history.hpp"
#include "fluxtop/sampling_worker.hpp"
#include "fluxtop/windows.hpp"
#include "fluxtop/metric_text.hpp"
#include "fluxtop/overlay_ini.hpp"
#include "fluxtop/control_wheel.hpp"
#include "fluxtop/resource_ids.h"

#include <windowsx.h>
#include <commctrl.h>
#include <shellapi.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <unordered_map>
#include <tuple>

namespace fluxtop {
namespace {
using Microsoft::WRL::ComPtr;
constexpr wchar_t overlay_class[] = L"FluxTop.Overlay.Window";
constexpr wchar_t client_class[] = L"FluxTop.Client.Window";
constexpr UINT sample_message = WM_APP + 10, tray_message = WM_APP + 11, icon_message = WM_APP + 12;
constexpr UINT foreground_update_message = WM_APP + 13;
constexpr int toggle_hotkey = 1, client_hotkey = 2;
enum Control : UINT { open_client = 100, toggle_table, quit_app, top_label, top_control, sort_control, font_control,
    alpha_control, visible_control, move_control, icons_control, stats_control, startup_control,
    alpha_label, interval_control, summary_control, header_colors_control, row_colors_control, animation_control, logs_control, metric_control = 200, disk_checkbox_control = 300, disk_total_checkbox_control = 326, tab_control = 450, font_label = 500, interval_label = 501,
    cpu_package_control=604,cpu_max_control,cpu_average_control,cpu_checkbox_control=700,
    blacklist_button=1800, table_options_group, metrics_options_group, chart_options_group, diagnostic_options_group,
    next_chart_page, next_table_page, chart_heading_control, manual_move_control,
    chart_checkbox_control=2000 };
constexpr unsigned all_metrics = 31; // Five resource columns; FPS is in the application heading.
constexpr unsigned default_metrics = 31;
constexpr int interval_step_ms = 500;
constexpr int interval_first_step = 1;
constexpr int interval_last_step = 10000 / interval_step_ms;
constexpr int interval_position(unsigned milliseconds) noexcept {
    return std::clamp(static_cast<int>((milliseconds + interval_step_ms / 2) / interval_step_ms),
        interval_first_step, interval_last_step);
}
std::wstring interval_caption(unsigned milliseconds) {
    auto seconds = std::to_wstring(milliseconds / 1000);
    if (const auto remainder = milliseconds % 1000) {
        auto fraction = std::to_wstring(1000 + remainder).substr(1);
        fraction.erase(fraction.find_last_not_of(L'0') + 1);
        seconds += L"," + fraction;
    }
    return L"Сбор и обновление: " + seconds + L" с (0,5–10)";
}
constexpr std::array<Sort, 8> sort_ui_values{Sort::automatic, Sort::cpu, Sort::ram, Sort::gpu,
    Sort::vram, Sort::rx, Sort::tx, Sort::network};
constexpr std::size_t sort_ui_index(Sort value) noexcept {
    for (std::size_t index = 0; index < sort_ui_values.size(); ++index)
        if (sort_ui_values[index] == value) return index;
    return 0;
}
static_assert(sort_ui_index(Sort::automatic) == 0 && sort_ui_index(Sort::cpu) == 1 &&
    sort_ui_index(Sort::gpu) == 3 && sort_ui_index(Sort::network) == 7);
constexpr int load_band(double x) { return x < 50.0 ? 0 : x < 70.0 ? 1 : 2; }
D2D1_COLOR_F overlay_band_color(int band) {
    return band==0?D2D1::ColorF(0.25f,0.92f,0.35f,1)
        :band==1?D2D1::ColorF(1.0f,0.80f,0.13f,1)
        :D2D1::ColorF(1.0f,0.22f,0.22f,1);
}
D2D1_COLOR_F overlay_load_color(std::optional<double> load) {
    if(!load || !std::isfinite(*load) || *load<0.0)return D2D1::ColorF(1,1,1,1);
    return overlay_band_color(load_band(*load));
}
constexpr int temperature_band(double x) { return x < 65.0 ? 0 : x <= 80.0 ? 1 : 2; }
D2D1_COLOR_F chart_ink(COLORREF color) {
    return D2D1::ColorF(GetRValue(color)/255.0f,GetGValue(color)/255.0f,GetBValue(color)/255.0f);
}
COLORREF overlay_chart_series_color(OverlayChartKind kind) {
    switch(kind) {
    case OverlayChartKind::ram:return system_chart_colors[1];
    case OverlayChartKind::network:return system_chart_colors[2];
    case OverlayChartKind::gpu:return system_chart_colors[3];
    case OverlayChartKind::vram:return system_chart_colors[4];
    case OverlayChartKind::fps:return fps_chart_color;
    case OverlayChartKind::disk:return disk_chart_color;
    case OverlayChartKind::cpu_temperature:return cpu_temperature_color;
    case OverlayChartKind::cpu:
    case OverlayChartKind::core:return system_chart_colors[0];
    }
    return system_chart_colors[0];
}
static_assert(load_band(49.9) == 0 && load_band(50.0) == 1 && load_band(70.0) == 2);
static_assert(temperature_band(64.9) == 0 && temperature_band(65.0) == 1
    && temperature_band(80.0) == 1 && temperature_band(80.1) == 2);
constexpr UINT_PTR settings_timer = 1;
constexpr UINT_PTR overlay_animation_timer = 2;
// Timer wakes are cheap; paint scheduling uses actual frame cost and elapsed time.
constexpr UINT overlay_animation_timer_ms = 40; // 25 FPS maximum; stops at transition end.
constexpr unsigned overlay_transition_budget_ms(unsigned sampling_ms) noexcept {
    return sampling_ms - std::min(sampling_ms / 2, 2 * overlay_animation_timer_ms);
}
constexpr unsigned overlay_transition_duration_ms(float slot_travel,unsigned sampling_ms) noexcept {
    const auto distance_ms = static_cast<unsigned>(280.0f + 6.0f * std::clamp(slot_travel,0.0f,40.0f));
    return std::min(distance_ms, overlay_transition_budget_ms(sampling_ms));
}
static_assert(overlay_transition_duration_ms(0.0f,1000)==280 &&
    overlay_transition_duration_ms(10.0f,1000)==340 &&
    overlay_transition_duration_ms(100.0f,1000)==520 &&
    overlay_transition_duration_ms(100.0f,500)==420 &&
    overlay_transition_duration_ms(100.0f,250)==170);
// Animated display only: sampling cadence and AutoScore are unchanged.
constexpr float overlay_smoothstep(float value) noexcept {
    const float t=std::clamp(value,0.0f,1.0f);
    return t*t*(3.0f-2.0f*t);
}
static_assert(overlay_smoothstep(0.0f)==0.0f && overlay_smoothstep(1.0f)==1.0f);

// An out-of-context WinEvent hook delivers foreground changes on this UI thread.
// The callback only posts a message: resource collection is never done in the hook.
thread_local HWND foreground_update_target{};
void CALLBACK foreground_changed(HWINEVENTHOOK, DWORD event, HWND, LONG, LONG, DWORD, DWORD) {
    if (event == EVENT_SYSTEM_FOREGROUND && foreground_update_target)
        PostMessageW(foreground_update_target,foreground_update_message,0,0);
}

std::wstring wide(std::string_view value) { return utf8_to_wide(value,true); }
using metric_text::decimal;
using metric_text::percent;
using metric_text::memory;
using metric_text::network_rate;
void require_hr(HRESULT result, const char* operation) {
    if (FAILED(result)) throw std::runtime_error(std::string(operation) + " failed (HRESULT " + std::to_string(static_cast<unsigned long>(result)) + ")");
}
std::wstring env(const wchar_t* name) {
    std::wstring value(32768, L'\0');
    const auto size = GetEnvironmentVariableW(name, value.data(), static_cast<DWORD>(value.size()));
    if (!size || size >= value.size()) return {};
    value.resize(size); return value;
}
std::filesystem::path local_file(const wchar_t* override_name, const wchar_t* filename) {
    if (auto value = env(override_name); !value.empty()) return std::filesystem::absolute(value);
    const auto base = env(L"LOCALAPPDATA");
    return base.empty() ? std::filesystem::path{} : std::filesystem::path(base) / L"FluxTop" / filename;
}

struct Row {
    std::uint32_t pid{};
    std::optional<std::uint64_t> created;
    std::wstring name, cpu, ram, gpu, vram, network;
    std::optional<double> disk_read,disk_write;
    std::array<std::optional<double>,26> disk_volumes{};
    // Utilization is stored per cell for load colors.
    std::array<std::optional<double>,4> loads{}; // CPU / RAM / GPU / GPU RAM
    std::vector<std::optional<double>> logical_cpu;
    std::wstring icon_path;
    bool operator==(const Row&) const = default;
};

class Overlay {
public:
    explicit Overlay(Options options) : options_(options) {}
    ~Overlay();
    int run();
private:
    static LRESULT CALLBACK dispatch(HWND window, UINT message, WPARAM wp, LPARAM lp);
    static LRESULT CALLBACK client_dispatch(HWND window, UINT message, WPARAM wp, LPARAM lp);
    LRESULT message(UINT message, WPARAM wp, LPARAM lp);
    LRESULT client_message(HWND window, UINT message, WPARAM wp, LPARAM lp);
    void load_settings();
    void save_settings() noexcept;
    void load_icons();
    void fonts();
    void add_tray();
    void tray_menu();
    void show_client();
    void switch_page(unsigned page);
    void init_client();
    void layout_client();
    void apply_client();
    void refresh_disk_controls();
    bool refresh_chart_choices(bool create_controls=false);
    void update_selected_charts();
    void draw_overlay_charts(float width,float top);
    std::vector<wchar_t> enabled_disk_letters() const;
    void cpu_settings_changed();
    void refresh_cpu_controls(bool force=false);
    void client_stats();
    HWND control(const wchar_t* kind, const wchar_t* label, DWORD style, UINT id, int x, int y, int width, int height);
    void set_visible(bool visible);
    void update_overlay_visibility();
    void learn_foreground_application();
    void save_application(std::size_t index);
    void ensure_icon_cache();
    void release_unused_icon_cache();
    void activity();
    void receive();
    void update_foreground();
    void update_view();
    void fit_table();
    void set_manual_movement(bool enabled);
    void release_surface();
    void surface();
    // Animation ticks repaint only the moving row area; ordinary updates repaint everything.
    void paint(bool animation_only = false);
    void draw_text(std::wstring_view value, D2D1_RECT_F rect, IDWriteTextFormat* font,
        std::optional<double> load = std::nullopt);
    void draw_header_metric(unsigned column, const std::wstring& reading, std::optional<double> load,
        std::optional<double> temperature, D2D1_RECT_F rect);
    struct ReadingTint { std::size_t first{}, count{}; D2D1_COLOR_F color{}; };
    void draw_heading(std::wstring_view line,D2D1_RECT_F rect,IDWriteTextFormat* font,
        const std::array<ReadingTint,2>& tints);
    void draw_cached_row_text(const Row& row, unsigned column, std::wstring_view value,
        D2D1_RECT_F rect, IDWriteTextFormat* font, bool highlight = false,
        std::optional<double> load = std::nullopt, ID2D1RenderTarget* destination = nullptr,
        ID2D1SolidColorBrush* destination_brush = nullptr);
    void draw_base_row(const Row& row,const OverlayColumnPositions& geometry,float y,
        ID2D1RenderTarget* destination=nullptr,ID2D1SolidColorBrush* ink=nullptr);
    ID2D1Bitmap* cached_row_bitmap(const Row& row, const OverlayColumnPositions& geometry,
        std::size_t& built_this_frame);
    void clear_row_images() noexcept;
    float text_size() const { return static_cast<float>(options_.font_size); }
    float row_height() const { return text_size() * 1.6f; }
    bool has_enabled_disks() const {
        return std::any_of(available_disk_letters_.begin(),available_disk_letters_.end(),
            [this](wchar_t letter){return (selected_disks_mask_ & (1U<<(letter-L'A')))!=0;});
    }
    bool summary_only() const { return summary_ || (!metrics_ && !has_enabled_disks() && !disk_total_enabled_ && cpu_columns_.empty()); }
    unsigned active_metrics() const { return metrics_ ? metrics_ : ((has_enabled_disks() || disk_total_enabled_ || !cpu_columns_.empty())?0U:all_metrics); }
    unsigned ranking_metrics() const {
        // Display switches only affect columns; AutoScore always uses every available metric.
        return auto_score_all_metrics;
    }
    OverlayTableGeometry table_geometry(std::size_t disk_count) const {
        return calculate_overlay_table(text_size(),active_metrics(),summary_only(),show_icons_,
            disk_count+(disk_total_enabled_?1U:0U),cpu_columns_.size());
    }
    float table_width() const { return table_geometry(enabled_disk_letters().size()).total_width; }
    float dip(int px) const { return static_cast<float>(px) * 96.0f / static_cast<float>(dpi_); }
    int pixels(float value) const { return static_cast<int>(value * static_cast<float>(dpi_) / 96.0f + .5f); }
    std::size_t visible_rows(float height) const {
        return static_cast<std::size_t>(std::max(0.0f, (height - chart_layout_.height - row_height() - 4 + .01f) / row_height()));
    }

    Options options_;
    HWND hwnd_{}, client_{}, tabs_{};
    unsigned client_page_{};
    std::unique_ptr<StatisticsPage> statistics_;
    std::shared_ptr<SystemGraphHistory> graph_history_=std::make_shared<SystemGraphHistory>();
    CpuViewSettings cpu_view_;
    std::vector<CpuViewColumn> cpu_columns_;
    HWND cpu_label_{},cpu_temperature_label_{};
    std::vector<std::pair<HWND,std::size_t>> cpu_checks_;
    std::vector<CpuViewId> cpu_topology_ids_;
    HICON icon_{}, large_icon_{};
    HWINEVENTHOOK foreground_hook_{};
    DWORD focused_pid_{}; // Auto-ranked pinned application.
    DWORD foreground_pid_{}; // Actual active window PID (any sorting mode).
    HWND foreground_window_{};
    std::uint64_t foreground_epoch_{};
    std::optional<double> foreground_fps_; // Only the currently focused PID.
    HFONT client_font_{};
    UINT dpi_ = 96, client_dpi_ = 96, taskbar_created_{};
    bool tray_ = false, toggle_key_ = false, client_key_ = false, visible_ = false;
    bool overlay_running_=true, always_show_=false, inspect_overlay_=false;
    bool show_icons_ = true, stopping_ = false, initializing_client_ = false;
    std::optional<OverlayScenePosition> scene_position_;
    bool manual_movement_{}, dragging_{};
    HWND scene_dialog_{};
    std::vector<OverlayApplication> learned_apps_;
    std::optional<std::size_t> foreground_app_;
    std::optional<std::uint64_t> foreground_app_creation_;
    bool migrate_app_settings_{};
    HWND blacklist_dialog_{};
    unsigned opacity_ = 100;
    unsigned metrics_ = default_metrics;
    unsigned selected_disks_mask_{}; // 26 letters, persisted even when a drive disappears.
    bool disk_total_enabled_{true};
    HWND disk_total_check_{};
    std::vector<wchar_t> available_disk_letters_;
    std::array<HWND,26> disk_checks_{};
    HWND chart_settings_label_{};
    std::vector<HWND> chart_checks_;
    std::vector<OverlayChart> chart_choices_;
    std::vector<std::size_t> selected_charts_;
    std::vector<wchar_t> chart_disks_;
    std::vector<CpuViewId> chart_cores_;
    OverlayChartLayout chart_layout_;
    std::size_t chart_page_{};
    HWND shortcuts_label_{};
    bool summary_ = false;
    bool header_colors_ = true, row_colors_ = true;
    bool animation_enabled_ = true;
    int x_ = CW_USEDEFAULT, y_ = CW_USEDEFAULT;
    int right_anchor_ = CW_USEDEFAULT;
    std::wstring settings_;
    std::size_t scroll_{};
    std::uint64_t disk_delivery_ui_samples_{}; // Diagnostics, not rendering state.
    int exit_code_{};
    struct Child { HWND window; RECT bounds; UINT group; };
    std::vector<Child> controls_;
    UINT open_group_{};
    SamplingWorker worker_;
    SpikeTracker grouped_spikes_; // Updated once per new system snapshot, never per foreground event.
    std::vector<Process> grouped_processes_;
    std::shared_ptr<AppIconCache> icons_;
    std::atomic<bool> icon_posted_{false};
    std::mutex mailbox_mutex_;
    std::optional<Snapshot> pending_, snapshot_;
    std::string pending_error_, error_;
    bool posted_ = false, pending_finished_ = false;
    std::vector<Row> rows_;
    struct RowMotion { float start_slot{}; int previous_index{-1}; };
    // Only the previous visible-ranking snapshot is retained for one transition.
    std::vector<Row> previous_rows_;
    std::vector<RowMotion> row_motion_;
    ULONGLONG row_animation_started_{};
    unsigned row_animation_duration_ms_{650};
    float row_animation_last_painted_progress_{};
    ULONGLONG row_animation_next_frame_tick_{};
    bool row_animation_slow_frame_reported_{};
    bool row_animation_active_{};
    // D2D bitmaps are render-target resources: reuse until the icon or target changes.
    struct CachedBitmap {
        std::optional<std::uint64_t> created;
        std::shared_ptr<const AppIcon> source;
        ComPtr<ID2D1Bitmap> bitmap;
        std::uint64_t used{};
    };
    std::unordered_map<std::uint32_t,CachedBitmap> icon_bitmaps_;
    std::uint64_t icon_bitmap_clock_{};
    struct CachedRowTextLayouts {
        std::optional<std::uint64_t> created;
        std::array<std::wstring,6> values;
        std::array<float,6> widths{};
        std::array<float,6> heights{};
        std::array<ComPtr<IDWriteTextLayout>,6> layouts;
        std::uint64_t used{};
    };
    // A bounded number of visible applications * 6 layouts, evicted by LRU.
    std::unordered_map<std::uint32_t,CachedRowTextLayouts> row_text_cache_;
    std::uint64_t row_text_clock_{};
    struct CachedRowImage {
        Row content;
        ComPtr<ID2D1Bitmap> bitmap;
        std::size_t bytes{};
    };
    // Images exist only while rows move; at most 6 MiB, never a permanent 100-row atlas.
    std::unordered_map<std::uint32_t,CachedRowImage> row_images_;
    std::size_t row_image_bytes_{};
    ComPtr<ID2D1Factory> factory_;
    ComPtr<IDWriteFactory> write_factory_;
    ComPtr<IDWriteTextFormat> body_font_, number_font_;
    ComPtr<ID2D1DCRenderTarget> target_;
    ComPtr<ID2D1SolidColorBrush> brush_;
    HDC dc_{};
    HBITMAP bitmap_{};
    HGDIOBJ old_bitmap_{};
    void* bits_{};
    SIZE bitmap_size_{};
};

Overlay::~Overlay() {
    if (foreground_hook_) { UnhookWinEvent(foreground_hook_); foreground_hook_ = nullptr; }
    foreground_update_target = nullptr;
    worker_.stop(); icons_.reset();
    if (client_ && IsWindow(client_)) DestroyWindow(client_);
    if (hwnd_ && IsWindow(hwnd_)) DestroyWindow(hwnd_);
    release_surface();
    if (client_font_) DeleteObject(client_font_);
    if (icon_) DestroyIcon(icon_);
    if (large_icon_) DestroyIcon(large_icon_);
}
void Overlay::load_settings() {
    settings_ = local_file(L"FLUXTOP_OVERLAY_SETTINGS", L"overlay.ini").wstring();
    if (settings_.empty()) return;
    cpu_view_=load_cpu_view([this](const auto& key){return ini_overlay_read_text(settings_,key);});
    const auto read = [&](const wchar_t* key, int fallback) { return ini_overlay_read_int(settings_,key,fallback); };
    always_show_=read(L"overlay_always_show",0)!=0;
    const auto scene_x=read(L"scene_x",-1),scene_y=read(L"scene_y",-1);
    if(scene_x>=0 && scene_y>=0)
        scene_position_=OverlayScenePosition{std::min(scene_x,scene_position_units),
            std::min(scene_y,scene_position_units),ini_overlay_read_text(settings_,L"scene_display")};
    const auto entries=std::clamp(read(L"overlay_applications",0),0,4096);
    for(int i=0;i<entries;++i) {
        const auto key=L"overlay_app_"+std::to_wstring(i);
        auto path=ini_overlay_read_text(settings_,key);
        auto exe=ini_overlay_read_text(settings_,key+L"_exe");
        auto name=ini_overlay_read_text(settings_,key+L"_name");
        const bool blocked=read((key+L"_blocked").c_str(),0)!=0;
        if(exe.empty()) {exe=overlay_executable_name(path);migrate_app_settings_=true;}
        if(exe.empty())continue;
        if(name.empty())name=wide(display_names_detail::base_name(utf8(exe.c_str())));
        if(const auto existing=overlay_application_index(learned_apps_,exe)) {
            learned_apps_[*existing].blocked|=blocked;
            migrate_app_settings_=true;
        } else learned_apps_.push_back({std::move(exe),std::move(name),std::move(path),blocked});
    }
    x_ = read(L"x", CW_USEDEFAULT); y_ = read(L"y", CW_USEDEFAULT);
    right_anchor_ = read(L"anchor_right", CW_USEDEFAULT);
    if (read(L"layout_version", 0) >= 2) opacity_ = static_cast<unsigned>(std::clamp(read(L"opacity", 100), 1, 100));
    const auto saved_metrics = static_cast<unsigned>(read(L"metrics", default_metrics));
    selected_disks_mask_ = static_cast<unsigned>(read(L"disk_columns",0)) & ((1U<<26)-1U);
    disk_total_enabled_=read(L"disk_total_column",1)!=0;
    metrics_ = read(L"layout_version", 0) >= 3 ? saved_metrics & all_metrics
        : (saved_metrics & 15U) | ((saved_metrics & 48U) ? 16U : 0U);
    // If the user previously enabled only the removed FPS checkbox, restore the standard metric set.
    if (metrics_ == 0 && (saved_metrics & (1U<<5))) metrics_ = default_metrics;
    summary_ = read(L"summary_only", 0) != 0;
    header_colors_ = read(L"header_colors", 1) != 0;
    row_colors_ = read(L"row_colors", 1) != 0;
    animation_enabled_ = read(L"overlay_animation", 1) != 0;
    if (!options_.interval_explicit) {
        const auto saved = read(L"interval_ms", 1000);
        const auto bounded = std::clamp(saved, interval_first_step * interval_step_ms, interval_last_step * interval_step_ms);
        options_.interval_ms = static_cast<unsigned>(interval_position(static_cast<unsigned>(bounded)) * interval_step_ms);
    }
    show_icons_ = read(L"icons", 1) != 0;
    if (!options_.font_explicit) options_.font_size = static_cast<unsigned>(std::clamp(read(L"font_size", 16), 5, 42));
    if (!options_.top_explicit) {
        options_.top = static_cast<unsigned>(std::clamp(read(L"top", 10), 1, 100));
    }
    if (!options_.sort_explicit) options_.sort = static_cast<Sort>(std::clamp(read(L"sort", 0), 0, 7));
    if (!options_.collectors.gpu && (options_.sort == Sort::gpu || options_.sort == Sort::vram)) options_.sort = Sort::cpu;
    refresh_chart_choices();
}
void Overlay::save_settings() noexcept {
    if (settings_.empty() || !hwnd_) return;
    try {
        std::error_code error; std::filesystem::create_directories(std::filesystem::path(settings_).parent_path(), error);
        if (error) return;
        RECT rect{}; if (!GetWindowRect(hwnd_, &rect)) return;
        const auto save = [&](const wchar_t* key, int value) { ini_overlay_write(settings_,key,value); };
        save(L"layout_version", 3); save(L"x", rect.left); save(L"y", rect.top);
        save(L"anchor_right", right_anchor_ == CW_USEDEFAULT ? rect.right : right_anchor_);
        save(L"font_size", static_cast<int>(options_.font_size)); save(L"top", static_cast<int>(options_.top));
        save(L"sort", static_cast<int>(options_.sort)); save(L"opacity", static_cast<int>(opacity_)); save(L"icons", show_icons_ ? 1 : 0);
        save(L"metrics", static_cast<int>(metrics_)); save(L"summary_only", summary_ ? 1 : 0);
        save(L"header_colors", header_colors_ ? 1 : 0); save(L"row_colors", row_colors_ ? 1 : 0);
        save(L"overlay_animation", animation_enabled_ ? 1 : 0);
        save(L"disk_columns",static_cast<int>(selected_disks_mask_));
        save(L"disk_total_column",disk_total_enabled_?1:0);
        save(L"interval_ms", static_cast<int>(options_.interval_ms));
        save(L"overlay_always_show",always_show_?1:0);
        if(scene_position_) {
            save(L"scene_x",scene_position_->x);save(L"scene_y",scene_position_->y);
            ini_overlay_write_text(settings_,L"scene_display",scene_position_->display);
        }
        save_cpu_view(cpu_view_,[this](const auto& key,const auto& text){ini_overlay_write_text(settings_,key,text);});
    } catch (...) {}
}
void Overlay::load_icons() {
    const auto module = GetModuleHandleW(nullptr);
    const auto small_icon = static_cast<HICON>(LoadImageW(module, MAKEINTRESOURCEW(IDI_FLUXTOP), IMAGE_ICON,
        GetSystemMetricsForDpi(SM_CXSMICON, dpi_), GetSystemMetricsForDpi(SM_CYSMICON, dpi_), 0));
    const auto big_icon = static_cast<HICON>(LoadImageW(module, MAKEINTRESOURCEW(IDI_FLUXTOP), IMAGE_ICON,
        GetSystemMetricsForDpi(SM_CXICON, dpi_), GetSystemMetricsForDpi(SM_CYICON, dpi_), 0));
    if (!small_icon || !big_icon) { if (small_icon) DestroyIcon(small_icon); if (big_icon) DestroyIcon(big_icon); throw windows_error("LoadImageW FluxTop icon"); }
    for (const auto window : {hwnd_,client_}) if (window && IsWindow(window)) {
        SendMessageW(window, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(small_icon));
        SendMessageW(window, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(big_icon));
        SetClassLongPtrW(window, GCLP_HICONSM, reinterpret_cast<LONG_PTR>(small_icon));
        SetClassLongPtrW(window, GCLP_HICON, reinterpret_cast<LONG_PTR>(big_icon));
    }
    if (icon_) DestroyIcon(icon_); if (large_icon_) DestroyIcon(large_icon_);
    icon_ = small_icon; large_icon_ = big_icon;
    if (tray_) {
        NOTIFYICONDATAW data{}; data.cbSize = sizeof(data); data.hWnd = hwnd_; data.uID = 1; data.uFlags = NIF_ICON; data.hIcon = icon_;
        Shell_NotifyIconW(NIM_MODIFY, &data);
    }
}
std::vector<wchar_t> Overlay::enabled_disk_letters() const {
    std::vector<wchar_t> result;
    result.reserve(available_disk_letters_.size());
    for(const wchar_t letter:available_disk_letters_)
        if(selected_disks_mask_ & (1U<<(letter-L'A')))result.push_back(letter);
    return result;
}
void Overlay::cpu_settings_changed(){
    if(snapshot_)cpu_columns_=cpu_view_columns(cpu_view_,snapshot_->cpu_topology);
    if(statistics_)statistics_->set_cpu_view(cpu_view_);
    clear_row_images();update_view();save_settings();fit_table();InvalidateRect(hwnd_,nullptr,FALSE);
}
void Overlay::refresh_cpu_controls(bool force){
    if(!cpu_label_ || !snapshot_)return;
    const auto& topology=snapshot_->cpu_topology;
    std::vector<CpuViewId> detected;for(const auto& core:topology.cores)detected.push_back({core.group,core.core_id});
    if(!force && detected==cpu_topology_ids_)return;
    cpu_topology_ids_=std::move(detected);
    for(const auto id:cpu_topology_ids_)if(cpu_view_.columns.size()<CpuViewSettings::column_limit && std::none_of(cpu_view_.columns.begin(),cpu_view_.columns.end(),[&](const auto& c){return c.id==id;}))
        cpu_view_.columns.push_back({id});
    for(const auto& [window,index]:cpu_checks_){
        (void)index;controls_.erase(std::remove_if(controls_.begin(),controls_.end(),[window](const auto& child){return child.window==window;}),controls_.end());
        DestroyWindow(window);
    }cpu_checks_.clear();
    for(const auto& core:topology.cores){
        const auto found=std::find_if(cpu_view_.columns.begin(),cpu_view_.columns.end(),[&](const auto& col){return col.id==CpuViewId{core.group,core.core_id};});
        if(found==cpu_view_.columns.end())continue;
        const auto ordinal=cpu_checks_.size();
        const auto name=cpu_core_label(core,topology.cores);
        const auto box=control(L"BUTTON",name.c_str(),BS_AUTOCHECKBOX|WS_TABSTOP,cpu_checkbox_control+static_cast<UINT>(ordinal),
            22+static_cast<int>(ordinal%5)*104,520+static_cast<int>(ordinal/5)*32,100,26);
        SendMessageW(box,BM_SETCHECK,found->visible?BST_CHECKED:BST_UNCHECKED,0);
        cpu_checks_.emplace_back(box,static_cast<std::size_t>(found-cpu_view_.columns.begin()));
        if(client_page_!=0)ShowWindow(box,SW_HIDE);
    }
    if(!initializing_client_)refresh_disk_controls();
}
void Overlay::refresh_disk_controls() {
    if(!client_ || !IsWindow(client_))return;
    // Disk topology changes are infrequent; keep no permanently hidden listbox.
    if(disk_total_check_) {
        const HWND old=disk_total_check_;
        controls_.erase(std::remove_if(controls_.begin(),controls_.end(),
            [old](const Child& entry){return entry.window==old;}),controls_.end());
        DestroyWindow(old);disk_total_check_=nullptr;
    }
    for(auto& check:disk_checks_) {
        if(!check)continue;
        const HWND old=check;
        controls_.erase(std::remove_if(controls_.begin(),controls_.end(),
            [old](const Child& entry){return entry.window==old;}),controls_.end());
        DestroyWindow(old);
        check=nullptr;
    }
    disk_total_check_=control(L"BUTTON",L"Disks",BS_AUTOCHECKBOX|WS_TABSTOP,
        disk_total_checkbox_control,22,483,98,26);
    SendMessageW(disk_total_check_,BM_SETCHECK,disk_total_enabled_?BST_CHECKED:BST_UNCHECKED,0);
    if(client_page_!=0)ShowWindow(disk_total_check_,SW_HIDE);
    for(std::size_t i=0;i<available_disk_letters_.size();++i) {
        const wchar_t letter=available_disk_letters_[i];
        if(letter<L'A' || letter>L'Z')continue;
        const unsigned ordinal=static_cast<unsigned>(letter-L'A');
        const auto name=L"Disk "+std::wstring(1,letter);
        const auto slot=i+1U;
        const HWND box=control(L"BUTTON",name.c_str(),BS_AUTOCHECKBOX | WS_TABSTOP,
            disk_checkbox_control+ordinal,22+static_cast<int>(slot%5U)*104,
            483+static_cast<int>(slot/5U)*32,98,26);
        disk_checks_[ordinal]=box;
        SendMessageW(box,BM_SETCHECK,(selected_disks_mask_ & (1U<<ordinal))?
            BST_CHECKED:BST_UNCHECKED,0);
        if(client_page_!=0)ShowWindow(box,SW_HIDE);
    }
    // The same responsive checkbox grid also determines the settings height.
    const int rows=static_cast<int>((available_disk_letters_.size()+1U+4U)/5U);
    const int cpu_y=486+rows*32;
    constexpr int heading_spacing=28;
    const int temperature_y=cpu_y+heading_spacing+static_cast<int>((cpu_checks_.size()+4)/5)*32;
    const int cpu_aggregate_y=temperature_y+heading_spacing;
    const int summary_y=cpu_aggregate_y+36;
    const int header_y=summary_y+33;
    const int animation_y=header_y+36;
    const int stats_y=animation_y+43;
    const int shortcuts_y=stats_y+112;
    const int quit_y=shortcuts_y+40;
    const auto position=[this](HWND handle,int y) {
        if(!handle)return;
        for(auto& child:controls_)if(child.window==handle) {
            const int height=child.bounds.bottom-child.bounds.top;
            child.bounds.top=y;child.bounds.bottom=y+height;
            return;
        }
    };
    position(cpu_label_,cpu_y);
    for(std::size_t i=0;i<cpu_checks_.size();++i)position(cpu_checks_[i].first,cpu_y+heading_spacing+static_cast<int>(i/5)*32);
    position(cpu_temperature_label_,temperature_y);
    position(GetDlgItem(client_,cpu_package_control),cpu_aggregate_y);position(GetDlgItem(client_,cpu_max_control),cpu_aggregate_y);
    position(GetDlgItem(client_,cpu_average_control),cpu_aggregate_y);
    position(GetDlgItem(client_,summary_control),summary_y);
    position(GetDlgItem(client_,header_colors_control),header_y);
    position(GetDlgItem(client_,row_colors_control),header_y);
    position(GetDlgItem(client_,animation_control),animation_y);
    position(GetDlgItem(client_,stats_control),stats_y);
    position(shortcuts_label_,shortcuts_y);
    position(GetDlgItem(client_,logs_control),quit_y);
    position(GetDlgItem(client_,quit_app),quit_y);
    if(!initializing_client_)layout_client();
}
void Overlay::update_selected_charts() {
    selected_charts_.clear();
    for(std::size_t i=0;i<chart_choices_.size();++i)
        if(chart_choices_[i].enabled)selected_charts_.push_back(i);
    chart_page_=0;
}
bool Overlay::refresh_chart_choices(bool create_controls) {
    const std::span<const CpuCoreTemperature> cores=snapshot_?
        std::span<const CpuCoreTemperature>(snapshot_->cpu_topology.cores):std::span<const CpuCoreTemperature>{};
    const auto core_id=[](const CpuCoreTemperature& core){return CpuViewId{core.group,core.core_id};};
    const bool changed=chart_choices_.empty() || chart_disks_!=available_disk_letters_ ||
        !std::ranges::equal(cores,chart_cores_,{},core_id);
    if(changed) {
        chart_disks_=available_disk_letters_;
        chart_cores_.clear();
        chart_choices_={
            {OverlayChartKind::cpu,L"CPU",L"cpu"},
            {OverlayChartKind::cpu_temperature,L"Темп. ЦП",L"cpu_temperature"},
            {OverlayChartKind::ram,L"RAM",L"ram"},
            {OverlayChartKind::gpu,L"GPU",L"gpu"},
            {OverlayChartKind::vram,L"GPU RAM",L"vram"},
            {OverlayChartKind::network,L"Rx/Tx",L"network"},
            {OverlayChartKind::fps,L"FPS",L"fps"}
        };
        for(const auto letter:chart_disks_)
            chart_choices_.push_back({OverlayChartKind::disk,metric_text::disk_name(letter),
                L"disk_"+std::wstring(1,letter),letter});
        for(const auto& core:cores) {
            const auto id=core_id(core);
            chart_cores_.push_back(id);
            chart_choices_.push_back({OverlayChartKind::core,cpu_core_label(core,cores),
                L"core_"+std::to_wstring(id.group)+L"_"+std::to_wstring(id.number),0,id});
        }
        for(auto& chart:chart_choices_) {
            chart.key=L"overlay_chart_"+chart.key;
            chart.enabled=ini_overlay_read(settings_,chart.key.c_str(),0)!=0;
        }
        update_selected_charts();
    }
    if(chart_settings_label_ && (changed || create_controls)) {
        for(HWND box:chart_checks_) {
            std::erase_if(controls_,[box](const Child& child){return child.window==box;});
            DestroyWindow(box);
        }
        chart_checks_.clear();
        for(std::size_t i=0;i<chart_choices_.size();++i) {
            const auto& chart=chart_choices_[i];
            const auto box=control(L"BUTTON",chart.title.c_str(),BS_AUTOCHECKBOX|WS_TABSTOP,
                chart_checkbox_control+static_cast<UINT>(i),22+static_cast<int>(i%5)*104,
                30+static_cast<int>(i/5)*32,102,26);
            SendMessageW(box,BM_SETCHECK,chart.enabled?BST_CHECKED:BST_UNCHECKED,0);
            if(client_page_!=0)ShowWindow(box,SW_HIDE);
            chart_checks_.push_back(box);
        }
    }
    return changed;
}
void Overlay::fonts() {
    // Layouts and rasterized rows use the previous font; release both.
    clear_row_images();
    row_text_cache_.clear();
    body_font_.Reset(); number_font_.Reset();
    const auto font = [&](const wchar_t* family, ComPtr<IDWriteTextFormat>& output) {
        require_hr(write_factory_->CreateTextFormat(family, nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, text_size(), L"ru-ru", &output), "Overlay font");
        output->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        output->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER,0,0};
        ComPtr<IDWriteInlineObject> ellipsis;
        require_hr(write_factory_->CreateEllipsisTrimmingSign(output.Get(), &ellipsis), "Text trimming");
        output->SetTrimming(&trim, ellipsis.Get());
    };
    font(L"Segoe UI", body_font_); font(L"Consolas", number_font_);
    number_font_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
}
void Overlay::fit_table() {
    if(dragging_)return;
    RECT rect{}; if (!GetWindowRect(hwnd_, &rect)) return;
    MONITORINFO monitor{sizeof(monitor)};
    const auto current=MonitorFromRect(&rect,MONITOR_DEFAULTTONEAREST);
    const auto display=scene_position_?overlay_display_monitor(scene_position_->display,current):current;
    if (!GetMonitorInfoW(display,&monitor)) return;
    const auto& area = monitor.rcWork; const int margin = pixels(8);
    const float desired_width=selected_charts_.empty()?table_width():std::max(table_width(),std::max(152.0f,text_size()*12));
    const int width = std::max(1,std::min(pixels(desired_width), static_cast<int>(area.right-area.left-margin*2)));
    chart_layout_=overlay_chart_layout(dip(width),dip(area.bottom-area.top-margin*2),text_size(),selected_charts_.size());
    const auto last_page=chart_layout_.capacity?(selected_charts_.size()-1)/chart_layout_.capacity:0;
    chart_page_=std::min(chart_page_,last_page);
    const int requested_height = static_cast<int>(std::ceil((row_height() * static_cast<float>(summary_only() ? 1 : options_.top+1) + 4 + chart_layout_.height) * static_cast<float>(dpi_) / 96.0f));
    const int height = std::min(requested_height, static_cast<int>(area.bottom-area.top-margin*2));
    const int right = right_anchor_ == CW_USEDEFAULT ? rect.right : right_anchor_;
    const int x = scene_position_?area.left+scene_axis_offset(area.right-area.left,width,margin,scene_position_->x):
        overlay_fitted_left(right, width, area.left, area.right, margin);
    const int y = scene_position_?area.top+scene_axis_offset(area.bottom-area.top,height,margin,scene_position_->y):
        std::clamp(static_cast<int>(rect.top), static_cast<int>(area.top+margin), static_cast<int>(area.bottom-margin-height));
    if (right_anchor_ == CW_USEDEFAULT) right_anchor_ = x + width;
    SetWindowPos(hwnd_, HWND_TOPMOST, x, y, width, height,
        SWP_NOACTIVATE|(scene_dialog_?SWP_NOZORDER:0));
}
void Overlay::add_tray() {
    NOTIFYICONDATAW data{}; data.cbSize = sizeof(data); data.hWnd = hwnd_; data.uID = 1;
    data.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP; data.uCallbackMessage = tray_message; data.hIcon = icon_;
    wcscpy_s(data.szTip, L"FluxTop — открыть клиент");
    tray_ = Shell_NotifyIconW(NIM_ADD, &data) != FALSE;
    if (tray_) { data.uVersion = NOTIFYICON_VERSION; Shell_NotifyIconW(NIM_SETVERSION, &data); }
}
void Overlay::tray_menu() {
    const auto popup = CreatePopupMenu(); if (!popup) return;
    AppendMenuW(popup, MF_STRING, open_client, L"Открыть клиент");
    AppendMenuW(popup, MF_STRING, toggle_table, overlay_running_ ? L"Приостановить оверлей" : L"Возобновить оверлей");
    if(chart_layout_.capacity && selected_charts_.size()>chart_layout_.capacity)
        AppendMenuW(popup,MF_STRING,next_chart_page,L"Следующая страница графиков");
    RECT viewport{};GetClientRect(hwnd_,&viewport);
    const auto rows=visible_rows(dip(viewport.bottom));
    if(rows && rows<rows_.size())AppendMenuW(popup,MF_STRING,next_table_page,L"Следующая страница приложений");
    AppendMenuW(popup, MF_SEPARATOR, 0, nullptr); AppendMenuW(popup, MF_STRING, quit_app, L"Выход");
    POINT point{}; GetCursorPos(&point); SetForegroundWindow(hwnd_);
    const auto selected = TrackPopupMenu(popup, TPM_RETURNCMD | TPM_RIGHTBUTTON, point.x, point.y, 0, hwnd_, nullptr);
    DestroyMenu(popup); PostMessageW(hwnd_, WM_NULL, 0, 0);
    if (selected == open_client) show_client();
    else if(selected==toggle_table) {overlay_running_=!overlay_running_;update_overlay_visibility();}
    else if(selected==next_chart_page && chart_layout_.capacity) {
        const auto pages=(selected_charts_.size()+chart_layout_.capacity-1)/chart_layout_.capacity;
        if(pages)chart_page_=(chart_page_+1)%pages;
        InvalidateRect(hwnd_,nullptr,FALSE);
    } else if(selected==next_table_page && rows<rows_.size()) {
        const auto maximum=rows_.size()-rows;
        scroll_=scroll_==maximum?0:std::min(scroll_+rows,maximum);
        InvalidateRect(hwnd_,nullptr,FALSE);
    } else if (selected == quit_app) DestroyWindow(hwnd_);
}

HWND Overlay::control(const wchar_t* kind, const wchar_t* label, DWORD style, UINT id, int x, int y, int width, int height) {
    const auto window = CreateWindowExW(0, kind, label, WS_CHILD | WS_VISIBLE | style, 0, 0, 1, 1,
        client_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    if (!window) throw windows_error("Client control");
    protect_setting_from_wheel(window,kind);
    UINT group=table_options_group;
    if(id==stats_control || (id==0 && y==859))group=diagnostic_options_group;
    else if(id==chart_heading_control || id>=chart_checkbox_control)group=chart_options_group;
    else if((id>=metric_control && id<tab_control) ||
        (id>=cpu_package_control && id<blacklist_button) || y>=422)group=metrics_options_group;
    if(y==18 || id==visible_control || id==manual_move_control || id==move_control || id==blacklist_button ||
        id==logs_control || id==quit_app || (id>=table_options_group && id<=diagnostic_options_group))group=0;
    controls_.push_back({window, {x,y,x+width,y+height},group}); return window;
}
void Overlay::show_client() {
    diagnostics::write(diagnostics::Level::info,"ui.open","settings client requested");
    if (client_) { ShowWindow(client_, SW_RESTORE); SetForegroundWindow(client_); return; }
    const std::wstring client_title = L"FluxTop v" + wide(FLUXTOP_VERSION);
    client_ = CreateWindowExW(0, client_class, client_title.c_str(), WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 640, 560, nullptr, nullptr, GetModuleHandleW(nullptr), this);
    if (!client_) throw windows_error("Create client");
    client_dpi_ = GetDpiForWindow(client_); layout_client();
    ShowWindow(client_, SW_SHOW); SetForegroundWindow(client_); activity();
    // Optional check on client opening only. The monitor UI never blocks on HTTP.
    const auto appdata=env(L"LOCALAPPDATA");
    if (!appdata.empty()) {
        const auto launcher=std::filesystem::path(appdata)/L"FluxTop"/L"FluxTop.exe";
        std::error_code error;
        if (std::filesystem::is_regular_file(launcher,error)) {
            // Older installed launchers do not understand --check-update.
            const auto module=LoadLibraryExW(launcher.c_str(),nullptr,
                LOAD_LIBRARY_AS_DATAFILE|LOAD_LIBRARY_AS_IMAGE_RESOURCE);
            if (module) {
                wchar_t marker[64]{};
                const auto supported=LoadStringW(module,204,marker,static_cast<int>(std::size(marker)))>0
                    && std::wstring_view(marker)==L"FluxTop.update-consent.v1";
                FreeLibrary(module);
                if (supported) {
                    SHELLEXECUTEINFOW check{};
                    check.cbSize=sizeof(check);
                    check.lpFile=launcher.c_str();
                    check.lpParameters=L"--check-update";
                    check.nShow=SW_SHOWNORMAL;
                    (void)ShellExecuteExW(&check);
                }
            }
        }
    }
}
void Overlay::init_client() {
    initializing_client_ = true;
    tabs_=CreateWindowExW(0,WC_TABCONTROLW,L"",WS_CHILD|WS_VISIBLE|WS_CLIPSIBLINGS|WS_TABSTOP,
        0,0,580,840,client_,reinterpret_cast<HMENU>(static_cast<INT_PTR>(tab_control)),GetModuleHandleW(nullptr),nullptr);
    if(!tabs_) throw windows_error("Create client tabs");
    TCITEMW item{};item.mask=TCIF_TEXT;
    item.pszText=const_cast<LPWSTR>(L"Настройки");TabCtrl_InsertItem(tabs_,0,&item);
    item.pszText=const_cast<LPWSTR>(L"Статистика");TabCtrl_InsertItem(tabs_,1,&item);
    control(L"STATIC", L"Оверлей поверх приложений", 0, 0, 22, 18, 540, 25);
    control(L"STATIC", (L"Версия " + wide(FLUXTOP_VERSION)).c_str(), SS_RIGHT, 0, 418, 18, 140, 25);
    control(L"STATIC", (L"Количество приложений: " + std::to_wstring(options_.top) + L" / 100").c_str(),
        0, top_label, 22, 62, 268, 24);
    const auto top = control(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP,
        top_control, 290, 58, 268, 32);
    SendMessageW(top, TBM_SETRANGE, TRUE, MAKELPARAM(1,100));
    SendMessageW(top, TBM_SETPOS, TRUE, static_cast<LPARAM>(options_.top));
    control(L"STATIC", L"Сортировка", 0, 0, 22, 106, 230, 24);
    const auto sort = control(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, sort_control, 290, 102, 268, 160);
    for (const auto label : {L"Авто", L"CPU", L"RAM", L"GPU", L"Память GPU", L"Rx — приём", L"Tx — передача", L"Rx/Tx — суммарный трафик"}) SendMessageW(sort, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label));
    SendMessageW(sort, CB_SETCURSEL, static_cast<WPARAM>(sort_ui_index(options_.sort)), 0);
    control(L"STATIC", (L"Размер текста: " + std::to_wstring(options_.font_size) + L" px (5–42)").c_str(),
        0, font_label, 22, 150, 260, 24);
    const auto font = control(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP,
        font_control, 290, 146, 268, 32);
    SendMessageW(font, TBM_SETRANGE, TRUE, MAKELPARAM(5,42));
    SendMessageW(font, TBM_SETPOS, TRUE, static_cast<LPARAM>(options_.font_size));
    control(L"STATIC", (L"Непрозрачность: " + std::to_wstring(opacity_) + L"%").c_str(), 0, alpha_label, 22, 194, 260, 24);
    const auto alpha = control(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, alpha_control, 290, 190, 268, 32);
    SendMessageW(alpha, TBM_SETRANGE, TRUE, MAKELPARAM(1,100));
    SendMessageW(alpha, TBM_SETPOS, TRUE, opacity_);
    const auto icons = control(L"BUTTON", L"Показывать иконки приложений", BS_AUTOCHECKBOX | WS_TABSTOP, icons_control, 22, 238, 380, 26);
    SendMessageW(icons, BM_SETCHECK, show_icons_ ? BST_CHECKED : BST_UNCHECKED, 0);
    const auto visible = control(L"BUTTON", L"Показывать оверлей всегда", BS_AUTOCHECKBOX | WS_TABSTOP, visible_control, 22, 51, 380, 26);
    SendMessageW(visible, BM_SETCHECK, always_show_ ? BST_CHECKED : BST_UNCHECKED, 0);
    const auto manual=control(L"BUTTON",L"Перемещать оверлей вручную",BS_AUTOCHECKBOX|WS_TABSTOP,manual_move_control,22,81,380,26);
    SendMessageW(manual,BM_SETCHECK,manual_movement_?BST_CHECKED:BST_UNCHECKED,0);
    control(L"BUTTON",L"Настроить сцену",WS_TABSTOP,move_control,22,115,260,28);
    control(L"BUTTON",L"Чёрный список приложений",WS_TABSTOP,blacklist_button,22,151,260,28);
    for(const auto& [id,title]:{std::pair<UINT,const wchar_t*>{table_options_group,L"Внешний вид и обновление"},
        {metrics_options_group,L"Показатели и датчики"},{chart_options_group,L"Графики в оверлее"},
        {diagnostic_options_group,L"Диагностика и горячие клавиши"}})
        control(L"BUTTON",title,BS_LEFT|WS_TABSTOP,id,22,0,536,32);
    const auto startup = control(L"BUTTON", L"Автозапуск при входе в Windows (с запросом UAC)", BS_AUTOCHECKBOX | WS_TABSTOP, startup_control, 22, 337, 536, 26);
    SendMessageW(startup, BM_SETCHECK, autostart_enabled() ? BST_CHECKED : BST_UNCHECKED, 0);
    control(L"STATIC", interval_caption(options_.interval_ms).c_str(),
        0, interval_label, 22, 382, 260, 24);
    const auto interval = control(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP,
        interval_control, 290, 378, 268, 32);
    SendMessageW(interval, TBM_SETRANGE, TRUE, MAKELPARAM(interval_first_step,interval_last_step));
    SendMessageW(interval, TBM_SETPAGESIZE, 0, 1);
    SendMessageW(interval, TBM_SETPOS, TRUE, interval_position(options_.interval_ms));
    control(L"STATIC", L"Показывать показатели:", 0, 0, 22, 422, 536, 24);
    for (unsigned i=0;i<metric_names.size();++i) {
        const auto box = control(L"BUTTON", metric_names[i], BS_AUTOCHECKBOX | WS_TABSTOP,
            metric_control+i, 22+static_cast<int>(i)*104, 451, 98, 26);
        SendMessageW(box, BM_SETCHECK, (metrics_ & (1U<<i)) ? BST_CHECKED : BST_UNCHECKED, 0);
    }
    const auto summary = control(L"BUTTON", L"Только общие показатели (без списка приложений)", BS_AUTOCHECKBOX | WS_TABSTOP,
        summary_control, 22, 635, 536, 26);
    SendMessageW(summary, BM_SETCHECK, summary_ ? BST_CHECKED : BST_UNCHECKED, 0);
    const auto header_colors = control(L"BUTTON", L"Окрашивать общие показатели", BS_AUTOCHECKBOX | WS_TABSTOP,
        header_colors_control, 22, 668, 268, 26);
    SendMessageW(header_colors, BM_SETCHECK, header_colors_ ? BST_CHECKED : BST_UNCHECKED, 0);
    const auto row_colors = control(L"BUTTON", L"Окрашивать строки оверлея", BS_AUTOCHECKBOX | WS_TABSTOP,
        row_colors_control, 290, 668, 280, 26);
    SendMessageW(row_colors, BM_SETCHECK, row_colors_ ? BST_CHECKED : BST_UNCHECKED, 0);

    const auto animation = control(L"BUTTON", L"Анимация строк оверлея", BS_AUTOCHECKBOX | WS_TABSTOP,
        animation_control, 22, 704, 536, 26);
    SendMessageW(animation, BM_SETCHECK, animation_enabled_ ? BST_CHECKED : BST_UNCHECKED, 0);
    control(L"STATIC", L"", 0, stats_control, 22, 747, 536, 104);
    shortcuts_label_=control(L"STATIC", L"Ctrl+Alt+F9 — клиент   ·   Ctrl+Alt+F10 — оверлей", 0, 0, 22, 859, 540, 24);
    control(L"BUTTON", L"Логи", WS_TABSTOP, logs_control, 22, 899, 130, 32);
    control(L"BUTTON", L"Завершить работу", WS_TABSTOP, quit_app, 373, 899, 185, 32);
    ensure_icon_cache();
    statistics_=std::make_unique<StatisticsPage>(client_,history_path(),settings_,options_.top,graph_history_,icons_);
    statistics_->set_cpu_view(cpu_view_);
    statistics_->activate(false);
    if(snapshot_)statistics_->on_snapshot(snapshot_->timestamp_ms,grouped_processes_,options_.interval_ms,*snapshot_);
    cpu_label_=control(L"STATIC",L"Ядра CPU:",0,0,22,520,540,24);
    cpu_temperature_label_=control(L"STATIC",L"Температуры ЦП:",0,0,22,692,540,24);
    for(const auto [id,label,enabled]:{std::tuple{cpu_package_control,L"CPU Package",cpu_view_.package},std::tuple{cpu_max_control,L"Core Max",cpu_view_.maximum},std::tuple{cpu_average_control,L"Core Average",cpu_view_.average}}){
        const auto box=control(L"BUTTON",label,BS_AUTOCHECKBOX|WS_TABSTOP,id,22+static_cast<int>(id-cpu_package_control)*190,720,180,26);
        SendMessageW(box,BM_SETCHECK,enabled?BST_CHECKED:BST_UNCHECKED,0);
    }
    chart_settings_label_=control(L"STATIC",L"Графики за последнюю минуту",0,chart_heading_control,22,0,540,24);
    refresh_chart_choices(true);
    refresh_cpu_controls();
    refresh_disk_controls();
    initializing_client_ = false; client_stats();
}
void Overlay::switch_page(unsigned page) {
    diagnostics::write(diagnostics::Level::info,"ui.page","tab="+std::to_string(page));
    if(page!=0 && client_page_==0) {
        // A quick tab switch can precede the delayed Settings trackbar commit.
        const auto pending_top=SendDlgItemMessageW(client_,top_control,TBM_GETPOS,0,0);
        if(pending_top>=1 && pending_top<=100 &&
            static_cast<unsigned>(pending_top)!=options_.top) {
            KillTimer(client_,settings_timer);
            apply_client();
        }
        if(statistics_) statistics_->set_limit(options_.top);
    }
    client_page_=page==1?1U:0U;
    // Statistics gets a readable workspace; Settings retains its compact dimensions.
    layout_client();
    if(statistics_) statistics_->activate(client_page_==1);
}
void Overlay::layout_client() {
    if (!client_) return;
    const auto px = [this](int value) { return MulDiv(value, static_cast<int>(client_dpi_), 96); };
    const int page_width=client_page_==1?1420:680;
    std::vector<std::pair<HWND,RECT>> placements;
    placements.reserve(controls_.size());
    for(const auto& child:controls_) {
        const auto id=GetDlgCtrlID(child.window);
        if(!child.group && (id<table_options_group || id>diagnostic_options_group) &&
            id!=logs_control && id!=quit_app)placements.emplace_back(child.window,child.bounds);
    }
    int next=195;
    constexpr std::array group_titles{L"Внешний вид и обновление",L"Показатели и датчики",
        L"Графики в оверлее",L"Диагностика и горячие клавиши"};
    for(UINT group=table_options_group;group<=diagnostic_options_group;++group) {
        const auto heading=GetDlgItem(client_,static_cast<int>(group));
        const bool expanded=open_group_==group;
        const auto title=std::wstring(expanded?L"−   ":L"+   ")+group_titles[group-table_options_group];
        SetWindowTextW(heading,title.c_str());
        placements.emplace_back(heading,RECT{22,next,558,next+32});
        next+=38;
        if(!expanded)continue;
        LONG first=LONG_MAX;
        for(const auto& child:controls_)if(child.group==group)first=std::min(first,child.bounds.top);
        int bottom=next;
        for(const auto& child:controls_)if(child.group==group) {
            RECT bounds=child.bounds;OffsetRect(&bounds,0,next-first);
            placements.emplace_back(child.window,bounds);
            const int end=GetDlgCtrlID(child.window)==sort_control?static_cast<int>(bounds.top)+28:
                static_cast<int>(bounds.bottom);
            bottom=std::max(bottom,end);
        }
        next=bottom+10;
    }
    placements.emplace_back(GetDlgItem(client_,logs_control),RECT{22,next+8,152,next+40});
    placements.emplace_back(GetDlgItem(client_,quit_app),RECT{373,next+8,558,next+40});
    const int settings_height=next+98;
    MONITORINFO monitor{sizeof(monitor)};GetMonitorInfoW(MonitorFromWindow(client_,MONITOR_DEFAULTTONEAREST),&monitor);
    const int available=std::max(400,MulDiv(monitor.rcWork.bottom-monitor.rcWork.top,96,static_cast<int>(client_dpi_))-80);
    const int page_height=client_page_==1?std::min(1000,available):std::min(settings_height,available);
    ShowScrollBar(client_,SB_BOTH,FALSE);
    const double fit=client_page_==0?std::min(1.0,static_cast<double>(page_height-40)/static_cast<double>(settings_height-40)):1.0;
    const auto fitted=[fit](int value){return static_cast<int>(std::lround(value*fit));};
    const auto font = CreateFontW(-px(std::max(9,fitted(14))), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    for(const auto& child:controls_)
        if(client_page_!=0 || (child.group && child.group!=open_group_))ShowWindow(child.window,SW_HIDE);
    if(client_page_==0)for(const auto& [window,bounds]:placements) {
        SendMessageW(window,WM_SETFONT,reinterpret_cast<WPARAM>(font),TRUE);
        MoveWindow(window,px(bounds.left),px(40+fitted(bounds.top)),px(bounds.right-bounds.left),
            px(fitted(bounds.bottom-bounds.top)),TRUE);
        ShowWindow(window,SW_SHOW);
    }
    if(tabs_) MoveWindow(tabs_,0,0,px(page_width),px(page_height),TRUE);
    if(statistics_) statistics_->resize(px(12),px(40),px(page_width-24),px(page_height-52));
    if (client_font_) DeleteObject(client_font_); client_font_ = font;
    RECT rect{0,0,px(page_width),px(page_height)};
    AdjustWindowRectExForDpi(&rect, static_cast<DWORD>(GetWindowLongPtrW(client_, GWL_STYLE)), FALSE, 0, client_dpi_);
    RECT current{};GetWindowRect(client_,&current);
    const int height=rect.bottom-rect.top;
    const int top=std::clamp(static_cast<int>(current.top),static_cast<int>(monitor.rcWork.top),
        std::max(static_cast<int>(monitor.rcWork.top),static_cast<int>(monitor.rcWork.bottom)-height));
    SetWindowPos(client_, nullptr, current.left, top, rect.right-rect.left, height, SWP_NOZORDER | SWP_NOACTIVATE);
}
void Overlay::apply_client() {
    const auto old_font_size = options_.font_size;
    const auto selected_font = SendDlgItemMessageW(client_,font_control,TBM_GETPOS,0,0);
    const auto top = SendDlgItemMessageW(client_,top_control,TBM_GETPOS,0,0);
    const auto selected_sort = SendDlgItemMessageW(client_, sort_control, CB_GETCURSEL, 0, 0);
    const auto interval_step = SendDlgItemMessageW(client_,interval_control,TBM_GETPOS,0,0);
    const auto alpha = SendDlgItemMessageW(client_, alpha_control, TBM_GETPOS, 0, 0);
    if (alpha < 1 || alpha > 100 || selected_sort < 0 || selected_sort >= static_cast<LRESULT>(sort_ui_values.size())) return;
    if (top >= 1 && top <= 100) options_.top = static_cast<unsigned>(top);
    if (selected_font >= 5 && selected_font <= 42) options_.font_size = static_cast<unsigned>(selected_font);
    const auto new_sort = sort_ui_values[static_cast<std::size_t>(selected_sort)];
    if (options_.collectors.gpu || (new_sort != Sort::gpu && new_sort != Sort::vram)) options_.sort = new_sort;
    else SendDlgItemMessageW(client_, sort_control, CB_SETCURSEL, static_cast<WPARAM>(sort_ui_index(options_.sort)), 0);
    if (interval_step >= interval_first_step && interval_step <= interval_last_step &&
        interval_step != interval_position(options_.interval_ms)) {
        options_.interval_ms = static_cast<unsigned>(interval_step * interval_step_ms);
        worker_.set_interval(options_.interval_ms);
        const auto budget = overlay_transition_budget_ms(options_.interval_ms);
        if (row_animation_active_ && row_animation_duration_ms_ > budget) {
            const auto now = GetTickCount64();
            const auto elapsed = std::min<ULONGLONG>(now - row_animation_started_, row_animation_duration_ms_);
            row_animation_started_ = now - elapsed * budget / row_animation_duration_ms_;
            row_animation_duration_ms_ = budget;
            row_animation_next_frame_tick_ = 0;
        }
    }
    opacity_ = static_cast<unsigned>(alpha); show_icons_ = SendDlgItemMessageW(client_, icons_control, BM_GETCHECK, 0, 0) == BST_CHECKED;
    SetDlgItemTextW(client_, alpha_label, (L"Непрозрачность: " + std::to_wstring(opacity_) + L"%").c_str());
    SetDlgItemTextW(client_, top_label, (L"Количество приложений: " + std::to_wstring(options_.top) + L" / 100").c_str());
    SetDlgItemTextW(client_, font_label, (L"Размер текста: " + std::to_wstring(options_.font_size) + L" px (5–42)").c_str());
    SetDlgItemTextW(client_, interval_label, interval_caption(options_.interval_ms).c_str());
    if(statistics_) statistics_->set_limit(options_.top);
    metrics_ = 0;
    for (unsigned i=0;i<metric_names.size();++i) {
        if (SendDlgItemMessageW(client_,metric_control+i,BM_GETCHECK,0,0)==BST_CHECKED)
            metrics_ |= 1U<<i;
    }
    if(disk_total_check_)
        disk_total_enabled_=SendMessageW(disk_total_check_,BM_GETCHECK,0,0)==BST_CHECKED;
    for(std::size_t i=0;i<disk_checks_.size();++i)if(disk_checks_[i]) {
        const unsigned bit=1U<<static_cast<unsigned>(i);
        if(SendMessageW(disk_checks_[i],BM_GETCHECK,0,0)==BST_CHECKED)
            selected_disks_mask_|=bit;
        else selected_disks_mask_&=~bit;
    }
    summary_ = SendDlgItemMessageW(client_,summary_control,BM_GETCHECK,0,0)==BST_CHECKED;
    header_colors_ = SendDlgItemMessageW(client_,header_colors_control,BM_GETCHECK,0,0)==BST_CHECKED;
    row_colors_ = SendDlgItemMessageW(client_,row_colors_control,BM_GETCHECK,0,0)==BST_CHECKED;
    animation_enabled_ = SendDlgItemMessageW(client_,animation_control,BM_GETCHECK,0,0)==BST_CHECKED;
    if (!animation_enabled_) {
        KillTimer(hwnd_,overlay_animation_timer);
        row_animation_active_=false;
        previous_rows_.clear();row_motion_.clear();
    }
    clear_row_images(); // New metrics, colors, sizes or sorting may invalidate row pixels.
    scroll_ = 0; if (old_font_size != options_.font_size) fonts();
    diagnostics::write(diagnostics::Level::info,"ui.settings",
        "interval_ms="+std::to_string(options_.interval_ms)+" top="+std::to_string(options_.top)+
        " sort="+std::to_string(static_cast<int>(options_.sort))+
        " show_icons="+std::to_string(show_icons_)+" animate="+std::to_string(animation_enabled_));
    fit_table(); update_view(); save_settings(); client_stats();
    InvalidateRect(hwnd_, nullptr, FALSE);
}
void Overlay::client_stats() {
    if (!client_) return;
    std::wstring info;
    if (!error_.empty()) info = L"Ошибка сбора: " + wide(error_);
    else if (snapshot_) {
        const auto& s = *snapshot_;
        info = L"Процессов: " + std::to_wstring(s.processes.size()) + L"   ·   Недоступно: " + std::to_wstring(s.inaccessible)
            + L"\r\nМонитор: CPU " + percent(s.self_cpu_percent) + L"   ·   RAM " + memory(s.self_working_set)
            + L"\r\nДоступ: " + (s.elevated.value_or(false) ? std::wstring(L"администратор") : std::wstring(L"обычный"))
            + L"   ·   GPU: " + wide(s.gpu_status) + L"   ·   Время сбора: " + decimal(s.collection_ms) + L" мс";
        info += L"\r\nИконок в памяти: " + std::to_wstring(icons_ ? icons_->stats().entries : 0) + L" / " + std::to_wstring(icons_ ? icons_->stats().capacity : std::clamp<std::size_t>(static_cast<std::size_t>(options_.top)+16,32,128));
        info += L"   ·   Сеть: " + wide(s.network_status);
        info += L"   ·   Disks: " + (s.disk_io_status=="administrator-required" ?
            std::wstring(L"нужен администратор") : wide(s.disk_io_status));
        info += L"\r\nЗаписано агрегатов за сеанс: " + (s.history_records ? std::to_wstring(*s.history_records) : L"—");
        if (!s.history_error.empty()) info += L"\r\nОшибка записи истории: " + wide(s.history_error);
    } else info = L"Ожидание первого снимка…";
    SetDlgItemTextW(client_, stats_control, info.c_str());
}

void Overlay::activity() { /* History collection continues even when both windows are hidden. */ }
void Overlay::set_visible(bool visible) {
    if (visible == visible_) return;
    diagnostics::write(diagnostics::Level::info,"ui.overlay",visible?"show":"hide");
    visible_ = visible;
    if (!visible) {
        clear_row_images();
        KillTimer(hwnd_,overlay_animation_timer);
        row_animation_active_=false;
        previous_rows_.clear();row_motion_.clear();
        ShowWindow(hwnd_, SW_HIDE); release_surface(); release_unused_icon_cache();
        row_text_cache_.clear(); // Hidden overlays should not keep text layout buffers.
    }
    else {
        ShowWindow(hwnd_,SW_SHOWNOACTIVATE);
        // Startup information can override the first ShowWindow request.
        if(!IsWindowVisible(hwnd_))ShowWindow(hwnd_,SW_SHOWNOACTIVATE);
        InvalidateRect(hwnd_,nullptr,FALSE);
    }
    activity();
}
void Overlay::update_overlay_visibility() {
    const bool blocked=foreground_app_ && learned_apps_[*foreground_app_].blocked;
    set_visible(overlay_should_show(overlay_running_,always_show_ || inspect_overlay_ || manual_movement_,
        foreground_app_.has_value(),blocked));
}
void Overlay::set_manual_movement(bool enabled) {
    if(manual_movement_==enabled)return;
    manual_movement_=enabled;
    const auto style=GetWindowLongPtrW(hwnd_,GWL_EXSTYLE);
    SetWindowLongPtrW(hwnd_,GWL_EXSTYLE,enabled?style&~static_cast<LONG_PTR>(WS_EX_TRANSPARENT):
        style|WS_EX_TRANSPARENT);
    update_overlay_visibility();
    InvalidateRect(hwnd_,nullptr,FALSE);
}
void Overlay::ensure_icon_cache() {
    if(!icons_)icons_=std::make_shared<AppIconCache>(history_path(),[this] {
        if(!icon_posted_.exchange(true))PostMessageW(hwnd_,icon_message,0,0);
    });
}
void Overlay::release_unused_icon_cache() {
    if(!statistics_ && !blacklist_dialog_ && !(visible_ && show_icons_ && !summary_only()))icons_.reset();
}
void Overlay::save_application(std::size_t index) {
    if(settings_.empty())return;
    std::error_code error;
    std::filesystem::create_directories(std::filesystem::path(settings_).parent_path(),error);
    if(error)return;
    const auto write=[this](std::size_t i) {
        const auto key=L"overlay_app_"+std::to_wstring(i);
        const auto& app=learned_apps_[i];
        ini_overlay_write_text(settings_,key,app.icon_path);
        ini_overlay_write_text(settings_,key+L"_exe",app.executable);
        ini_overlay_write_text(settings_,key+L"_name",app.name);
        ini_overlay_write(settings_,(key+L"_blocked").c_str(),app.blocked);
    };
    if(migrate_app_settings_) {
        for(std::size_t i=0;i<learned_apps_.size();++i)write(i);
        migrate_app_settings_=false;
    } else write(index);
    ini_overlay_write(settings_,L"overlay_applications",static_cast<int>(learned_apps_.size()));
}
void Overlay::learn_foreground_application() {
    if(foreground_app_ && snapshot_->fps_focus_epoch==foreground_epoch_)return;
    const auto& processes=snapshot_->processes;
    const auto row=std::find_if(processes.begin(),processes.end(),
        [](const Process& process){return process.presented_fps && std::isfinite(*process.presented_fps) &&
            *process.presented_fps>0;});
    if(row==processes.end())return;
    const bool current=snapshot_->fps_focus_epoch==foreground_epoch_ && row->pid==foreground_pid_;
    auto path=row->exe_path?wide(*row->exe_path):std::wstring{};
    const auto image=path.empty()?wide(row->name):path;
    const auto executable=overlay_executable_name(image);
    if(executable.empty())return;
    auto app=overlay_application_index(learned_apps_,executable);
    if(!app && learned_apps_.size()<4096) {
        app=learned_apps_.size();
        learned_apps_.push_back({std::wstring(executable),wide(display_names_detail::base_name(row->name)),
            std::move(path),false});
        save_application(*app);
    } else if(app && learned_apps_[*app].icon_path.empty() && !path.empty()) {
        learned_apps_[*app].icon_path=std::move(path);
        save_application(*app);
        if(blacklist_dialog_)PostMessageW(blacklist_dialog_,blacklist_icons_ready,0,0);
    }
    if(current) {foreground_app_=app;foreground_app_creation_=row->creation_time;}
}
void Overlay::receive() {
    bool finished{}, received_snapshot{};
    {
        std::lock_guard lock(mailbox_mutex_);
        if (pending_) { snapshot_ = std::move(pending_); received_snapshot = true; }
        error_ = std::move(pending_error_); finished = pending_finished_;
        pending_.reset(); pending_finished_ = false; posted_ = false;
    }
    if (received_snapshot && snapshot_) {
        learn_foreground_application();
        graph_history_->append(*snapshot_,GetTickCount64(),std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
        const auto updated_cpu_columns=cpu_view_columns(cpu_view_,snapshot_->cpu_topology);
        if(updated_cpu_columns!=cpu_columns_){cpu_columns_=updated_cpu_columns;clear_row_images();if(hwnd_)fit_table();}
        refresh_cpu_controls();
        std::vector<wchar_t> detected;
        detected.reserve(snapshot_->disks.size());
        for(const auto& disk:snapshot_->disks)detected.push_back(disk.letter);
        if(detected!=available_disk_letters_) {
            available_disk_letters_=std::move(detected);
            refresh_disk_controls();
            if(hwnd_)fit_table();
        }
        if(refresh_chart_choices()) {
            refresh_disk_controls();
            if(hwnd_)fit_table();
        }
        diagnostics::write(diagnostics::Level::trace,"ui.snapshot",
            "processes="+std::to_string(snapshot_->processes.size())+
            " history_error="+snapshot_->history_error);
        // Keep process-level history untouched. Aggregate only the displayed family rows.
        grouped_processes_=aggregate_application_processes(snapshot_->processes);
        // GpuCollector has already checked each PID against adapter usage.
        // A SUM across processes is not a unique physical VRAM allocation;
        // shared allocations can be counted by more than one process.
        // Never discard verified member values based on the group sum.
        grouped_spikes_.update(grouped_processes_,snapshot_->total_ram_capacity,
            snapshot_->total_gpu_memory_capacity);
        if(statistics_) statistics_->on_snapshot(snapshot_->timestamp_ms,grouped_processes_,options_.interval_ms,*snapshot_);
    }
    update_view(); client_stats();
    if(received_snapshot && snapshot_) {
        // Audit ETW -> per-PID snapshot -> grouped app -> selected top rows.
        // These are rates from one snapshot, not additional disk measurements.
        double raw_bps=0.0,grouped_bps=0.0,top_bps=0.0;
        std::size_t raw_rows=0,grouped_rows=0,top_rows=0;
        const auto record=[](std::optional<double> read,std::optional<double> write,
            double& sum,std::size_t& positive_rows) {
            if(!read || !write || !std::isfinite(*read) || !std::isfinite(*write))return;
            const double rate=*read+*write;
            if(rate>0.0 && std::isfinite(rate)) {sum+=rate;++positive_rows;}
        };
        for(const auto& process:snapshot_->processes)
            record(process.disk_read_bytes_per_second,process.disk_write_bytes_per_second,
                raw_bps,raw_rows);
        for(const auto& process:grouped_processes_)
            record(process.disk_read_bytes_per_second,process.disk_write_bytes_per_second,
                grouped_bps,grouped_rows);
        for(const auto& row:rows_)
            record(row.disk_read,row.disk_write,top_bps,top_rows);
        ++disk_delivery_ui_samples_;
        if(raw_bps>0.0 || disk_delivery_ui_samples_%15==0) {
            diagnostics::write(diagnostics::Level::info,"ui.disk_delivery",
                "raw_bps="+std::to_string(raw_bps)+
                " grouped_bps="+std::to_string(grouped_bps)+
                " top_bps="+std::to_string(top_bps)+
                " raw_rows="+std::to_string(raw_rows)+
                " grouped_rows="+std::to_string(grouped_rows)+
                " top_rows="+std::to_string(top_rows)+
                " top_limit="+std::to_string(options_.top)+
                " total_column="+(disk_total_enabled_?"true":"false")+
                " volume_columns="+std::to_string(enabled_disk_letters().size())+
                " disk_status="+snapshot_->disk_io_status);
        }
    }
    if (visible_) InvalidateRect(hwnd_, nullptr, FALSE);
    if (finished) {
        if (visible_) {
            paint();
            const auto capture = env(L"FLUXTOP_OVERLAY_CAPTURE");
            if (!capture.empty() && !save_argb_png(capture,bits_,static_cast<unsigned>(bitmap_size_.cx),static_cast<unsigned>(bitmap_size_.cy))) exit_code_=1;
        }
        DestroyWindow(hwnd_);
    }
}
void Overlay::update_foreground() {
    const HWND active_window = GetForegroundWindow();
    const auto previous_pid=foreground_pid_;
    foreground_pid_ = 0;
    if (active_window && !IsIconic(active_window))
        GetWindowThreadProcessId(active_window, &foreground_pid_);
    if(active_window!=foreground_window_ || foreground_pid_!=previous_pid) {
        foreground_window_=active_window;
        ++foreground_epoch_;
        foreground_app_.reset();foreground_app_creation_.reset();
    }
    if(snapshot_ && foreground_pid_ && foreground_pid_!=GetCurrentProcessId()) {
        const auto& rows=snapshot_->processes;
        const auto row=std::find_if(rows.begin(),rows.end(),
            [this](const Process& item){return item.pid==foreground_pid_;});
        if(row!=rows.end() && (!foreground_app_ || foreground_app_creation_!=row->creation_time)) {
            const auto image=wide(row->exe_path?*row->exe_path:row->name);
            foreground_app_=overlay_application_index(learned_apps_,overlay_executable_name(image));
            foreground_app_creation_=row->creation_time;
            if(foreground_app_ && row->exe_path && learned_apps_[*foreground_app_].icon_path.empty()) {
                learned_apps_[*foreground_app_].icon_path=image;
                save_application(*foreground_app_);
                if(blacklist_dialog_)PostMessageW(blacklist_dialog_,blacklist_icons_ready,0,0);
            }
        }
    }
    worker_.set_fps_focus(foreground_pid_!=GetCurrentProcessId() ? FpsFocus{
        reinterpret_cast<std::uintptr_t>(active_window), foreground_pid_, foreground_epoch_} : FpsFocus{});
    update_overlay_visibility();
}
void Overlay::update_view() {
    update_foreground();
    if (!snapshot_) return;
    const auto& s = *snapshot_;
    // Foreground changes only reorder borrowed pointers, not full Process trees.
    std::vector<const Process*> display;
    display.reserve(grouped_processes_.size());
    for (const auto& process : grouped_processes_) display.push_back(&process);
    sort_process_refs(display, options_.sort, ranking_metrics());
    foreground_fps_.reset();
    if (visible_ && s.fps_focus_epoch == foreground_epoch_) {
        for (const auto& process : s.processes) {
            if (process.pid != foreground_pid_) continue;
            if (process.presented_fps && std::isfinite(*process.presented_fps))
                foreground_fps_ = process.presented_fps;
            break;
        }
    }
    focused_pid_ = 0;
    if (options_.sort == Sort::automatic && foreground_pid_ &&
        prioritize_focused_process(display,foreground_pid_))
        focused_pid_ = display.front()->pid; // May be a child PID inside a grouped family.
    std::vector<Row> next;
    for (std::size_t i = 0; !summary_only() && i < std::min(display.size(), static_cast<std::size_t>(options_.top)); ++i) {
        const auto& p = *display[i];
        auto name = wide(application_display_name(p,grouped_processes_));
        std::array<std::optional<double>,4> loads{};
        for (std::size_t j=0; j<loads.size(); ++j)
            loads[j] = auto_metric_load_percent(p,static_cast<AutoMetric>(j+1),
                s.total_ram_capacity,s.total_gpu_memory_capacity);
        // No blink/flash effects in the overlay. AutoScore still uses burst data for ranking.
        // All display values use the same verified-subset fields as the
        // existing load colors, AutoScore, live statistics and history.
        next.push_back({p.pid,p.creation_time,std::move(name),percent(p.cpu_percent),
            memory(p.working_set),percent(p.gpu_percent),memory(p.dedicated_gpu_bytes),
            network_rate(p.rx_bytes_per_second,p.tx_bytes_per_second),
            p.disk_read_bytes_per_second,p.disk_write_bytes_per_second,
            p.disk_volume_bytes_per_second,loads,{},p.exe_path?wide(*p.exe_path):std::wstring{}});
        for(const auto& column:cpu_columns_)next.back().logical_cpu.push_back(cpu_application_core_load(p.cpu_execution,column.id,s.cpu_topology));
    }
    if (visible_ && show_icons_ && !summary_only()) {
        ensure_icon_cache();
        icons_->set_capacity(std::clamp<std::size_t>(static_cast<std::size_t>(options_.top)+16,32,128));
        for (const auto& row : next) icons_->request(row.icon_path);
    } else release_unused_icon_cache();
    if (next != rows_) {
        // Numeric-only updates never restart the animation timer.
        bool order_changed=next.size()!=rows_.size();
        if(!order_changed) for(std::size_t i=0;i<next.size();++i) {
            if(next[i].pid!=rows_[i].pid || next[i].created!=rows_[i].created) {
                order_changed=true;break;
            }
        }
        if(!order_changed) {
            rows_=std::move(next);
        } else if (visible_ && animation_enabled_ && !summary_only() && !next.empty()) {
            const ULONGLONG tick=GetTickCount64();
            // Retarget from the current *visual* positions if the foreground changes mid-flight.
            const float previous_progress=row_animation_active_ && row_animation_duration_ms_
                ? overlay_smoothstep(static_cast<float>(tick-row_animation_started_)
                    /static_cast<float>(row_animation_duration_ms_)) : 1.0f;
            std::vector<float> previous_positions(rows_.size());
            for(std::size_t i=0;i<rows_.size();++i) {
                const float from=row_animation_active_ && i<row_motion_.size()
                    ?row_motion_[i].start_slot:static_cast<float>(i);
                previous_positions[i]=from+(static_cast<float>(i)-from)*previous_progress;
            }
            previous_rows_=std::move(rows_);
            rows_=std::move(next);
            clear_row_images(); // Rebuild only the rows used by the new transition.
            row_motion_.clear();row_motion_.reserve(rows_.size());
            // Pair PID with its creation stamp: a reused PID must not inherit old motion.
            // Hash lookup replaces quadratic matching for 100-row reorderings.
            std::unordered_multimap<std::uint32_t,std::size_t> previous_by_pid;
            previous_by_pid.reserve(previous_rows_.size());
            for(std::size_t j=0;j<previous_rows_.size();++j)
                previous_by_pid.emplace(previous_rows_[j].pid,j);
            const bool pinned=options_.sort==Sort::automatic && focused_pid_
                && rows_.front().pid==focused_pid_;
            for(std::size_t i=0;i<rows_.size();++i) {
                int old=-1;
                const auto [first,last]=previous_by_pid.equal_range(rows_[i].pid);
                for(auto it=first;it!=last;++it) {
                    if(rows_[i].created==previous_rows_[it->second].created) {
                        old=static_cast<int>(it->second);break;
                    }
                }
                float from=old>=0?previous_positions[static_cast<std::size_t>(old)]
                    :static_cast<float>(i);
                // In Auto, the focused application occupies a reserved first row.
                if(pinned) from=i==0?0.0f:std::max(1.0f,from);
                row_motion_.push_back({from,old});
            }
            // Old rows were only needed to recover prior positions, not for painting.
            // Release their strings and icons immediately instead of retaining two tables.
            previous_rows_.clear();
            // A newly inserted/replaced row at the same slot needs no animation.
            bool has_motion=false;
            float max_slot_travel=0.0f;
            RECT viewport{};GetClientRect(hwnd_,&viewport);
            const auto visible_count=visible_rows(dip(std::max<LONG>(0,viewport.bottom)));
            const auto first_visible=std::min(scroll_,row_motion_.size());
            const auto last_visible=std::min(row_motion_.size(),first_visible+visible_count);
            for(std::size_t i=first_visible;i<last_visible;++i) {
                const float travel=std::abs(row_motion_[i].start_slot-static_cast<float>(i));
                max_slot_travel=std::max(max_slot_travel,travel);
                if(travel>0.01f) has_motion=true;
            }
            row_animation_started_=tick;
            // Reserve two paint ticks at the end of a short sampling period.
            row_animation_duration_ms_=overlay_transition_duration_ms(max_slot_travel,options_.interval_ms);
            row_animation_last_painted_progress_=0.0f;
            row_animation_next_frame_tick_=0;
            row_animation_slow_frame_reported_=false;
            row_animation_active_=has_motion;
            // One lightweight timer cadence for every row count. Actual paints are
            // throttled below according to render cost, not row count.
            if(has_motion) {
                if(!SetTimer(hwnd_,overlay_animation_timer,overlay_animation_timer_ms,nullptr)) {
                    row_animation_active_=false;
                    row_motion_.clear();
                }
            } else {
                KillTimer(hwnd_,overlay_animation_timer);
                row_motion_.clear();
            }
        } else {
            rows_=std::move(next);
            clear_row_images();
            KillTimer(hwnd_,overlay_animation_timer);
            row_animation_active_=false;
            previous_rows_.clear();row_motion_.clear();
        }
        if (visible_) InvalidateRect(hwnd_,nullptr,FALSE);
    }
}

void Overlay::release_surface() {
    clear_row_images(); // Target-dependent bitmaps cannot outlive a device/surface reset.
    icon_bitmaps_.clear(); // D2D bitmaps belong to this render target.
    // DWrite layouts are device-independent and stay cached across target recreation.
    brush_.Reset(); target_.Reset();
    if (dc_) { if (old_bitmap_) SelectObject(dc_, old_bitmap_); DeleteDC(dc_); }
    if (bitmap_) DeleteObject(bitmap_);
    dc_ = nullptr; bitmap_ = nullptr; old_bitmap_ = nullptr; bits_ = nullptr; bitmap_size_ = {};
}
void Overlay::surface() {
    RECT rect{}; GetClientRect(hwnd_, &rect);
    const SIZE size{std::max<LONG>(1,rect.right),std::max<LONG>(1,rect.bottom)};
    if (target_ && size.cx == bitmap_size_.cx && size.cy == bitmap_size_.cy) return;
    release_surface();
    dc_ = CreateCompatibleDC(nullptr); if (!dc_) throw windows_error("CreateCompatibleDC");
    BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(info.bmiHeader); info.bmiHeader.biWidth = size.cx;
    info.bmiHeader.biHeight = -size.cy; info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
    bitmap_ = CreateDIBSection(dc_, &info, DIB_RGB_COLORS, &bits_, nullptr, 0); if (!bitmap_) throw windows_error("Overlay ARGB bitmap");
    old_bitmap_ = SelectObject(dc_, bitmap_); bitmap_size_ = size;
    auto properties = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED),static_cast<float>(dpi_),static_cast<float>(dpi_));
    require_hr(factory_->CreateDCRenderTarget(&properties,&target_), "Transparent render target");
    require_hr(target_->BindDC(dc_,&rect), "Bind transparent bitmap");
    require_hr(target_->CreateSolidColorBrush(D2D1::ColorF(1,1,1,1),&brush_), "Text brush");
    target_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
}
void Overlay::draw_text(std::wstring_view value, D2D1_RECT_F rect, IDWriteTextFormat* font,
    std::optional<double> load) {
    auto shadow=rect;shadow.left+=1;shadow.right+=1;shadow.top+=1;shadow.bottom+=1;
    brush_->SetColor(D2D1::ColorF(0,0,0,.8f));
    target_->DrawText(value.data(),static_cast<UINT32>(value.size()),font,shadow,brush_.Get(),D2D1_DRAW_TEXT_OPTIONS_CLIP);
    brush_->SetColor(overlay_load_color(load));
    target_->DrawText(value.data(),static_cast<UINT32>(value.size()),font,rect,brush_.Get(),D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

// Shape a cell only when its value, geometry, font, or application identity changes.
// Render both shadow and foreground with one immutable layout, without fading.
void Overlay::draw_cached_row_text(const Row& row, unsigned column, std::wstring_view value,
    D2D1_RECT_F rect, IDWriteTextFormat* font, bool highlight, std::optional<double> load,
    ID2D1RenderTarget* destination, ID2D1SolidColorBrush* destination_brush) {
    if (value.empty()) return;
    ID2D1RenderTarget* renderer = destination ? destination : target_.Get();
    ID2D1SolidColorBrush* ink = destination_brush ? destination_brush : brush_.Get();
    auto found = row_text_cache_.find(row.pid);
    if (found == row_text_cache_.end()) {
        const std::size_t cache_capacity = std::clamp<std::size_t>(
            static_cast<std::size_t>(options_.top)+8,24,100);
        if (row_text_cache_.size() >= cache_capacity) {
            const auto oldest = std::min_element(row_text_cache_.begin(),row_text_cache_.end(),
                [](const auto& a,const auto& b){return a.second.used < b.second.used;});
            if (oldest != row_text_cache_.end()) row_text_cache_.erase(oldest);
        }
        found = row_text_cache_.try_emplace(row.pid).first;
    }
    auto& entry = found->second;
    if (entry.created != row.created) {
        entry = CachedRowTextLayouts{};
        entry.created = row.created;
    }
    entry.used = ++row_text_clock_;
    auto& layout = entry.layouts[column];
    auto& cached_text = entry.values[column];
    const float width = std::max(1.0f,rect.right-rect.left);
    const float height = std::max(1.0f,rect.bottom-rect.top);
    if (!layout || cached_text != value || entry.widths[column] != width || entry.heights[column] != height) {
        cached_text.assign(value.data(),value.size());
        entry.widths[column] = width;
        entry.heights[column] = height;
        layout.Reset();
        require_hr(write_factory_->CreateTextLayout(cached_text.c_str(),static_cast<UINT32>(cached_text.size()),
            font,width,height,&layout),"Overlay cached row text");
    }
    ink->SetColor(D2D1::ColorF(0,0,0,0.8f));
    renderer->DrawTextLayout(D2D1::Point2F(rect.left+1,rect.top+1),layout.Get(),ink,D2D1_DRAW_TEXT_OPTIONS_CLIP);
    ink->SetColor(overlay_load_color(row_colors_ && highlight?load:std::nullopt));
    renderer->DrawTextLayout(D2D1::Point2F(rect.left,rect.top),layout.Get(),ink,D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

void Overlay::clear_row_images() noexcept {
    row_images_.clear();
    row_image_bytes_=0;
}

void Overlay::draw_base_row(const Row& row,const OverlayColumnPositions& geometry,float y,
    ID2D1RenderTarget* destination,ID2D1SolidColorBrush* ink) {
    const float bottom=y+row_height();
    const auto cell=[&](std::size_t index) {
        return D2D1::RectF(geometry.left[index],y,geometry.right[index],bottom);
    };
    draw_cached_row_text(row,0,row.name,cell(0),body_font_.Get(),false,std::nullopt,destination,ink);
    const std::array<std::wstring_view,5> values{row.cpu,row.ram,row.gpu,row.vram,row.network};
    for(std::size_t metric=0;metric<values.size();++metric) {
        if(geometry.right[metric+1]<=geometry.left[metric+1])continue;
        const bool highlight=metric<row.loads.size();
        draw_cached_row_text(row,static_cast<unsigned>(metric+1),values[metric],cell(metric+1),
            number_font_.Get(),highlight,highlight?row.loads[metric]:std::optional<double>{},destination,ink);
    }
}
// Rasterize the text of a moving row once; the icon already has its own D2D bitmap cache.
// A fixed memory limit and 6 new images per frame avoid startup spikes at 100 rows.
ID2D1Bitmap* Overlay::cached_row_bitmap(const Row& row,
    const OverlayColumnPositions& geometry, std::size_t& built_this_frame) {
    constexpr std::size_t row_image_budget_bytes = 6U*1024U*1024U;
    constexpr std::size_t max_builds_per_frame = 6;
    auto found=row_images_.find(row.pid);
    if(found!=row_images_.end()) {
        if(found->second.content==row) return found->second.bitmap.Get();
        row_image_bytes_-=found->second.bytes;
        row_images_.erase(found);
    }
    if(built_this_frame>=max_builds_per_frame || row_images_.size()>=100) return nullptr;
    const double density=static_cast<double>(dpi_)/96.0;
    const auto pixel_width=static_cast<std::uint64_t>(std::ceil(geometry.total_width*density));
    const auto pixel_height=static_cast<std::uint64_t>(std::ceil(row_height()*density));
    const auto expected_bytes=pixel_width*pixel_height*4ULL;
    if(!pixel_width || !pixel_height || expected_bytes>row_image_budget_bytes-row_image_bytes_) return nullptr;

    ComPtr<ID2D1BitmapRenderTarget> offscreen;
    if(FAILED(target_->CreateCompatibleRenderTarget(D2D1::SizeF(geometry.total_width,row_height()),&offscreen)))
        return nullptr; // Safe fallback: paint the existing cached text layouts.
    ComPtr<ID2D1SolidColorBrush> ink;
    if(FAILED(offscreen->CreateSolidColorBrush(D2D1::ColorF(1,1,1,1),&ink))) return nullptr;
    offscreen->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    offscreen->BeginDraw();
    offscreen->Clear(D2D1::ColorF(0,0,0,0));
    draw_base_row(row,geometry,0,offscreen.Get(),ink.Get());
    if(FAILED(offscreen->EndDraw())) return nullptr;
    ComPtr<ID2D1Bitmap> bitmap;
    if(FAILED(offscreen->GetBitmap(&bitmap))) return nullptr;
    const auto physical=bitmap->GetPixelSize();
    const auto bytes=static_cast<std::uint64_t>(physical.width)*physical.height*4ULL;
    if(bytes>row_image_budget_bytes-row_image_bytes_) return nullptr;
    CachedRowImage cached;
    cached.content=row;
    cached.bytes=static_cast<std::size_t>(bytes);
    cached.bitmap=std::move(bitmap);
    auto [position,inserted]=row_images_.try_emplace(row.pid,std::move(cached));
    if(!inserted) return nullptr;
    row_image_bytes_+=position->second.bytes;
    ++built_this_frame;
    return position->second.bitmap.Get();
}
void Overlay::draw_header_metric(unsigned column, const std::wstring& reading, std::optional<double> load,
    std::optional<double> temperature, D2D1_RECT_F rect) {
    std::wstring line = metric_names[column];
    line += L" ";
    const auto from = line.size();
    line += reading;
    const auto digits = line.size() - from;
    std::size_t temp_from = 0, temp_length = 0;
    if (column == 0 || column == 2) {
        line += L" ";
        temp_from = line.size();
        line += temperature && std::isfinite(*temperature) ? decimal(*temperature,0) + L"°C" : L"—";
        if (temperature && std::isfinite(*temperature)) temp_length = line.size() - temp_from;
    }
    draw_heading(line,rect,number_font_.Get(),{{
        {from,load && std::isfinite(*load) && *load>=0?digits:0,overlay_load_color(load)},
        {temp_from,temp_length,temp_length?overlay_band_color(temperature_band(*temperature)):D2D1_COLOR_F{}}
    }});
}
void Overlay::draw_heading(std::wstring_view line,D2D1_RECT_F rect,IDWriteTextFormat* font,
    const std::array<ReadingTint,2>& tints) {
    draw_text(line,rect,font);
    if (!header_colors_ || (!tints[0].count && !tints[1].count)) return;
    // A colored CPU/GPU heading has two ranges, but only needs one text layout.
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(write_factory_->CreateTextLayout(line.data(),static_cast<UINT32>(line.size()),
        font,rect.right-rect.left,rect.bottom-rect.top,&layout))) return;
    for(const auto& tint:tints) {
        if(!tint.count)continue;
        DWRITE_HIT_TEST_METRICS hits[8]{};
        UINT32 count = 0;
        if (FAILED(layout->HitTestTextRange(static_cast<UINT32>(tint.first),static_cast<UINT32>(tint.count),
            0,0,hits,8,&count))) continue;
        for (UINT32 i=0;i<count;++i) {
            const auto& hit = hits[i];
            if (hit.width <= 0 || hit.height <= 0) continue;
            target_->PushAxisAlignedClip(D2D1::RectF(rect.left+hit.left,rect.top+hit.top,
                rect.left+hit.left+hit.width,rect.top+hit.top+hit.height),D2D1_ANTIALIAS_MODE_ALIASED);
            brush_->SetColor(tint.color);
            target_->DrawText(line.data(),static_cast<UINT32>(line.size()),font,
                rect,brush_.Get(),D2D1_DRAW_TEXT_OPTIONS_CLIP);
            target_->PopAxisAlignedClip();
        }
    }
}
void Overlay::draw_overlay_charts(float width,float top) {
    struct ChartVertex {
        D2D1_POINT_2F point;
        bool connected;
    };
    const auto& layout=chart_layout_;
    if(!layout.capacity)return;
    target_->SetTransform(D2D1::Matrix3x2F::Identity());
    const auto first=chart_page_*layout.capacity;
    const auto last=std::min(first+layout.capacity,selected_charts_.size());
    const auto& samples=graph_history_->points();
    const auto end=samples.empty()?0:samples.back().time;
    constexpr std::int64_t duration=60000;
    const auto start=end-duration;
    const auto begin=std::lower_bound(samples.begin(),samples.end(),start,
        [](const SystemChartPoint& point,std::int64_t time){return point.time<time;});
    std::array<std::vector<ChartVertex>,2> series_points;
    for(auto& points:series_points)points.reserve(static_cast<std::size_t>(samples.end()-begin));
    const float tile_width=(width-layout.gap*(static_cast<float>(layout.columns)+1))/static_cast<float>(layout.columns);
    if(tile_width<=0)return;
    for(auto index=first;index<last;++index) {
        const auto& chart=chart_choices_[selected_charts_[index]];
        const auto kind=chart.kind;
        const auto values=samples.empty()?OverlayChartValues{}:overlay_chart_values(chart,samples.back());
        const float x=layout.gap+static_cast<float>((index-first)%layout.columns)*(tile_width+layout.gap);
        const float y=top+layout.gap+static_cast<float>((index-first)/layout.columns)*(layout.tile_height+layout.gap);
        const float plot_top=y+row_height();
        const float plot_bottom=y+layout.tile_height-2;
        const bool temperature=kind==OverlayChartKind::cpu_temperature;
        const bool variable=kind==OverlayChartKind::fps || kind==OverlayChartKind::network;
        const wchar_t* unit=temperature?L" °C":kind==OverlayChartKind::fps?L"":
            kind==OverlayChartKind::network?L" Mbps":L"%";
        const bool has_value=values.primary && std::isfinite(*values.primary) && *values.primary>=0;
        const auto reading=has_value?decimal(*values.primary)+unit:std::wstring(L"—");
        std::wstring title=chart.title+L"  ";
        const auto primary_offset=title.size();
        title+=reading;
        const auto secondary_offset=title.size()+3;
        const bool paired=kind==OverlayChartKind::core || kind==OverlayChartKind::disk;
        if(paired)
            title+=L" · "+metric_text::temperature(values.secondary);
        const bool has_secondary=paired && values.secondary && std::isfinite(*values.secondary);
        draw_heading(title,D2D1::RectF(x,y,x+tile_width,plot_top),body_font_.Get(),{{
            {primary_offset,has_value && !variable?reading.size():0,
                temperature && has_value?overlay_band_color(temperature_band(*values.primary)):overlay_load_color(values.primary)},
            {secondary_offset,has_secondary?title.size()-secondary_offset:0,
                has_secondary?overlay_band_color(temperature_band(*values.secondary)):D2D1_COLOR_F{}}
        }});
        double primary_max=temperature?130.0:100.0;
        if(variable) {
            primary_max=1.0;
            for(auto sample=begin;sample!=samples.end();++sample)
                if(const auto value=overlay_chart_values(chart,*sample).primary)
                    primary_max=std::max(primary_max,*value*1.1);
        }
        target_->PushAxisAlignedClip(D2D1::RectF(x,plot_top,x+tile_width,plot_bottom),D2D1_ANTIALIAS_MODE_ALIASED);
        for(auto& points:series_points)points.clear();
        std::array<bool,2> connected{};
        std::int64_t previous_time{};
        const FocusedFpsSource* previous_source=nullptr;
        const std::array<D2D1_COLOR_F,2> colors{chart_ink(overlay_chart_series_color(kind)),
            chart_ink(cpu_temperature_color)};
        for(auto sample=begin;sample!=samples.end();++sample) {
            const auto current=overlay_chart_values(chart,*sample);
            const auto* source=sample->fps?sample->fps->source.get():nullptr;
            if(sample->time-previous_time>2LL*options_.interval_ms ||
                (kind==OverlayChartKind::fps && source!=previous_source))connected={};
            previous_time=sample->time;
            previous_source=source;
            const auto px=x+static_cast<float>(sample->time-start)/static_cast<float>(duration)*(tile_width-1);
            const std::array readings{current.primary,current.secondary};
            for(std::size_t series=0;series<readings.size();++series) {
                if(!readings[series] || !std::isfinite(*readings[series])) {
                    connected[series]=false;continue;
                }
                const auto maximum=series==0?primary_max:130.0;
                const auto ratio=static_cast<float>(std::clamp(*readings[series]/maximum,0.0,1.0));
                const auto point=D2D1::Point2F(px,plot_bottom-1-ratio*(plot_bottom-plot_top-2));
                series_points[series].push_back({point,connected[series]});
                connected[series]=true;
            }
        }
        // Fill each uninterrupted segment before painting any strokes.
        for(std::size_t series=0;series<series_points.size();++series) {
            const auto& points=series_points[series];
            if(std::none_of(points.begin(),points.end(),[](const ChartVertex& vertex){return vertex.connected;}))
                continue;
            ComPtr<ID2D1PathGeometry> area;
            ComPtr<ID2D1GeometrySink> sink;
            if(FAILED(factory_->CreatePathGeometry(area.GetAddressOf())) || FAILED(area->Open(sink.GetAddressOf())))
                continue;
            const float baseline=plot_bottom-1;
            for(auto segment=points.begin();segment!=points.end();) {
                const auto next=std::find_if(segment+1,points.end(),
                    [](const ChartVertex& vertex){return !vertex.connected;});
                if(next-segment>=2) {
                    sink->BeginFigure(D2D1::Point2F(segment->point.x,baseline),D2D1_FIGURE_BEGIN_FILLED);
                    for(auto vertex=segment;vertex!=next;++vertex)sink->AddLine(vertex->point);
                    sink->AddLine(D2D1::Point2F((next-1)->point.x,baseline));
                    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                }
                segment=next;
            }
            if(SUCCEEDED(sink->Close())) {
                auto fill=colors[series];
                fill.a*=0.20f;
                brush_->SetColor(fill);
                target_->FillGeometry(area.Get(),brush_.Get());
            }
        }
        for(std::size_t series=0;series<series_points.size();++series) {
            const auto& points=series_points[series];
            for(auto vertex=points.begin();vertex!=points.end();++vertex) {
                if(vertex->connected) {
                    brush_->SetColor(D2D1::ColorF(0,0,0,.8f));
                    target_->DrawLine((vertex-1)->point,vertex->point,brush_.Get(),3);
                    brush_->SetColor(colors[series]);
                    target_->DrawLine((vertex-1)->point,vertex->point,brush_.Get(),1.5f);
                } else {
                    brush_->SetColor(colors[series]);
                    target_->FillEllipse(D2D1::Ellipse(vertex->point,1,1),brush_.Get());
                }
            }
        }
        target_->PopAxisAlignedClip();
    }
    if(selected_charts_.size()>layout.capacity) {
        const auto pages=(selected_charts_.size()+layout.capacity-1)/layout.capacity;
        const auto page_number=std::to_wstring(chart_page_+1)+L" / "+std::to_wstring(pages);
        const auto y=top+layout.height-text_size()-layout.gap;
        draw_text(page_number,D2D1::RectF(layout.gap,y,width-layout.gap,y+row_height()),number_font_.Get());
    }
}
void Overlay::paint(bool animation_only) {
    if (!visible_ || !hwnd_) return;
    RECT client{};GetClientRect(hwnd_,&client);
    // A new/resized target has no previously painted heading to preserve.
    if(!target_ || bitmap_size_.cx!=std::max<LONG>(1,client.right) ||
        bitmap_size_.cy!=std::max<LONG>(1,client.bottom)) animation_only=false;
    surface();target_->BeginDraw();
    const auto count=visible_rows(dip(bitmap_size_.cy));
    scroll_=std::min(scroll_,rows_.size()>count?rows_.size()-count:0);
    const float progress=row_animation_active_ && row_animation_duration_ms_
        ?overlay_smoothstep(static_cast<float>(GetTickCount64()-row_animation_started_)
            /static_cast<float>(row_animation_duration_ms_)):1.0f;
    // On an animation tick, find the Y-band occupied by changing positions in
    // the previous and current frame. Static rows outside this band are reused.
    const float body_top=row_height()+1;
    const float table_bottom=std::max(body_top,dip(bitmap_size_.cy)-chart_layout_.height);
    float dirty_top=body_top,dirty_bottom=table_bottom;
    bool partial_body=false;
    if(animation_only && dip(bitmap_size_.cy)>body_top && row_animation_active_ && !row_motion_.empty()) {
        float low=dirty_bottom,high=body_top;
        const bool fixed_focus=options_.sort==Sort::automatic && focused_pid_
            && !rows_.empty() && rows_.front().pid==focused_pid_;
        for(std::size_t i=0;i<count && i+scroll_<rows_.size();++i) {
            const auto index=(fixed_focus && i==0)?0:i+scroll_;
            if(index>=row_motion_.size() || (fixed_focus && i==0))continue;
            const float from=row_motion_[index].start_slot-static_cast<float>(scroll_);
            const float target_slot=static_cast<float>(i);
            const float previous=from+(target_slot-from)*row_animation_last_painted_progress_;
            const float current=from+(target_slot-from)*progress;
            if(std::abs(previous-current)<0.005f)continue;
            low=std::min(low,1+(std::min(previous,current)+1)*row_height()-3.0f);
            high=std::max(high,1+(std::max(previous,current)+2)*row_height()+3.0f);
        }
        if(high>low) {
            dirty_top=std::clamp(low,body_top,dirty_bottom);
            dirty_bottom=std::clamp(high,body_top,table_bottom);
            partial_body=dirty_bottom>dirty_top;
        }
    }
    // Clear with the identity transform: preserve the header and any still
    // valid row pixels; never use opacity/fade to hide stale content.
    target_->SetTransform(D2D1::Matrix3x2F::Identity());
    const auto global_alpha=static_cast<BYTE>(opacity_*255/100);
    // Layered windows ignore fully transparent pixels in hit testing.
    const auto background=D2D1::ColorF(0,0,0,manual_movement_?2.0f/global_alpha:0.0f);
    if(animation_only && dip(bitmap_size_.cy)>body_top) {
        target_->PushAxisAlignedClip(D2D1::RectF(0,dirty_top,
            dip(bitmap_size_.cx),dirty_bottom),D2D1_ANTIALIAS_MODE_ALIASED);
        target_->Clear(background);
        target_->PopAxisAlignedClip();
    } else {
        animation_only=false;
        partial_body=false;
        target_->Clear(background);
    }
    // Column positions are immutable within one frame; don't recompute per row.
    const auto selected_disk_letters=enabled_disk_letters();
    const auto table=table_geometry(selected_disk_letters.size());
    const auto& geometry=table.base;
    const auto scale=std::min(1.0f,dip(bitmap_size_.cx)/table.total_width);
    target_->SetTransform(D2D1::Matrix3x2F::Scale(scale,1));
    const float height=row_height();
    const auto dynamic_cell=[&](const OverlayColumnRun& run,std::size_t index,float y) {
        return D2D1::RectF(run.left(index),y,run.right(index),y+height);
    };
    if(!animation_only) {
    std::array<D2D1_RECT_F,6> header{};
    for(std::size_t i=0;i<header.size();++i)
        header[i]=D2D1::RectF(geometry.left[i],1,geometry.right[i],1+height);
    header[0].left = overlay_layout.left_padding; // Above the icon column.
    std::wstring fps_heading = summary_only()?L"FPS: ":L"Приложение  FPS: ";
    if (foreground_fps_)
        fps_heading += std::to_wstring(static_cast<unsigned>(std::lround(*foreground_fps_)));
    else fps_heading += L"—"; // Never guess frames when the source is unavailable.
    draw_text(fps_heading,header[0],body_font_.Get());
    std::array<std::wstring,5> totals{L"—",L"—",L"—",L"—",L"— / —"};
    if (snapshot_) {
        const auto& s = *snapshot_;
        std::optional<double> ram;
        if (s.total_ram_used && s.total_ram_capacity && *s.total_ram_capacity)
            ram = 100.0 * static_cast<double>(*s.total_ram_used) / static_cast<double>(*s.total_ram_capacity);
        totals = {percent(s.total_cpu_percent),percent(ram),percent(s.total_gpu_percent),
            memory(s.total_gpu_memory),network_rate(s.total_rx_bytes_per_second,s.total_tx_bytes_per_second)};
    }
    std::array<std::optional<double>,5> load{};
    std::optional<double> cpu_temperature, gpu_temperature;
    if (snapshot_) {
        const auto& s = *snapshot_;
        load[0] = s.total_cpu_percent;
        if (s.total_ram_used && s.total_ram_capacity && *s.total_ram_capacity)
            load[1] = 100.0 * static_cast<double>(*s.total_ram_used) / static_cast<double>(*s.total_ram_capacity);
        load[2] = s.total_gpu_percent;
        if (s.total_gpu_memory && s.total_gpu_memory_capacity && *s.total_gpu_memory_capacity) {
            load[3] = 100.0 * static_cast<double>(*s.total_gpu_memory) / static_cast<double>(*s.total_gpu_memory_capacity);
            totals[3] = decimal(static_cast<double>(*s.total_gpu_memory) / 1073741824.0) + L" GiB";
        }
        cpu_temperature = s.cpu_temperature_c;
        gpu_temperature = s.gpu_temperature_c;
    }
    for (std::size_t i=1;i<header.size();++i) {
        const unsigned metric=static_cast<unsigned>(i-1);
        if (geometry.right[i]<=geometry.left[i]) continue;
        draw_header_metric(metric,totals[metric],load[metric],
            metric==0U ? cpu_temperature : metric==2U ? gpu_temperature : std::optional<double>{},header[i]);
    }
    if(disk_total_enabled_) {
        draw_text(L"Disks",dynamic_cell(table.disks,0,1),number_font_.Get());
    }
    if(snapshot_) {
        for(std::size_t i=0;i<selected_disk_letters.size();++i) {
            const auto found=std::find_if(snapshot_->disks.begin(),snapshot_->disks.end(),
                [&](const DiskActivity& d){return d.letter==selected_disk_letters[i];});
            const auto reading=found==snapshot_->disks.end()?std::optional<double>{}:found->active_percent;
            const auto area=dynamic_cell(table.disks,i+(disk_total_enabled_?1U:0U),1);
            const std::wstring title=metric_text::disk_name(selected_disk_letters[i])+L" "+metric_text::disk_activity(reading);
            draw_text(title,area,number_font_.Get(),header_colors_?reading:std::nullopt);
        }
    }
    const auto core_readings=snapshot_ && !cpu_columns_.empty()?cpu_chart_sample(*snapshot_):CpuChartSample{};
    for(std::size_t i=0;i<cpu_columns_.size();++i){
        const auto& column=cpu_columns_[i];std::optional<double> reading;
        reading=cpu_chart_value({CpuSeriesKind::core_load,column.id,L"",false},core_readings,{});
        const auto temperature=cpu_chart_value({CpuSeriesKind::core,column.id,L"",false},core_readings,{});
        const auto core=std::find_if(core_readings.cores.begin(),core_readings.cores.end(),[&](const auto& item){return column.id==CpuViewId{item.group,item.core_id};});
        const auto name=core==core_readings.cores.end()?std::wstring(L"Core ")+std::to_wstring(column.id.number+1):cpu_core_label(*core,core_readings.cores);
        draw_text(name+L" "+percent(reading)+L" "+metric_text::temperature(temperature),
            dynamic_cell(table.cores,i,1),number_font_.Get(),header_colors_?reading:std::nullopt);
    }
    } // Header values and layout stay untouched during animation-only frames.
    // The header never moves or gets covered by transitioning rows.
    target_->PushAxisAlignedClip(D2D1::RectF(0,row_height()+1,table.total_width,
        table_bottom),D2D1_ANTIALIAS_MODE_ALIASED);
    if(partial_body)
        target_->PushAxisAlignedClip(D2D1::RectF(0,dirty_top,table.total_width,dirty_bottom),
            D2D1_ANTIALIAS_MODE_ALIASED);
    std::size_t row_bitmap_builds=0;
    for (std::size_t i=0;i<count && i+scroll_<rows_.size();++i) {
        // Row zero remains visible while the remaining auto-ranked rows scroll.
        const bool fixed_focus = options_.sort == Sort::automatic && focused_pid_
            && !rows_.empty() && rows_.front().pid == focused_pid_;
        const std::size_t row_index=(fixed_focus && i==0)?0:i+scroll_;
        const auto& row=rows_[row_index];
        const RowMotion* motion=row_animation_active_ && row_index<row_motion_.size()
            ?&row_motion_[row_index]:nullptr;
        float slot=static_cast<float>(i);
        if(motion && !(fixed_focus && i==0)) {
            const float from=motion->start_slot-static_cast<float>(scroll_);
            slot=from+(slot-from)*progress;
            if(fixed_focus) slot=std::max(1.0f,slot);
        }
        const float opacity=1.0f; // Never fade newly appearing rows or their icons.
        const float y=1+(slot+1.0f)*row_height();
        // Do not issue hundreds of DirectWrite draws for untouched static rows.
        if(partial_body && (y+row_height()+2<dirty_top || y-2>dirty_bottom))continue;
        if (show_icons_ && icons_) if (const auto icon = icons_->get(row.icon_path)) {
            auto found=icon_bitmaps_.find(row.pid);
            if (found==icon_bitmaps_.end() || found->second.created!=row.created || found->second.source!=icon) {
                // A bounded LRU protects memory when many short-lived processes appear.
                constexpr std::size_t bitmap_capacity=128;
                if (found==icon_bitmaps_.end() && icon_bitmaps_.size()>=bitmap_capacity) {
                    const auto oldest=std::min_element(icon_bitmaps_.begin(),icon_bitmaps_.end(),
                        [](const auto& a,const auto& b){return a.second.used<b.second.used;});
                    icon_bitmaps_.erase(oldest);
                }
                CachedBitmap entry;
                entry.created=row.created;
                entry.source=icon;
                require_hr(target_->CreateBitmap(D2D1::SizeU(AppIcon::size,AppIcon::size),icon->pixels.data(),AppIcon::size*4,
                    D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED)),
                    &entry.bitmap),"Application icon bitmap");
                found=icon_bitmaps_.insert_or_assign(row.pid,std::move(entry)).first;
            }
            found->second.used=++icon_bitmap_clock_;
            const float side=text_size()*1.3f, top=y+(row_height()-side)/2;
            const float left=overlay_layout.left_padding;
            target_->DrawBitmap(found->second.bitmap.Get(),D2D1::RectF(left,top,left+side,top+side),opacity);
        }
        // During reordering, move already-rasterized text. Stationary rows outside
        // the dirty Y-band have already been skipped; no repeated text shaping.
        ID2D1Bitmap* row_bitmap = row_animation_active_
            ? cached_row_bitmap(row,geometry,row_bitmap_builds) : nullptr;
        if(row_bitmap) {
            target_->DrawBitmap(row_bitmap,D2D1::RectF(0,y,geometry.total_width,y+height),
                1.0f,D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        } else {
            // Low-memory / no-animation fallback uses the existing layout cache.
            draw_base_row(row,geometry,y);
        }
        // Disk columns are read/write throughput attributed to this application,
        // unlike the system-wide PDH percentages shown in the header.
        const bool disk_partial=snapshot_ && snapshot_->disk_io_status=="partial-attribution";
        const auto disk_text=[&](std::optional<double> rate) {
            return metric_text::disk_rate(rate,disk_partial);
        };
        if(disk_total_enabled_) {
            std::optional<double> total;
            if(row.disk_read && row.disk_write)total=*row.disk_read+*row.disk_write;
            draw_text(disk_text(total),dynamic_cell(table.disks,0,y),number_font_.Get());
        }
        for(std::size_t d=0;d<selected_disk_letters.size();++d) {
            const auto index=static_cast<unsigned>(selected_disk_letters[d]-L'A');
            draw_text(disk_text(row.disk_volumes[index]),dynamic_cell(table.disks,d+(disk_total_enabled_?1U:0U),y),number_font_.Get());
        }
        for(std::size_t c=0;c<cpu_columns_.size();++c){
            const auto reading=c<row.logical_cpu.size()?row.logical_cpu[c]:std::optional<double>{};
            draw_text(percent(reading),dynamic_cell(table.cores,c,y),
                number_font_.Get(),row_colors_?reading:std::nullopt);
        }
    }
    if(partial_body)target_->PopAxisAlignedClip();
    target_->PopAxisAlignedClip();
    if(!animation_only)draw_overlay_charts(dip(bitmap_size_.cx),table_bottom);
    const auto result=target_->EndDraw();
    if (result == D2DERR_RECREATE_TARGET) { release_surface(); return; }
    require_hr(result,"Transparent table paint");
    RECT rect{}; GetWindowRect(hwnd_,&rect); POINT destination{rect.left,rect.top}, source{};
    BLENDFUNCTION blend{AC_SRC_OVER,0,global_alpha,AC_SRC_ALPHA};
    if (!UpdateLayeredWindow(hwnd_,nullptr,&destination,&bitmap_size_,dc_,&source,0,&blend,ULW_ALPHA)) throw windows_error("UpdateLayeredWindow");
    if(row_animation_active_) row_animation_last_painted_progress_=progress;
}

int Overlay::run() {
    diagnostics::write(diagnostics::Level::info,"ui.start","overlay startup");
    load_settings(); dpi_ = GetDpiForSystem(); load_icons();
    require_hr(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,factory_.GetAddressOf()), "Direct2D factory");
    require_hr(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,__uuidof(IDWriteFactory),reinterpret_cast<IUnknown**>(write_factory_.GetAddressOf())), "DirectWrite factory");
    fonts();
    WNDCLASSEXW cls{}; cls.cbSize=sizeof(cls); cls.hInstance=GetModuleHandleW(nullptr); cls.hIcon=large_icon_; cls.hIconSm=icon_;
    cls.hCursor=LoadCursorW(nullptr,IDC_ARROW); cls.lpfnWndProc=dispatch; cls.lpszClassName=overlay_class;
    register_window_class(cls,"Register overlay class");
    cls.lpfnWndProc=client_dispatch; cls.lpszClassName=client_class; cls.hbrBackground=reinterpret_cast<HBRUSH>(COLOR_BTNFACE+1);
    register_window_class(cls,"Register client class");
    if (x_==CW_USEDEFAULT || y_==CW_USEDEFAULT) { RECT area{}; SystemParametersInfoW(SPI_GETWORKAREA,0,&area,0); x_=area.right-pixels(table_width())-pixels(20); y_=area.top+pixels(20); }
    const bool inspect = env(L"FLUXTOP_OVERLAY_INSPECT")==L"1";
    inspect_overlay_=inspect || (options_.samples && !env(L"FLUXTOP_OVERLAY_CAPTURE").empty());
    const DWORD style=WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | (inspect ? WS_EX_APPWINDOW : WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE);
    hwnd_=CreateWindowExW(style,overlay_class,L"Оверлей ресурсов",WS_POPUP,x_,y_,pixels(table_width()),pixels(row_height()*11+4),
        nullptr,nullptr,GetModuleHandleW(nullptr),this);
    if (!hwnd_) throw windows_error("Create transparent table");
    dpi_=GetDpiForWindow(hwnd_); fit_table(); taskbar_created_=RegisterWindowMessageW(L"TaskbarCreated"); add_tray();
    toggle_key_=RegisterHotKey(hwnd_,toggle_hotkey,MOD_CONTROL|MOD_ALT|MOD_NOREPEAT,VK_F10)!=FALSE;
    client_key_=RegisterHotKey(hwnd_,client_hotkey,MOD_CONTROL|MOD_ALT|MOD_NOREPEAT,VK_F9)!=FALSE;
    foreground_update_target = hwnd_;
    foreground_hook_ = SetWinEventHook(EVENT_SYSTEM_FOREGROUND,EVENT_SYSTEM_FOREGROUND,
        nullptr,foreground_changed,0,0,WINEVENT_OUTOFCONTEXT);
    // If the hook is unavailable, the regular snapshot still updates focused PID.
    update_foreground();
    if ((inspect && !options_.tray) || (!tray_ && !client_key_ && !options_.samples)) show_client();
    worker_.start(options_,[this](std::optional<Snapshot> snapshot,std::string error,bool finished) {
        std::lock_guard lock(mailbox_mutex_); pending_=std::move(snapshot); pending_error_=std::move(error); pending_finished_=finished;
        if (!posted_) posted_=PostMessageW(hwnd_,sample_message,0,0)!=FALSE;
    });
    MSG msg{}; BOOL result{};
    while ((result=GetMessageW(&msg,nullptr,0,0))>0) {
        if (!client_ || !IsDialogMessageW(client_,&msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
    worker_.stop(); icons_.reset();
    if (result<0) throw windows_error("GetMessageW"); return static_cast<int>(msg.wParam);
}

LRESULT CALLBACK Overlay::dispatch(HWND window,UINT msg,WPARAM wp,LPARAM lp) {
    auto* app=reinterpret_cast<Overlay*>(GetWindowLongPtrW(window,GWLP_USERDATA));
    if (msg==WM_NCCREATE) { app=static_cast<Overlay*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams); app->hwnd_=window; SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(app)); }
    if (!app) return DefWindowProcW(window,msg,wp,lp);
    try { return app->message(msg,wp,lp); }
    catch (const std::exception& error) {
        diagnostics::write(diagnostics::Level::error,"ui.exception",error.what());
        app->exit_code_=1;
        if (!app->options_.samples) MessageBoxW(app->client_,wide(error.what()).c_str(),L"FluxTop — ошибка",MB_OK|MB_ICONERROR);
        DestroyWindow(window); return 0;
    }
}
LRESULT CALLBACK Overlay::client_dispatch(HWND window,UINT msg,WPARAM wp,LPARAM lp) {
    auto* app=reinterpret_cast<Overlay*>(GetWindowLongPtrW(window,GWLP_USERDATA));
    if (msg==WM_NCCREATE) { app=static_cast<Overlay*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams); app->client_=window; SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(app)); }
    if (!app) return DefWindowProcW(window,msg,wp,lp);
    try { return app->client_message(window,msg,wp,lp); }
    catch (const std::exception& error) {
        diagnostics::write(diagnostics::Level::error,"ui.client_exception",error.what());
        MessageBoxW(window,wide(error.what()).c_str(),L"FluxTop — ошибка клиента",MB_OK|MB_ICONERROR); DestroyWindow(window); return 0; }
}
LRESULT Overlay::client_message(HWND window,UINT msg,WPARAM wp,LPARAM lp) {
    switch (msg) {
    case WM_CREATE: client_dpi_=GetDpiForWindow(window); init_client(); return 0;
    case WM_SIZE:
        // Minimize is not Close. Detached graph focus transitions cannot close the client.
        if (wp==SIZE_MINIMIZED) return 0;
        break;
    case WM_DPICHANGED: { client_dpi_=HIWORD(wp); const auto* r=reinterpret_cast<RECT*>(lp); SetWindowPos(window,nullptr,r->left,r->top,0,0,SWP_NOSIZE|SWP_NOZORDER); layout_client(); return 0; }
    case WM_APP+215: {
        // Statistics live-graph slider changed; mirror to overlay without recursion.
        const auto requested=std::clamp(static_cast<unsigned>(wp),1U,100U);
        if(options_.top!=requested) {
            options_.top=requested;
            SendDlgItemMessageW(window,top_control,TBM_SETPOS,TRUE,requested);
            SetDlgItemTextW(window,top_label,
                (L"Количество приложений: "+std::to_wstring(requested)+L" / 100").c_str());
            fit_table();update_view();save_settings();client_stats();
            InvalidateRect(hwnd_,nullptr,FALSE);
        }
        return 0;
    }
    case WM_HSCROLL:
        if (reinterpret_cast<HWND>(lp)==GetDlgItem(window,font_control) ||
            reinterpret_cast<HWND>(lp)==GetDlgItem(window,interval_control)) {
            const bool is_font=reinterpret_cast<HWND>(lp)==GetDlgItem(window,font_control);
            const unsigned selected=static_cast<unsigned>(SendMessageW(reinterpret_cast<HWND>(lp),TBM_GETPOS,0,0));
            if(is_font)
                SetDlgItemTextW(window,font_label,(L"Размер текста: "+std::to_wstring(selected)+L" px (5–42)").c_str());
            else
                SetDlgItemTextW(window,interval_label,interval_caption(selected * interval_step_ms).c_str());
            if(LOWORD(wp)==TB_ENDTRACK || LOWORD(wp)==TB_THUMBPOSITION) {
                KillTimer(window,settings_timer);
                apply_client();
            } else SetTimer(window,settings_timer,150,nullptr);
            return 0;
        }
        if (reinterpret_cast<HWND>(lp)==GetDlgItem(window,top_control)) {
            const auto selected=static_cast<unsigned>(SendDlgItemMessageW(window,top_control,TBM_GETPOS,0,0));
            SetDlgItemTextW(window,top_label,(L"Количество приложений: " + std::to_wstring(selected) + L" / 100").c_str());
            if (LOWORD(wp)==TB_ENDTRACK || LOWORD(wp)==TB_THUMBPOSITION) {
                KillTimer(window,settings_timer);
                apply_client();
            } else SetTimer(window,settings_timer,150,nullptr);
            return 0;
        }
        if (reinterpret_cast<HWND>(lp)==GetDlgItem(window,alpha_control)) {
            opacity_=static_cast<unsigned>(SendDlgItemMessageW(window,alpha_control,TBM_GETPOS,0,0));
            SetDlgItemTextW(window,alpha_label,(L"Непрозрачность: "+std::to_wstring(opacity_)+L"%").c_str());
            InvalidateRect(hwnd_,nullptr,FALSE); SetTimer(window,settings_timer,300,nullptr); return 0;
        }
        break;
    case WM_NOTIFY:
        if(tabs_ && reinterpret_cast<NMHDR*>(lp)->hwndFrom==tabs_
            && reinterpret_cast<NMHDR*>(lp)->code==TCN_SELCHANGE) {
            switch_page(static_cast<unsigned>(TabCtrl_GetCurSel(tabs_)));
            return 0;
        }
        break;
    case WM_COMMAND:
        if (initializing_client_) return 0;
        if(LOWORD(wp)>=table_options_group && LOWORD(wp)<=diagnostic_options_group && HIWORD(wp)==BN_CLICKED) {
            open_group_=open_group_==LOWORD(wp)?0:LOWORD(wp);
            layout_client();
            return 0;
        }
        if(LOWORD(wp)>=chart_checkbox_control && LOWORD(wp)<chart_checkbox_control+chart_checks_.size() &&
            HIWORD(wp)==BN_CLICKED) {
            const auto index=LOWORD(wp)-chart_checkbox_control;
            auto& chart=chart_choices_[index];
            chart.enabled=SendMessageW(chart_checks_[index],BM_GETCHECK,0,0)==BST_CHECKED;
            update_selected_charts();
            fit_table();
            save_settings();
            if(!settings_.empty())ini_overlay_write(settings_,chart.key.c_str(),chart.enabled);
            InvalidateRect(hwnd_,nullptr,FALSE);
            return 0;
        }
        diagnostics::write(diagnostics::Level::trace,"ui.command",
            "control_id="+std::to_string(LOWORD(wp))+" notification="+std::to_string(HIWORD(wp)));
        if(LOWORD(wp)>=cpu_checkbox_control && LOWORD(wp)<cpu_checkbox_control+cpu_checks_.size() && HIWORD(wp)==BN_CLICKED){
            const auto& [box,index]=cpu_checks_[LOWORD(wp)-cpu_checkbox_control];
            cpu_view_.columns[index].visible=SendMessageW(box,BM_GETCHECK,0,0)==BST_CHECKED;
            cpu_settings_changed();return 0;
        }
        if(LOWORD(wp)>=cpu_package_control && LOWORD(wp)<=cpu_average_control && HIWORD(wp)==BN_CLICKED){
            cpu_view_.package=SendDlgItemMessageW(client_,cpu_package_control,BM_GETCHECK,0,0)==BST_CHECKED;
            cpu_view_.maximum=SendDlgItemMessageW(client_,cpu_max_control,BM_GETCHECK,0,0)==BST_CHECKED;
            cpu_view_.average=SendDlgItemMessageW(client_,cpu_average_control,BM_GETCHECK,0,0)==BST_CHECKED;cpu_settings_changed();return 0;
        }
        if (LOWORD(wp)==sort_control && HIWORD(wp)==CBN_SELCHANGE) {
            apply_client(); return 0;
        }
        if(LOWORD(wp)==disk_total_checkbox_control && HIWORD(wp)==BN_CLICKED) {
            apply_client();return 0;
        }
        if(LOWORD(wp)>=disk_checkbox_control && LOWORD(wp)<disk_checkbox_control+26U
            && HIWORD(wp)==BN_CLICKED) {
            apply_client();return 0;
        }
        if (LOWORD(wp)>=metric_control && LOWORD(wp)<metric_control+metric_names.size() && HIWORD(wp)==BN_CLICKED) {
            apply_client(); return 0;
        }
        switch (LOWORD(wp)) {
        case startup_control:
            if (HIWORD(wp)==BN_CLICKED) {
                try { set_autostart(SendDlgItemMessageW(window,startup_control,BM_GETCHECK,0,0)==BST_CHECKED); }
                catch (const std::exception& error) { MessageBoxW(window,wide(error.what()).c_str(),L"Автозапуск FluxTop",MB_OK|MB_ICONERROR); }
                SendDlgItemMessageW(window,startup_control,BM_SETCHECK,autostart_enabled() ? BST_CHECKED : BST_UNCHECKED,0);
            }
            return 0;
        case icons_control: if (HIWORD(wp)==BN_CLICKED) apply_client(); return 0;
        case summary_control: if (HIWORD(wp)==BN_CLICKED) apply_client(); return 0;
        case header_colors_control: if (HIWORD(wp)==BN_CLICKED) apply_client(); return 0;
        case row_colors_control: if (HIWORD(wp)==BN_CLICKED) apply_client(); return 0;
        case animation_control: if (HIWORD(wp)==BN_CLICKED) apply_client(); return 0;
        case manual_move_control:
            if(HIWORD(wp)==BN_CLICKED)
                set_manual_movement(SendDlgItemMessageW(window,manual_move_control,BM_GETCHECK,0,0)==BST_CHECKED);
            return 0;
        case move_control:
            if(HIWORD(wp)==BN_CLICKED)
                if(auto position=edit_overlay_scene(window,hwnd_,scene_position_,pixels(8),scene_dialog_)) {
                    scene_position_=std::move(position);
                    fit_table();save_settings();InvalidateRect(hwnd_,nullptr,FALSE);
                }
            return 0;
        case blacklist_button:
            if(HIWORD(wp)==BN_CLICKED) {
                ensure_icon_cache();
                const auto cache=icons_; // Keep the worker alive until the modal dialog closes.
                edit_overlay_blacklist(window,learned_apps_,*cache,blacklist_dialog_,[this](std::size_t index,bool blocked){
                    learned_apps_[index].blocked=blocked;
                    save_application(index);
                    update_overlay_visibility();
                });
                release_unused_icon_cache();
            }
            return 0;
        case visible_control:
            if(HIWORD(wp)==BN_CLICKED) {
                always_show_=SendDlgItemMessageW(window,visible_control,BM_GETCHECK,0,0)==BST_CHECKED;
                update_overlay_visibility();save_settings();
            }
            return 0;
        case logs_control:
            if (HIWORD(wp)==BN_CLICKED) {
                const auto folder=diagnostics::log_directory();
                std::error_code error;
                if(!folder.empty())std::filesystem::create_directories(folder,error);
                if(folder.empty() || error ||
                    reinterpret_cast<INT_PTR>(ShellExecuteW(window,L"open",folder.c_str(),nullptr,nullptr,SW_SHOWNORMAL))<=32)
                    MessageBoxW(window,L"Не удалось открыть папку с логами.",L"Логи FluxTop",MB_OK|MB_ICONWARNING);
            }
            return 0;
        case quit_app: DestroyWindow(hwnd_); return 0;
        }
        break;
    case WM_VSCROLL:case WM_MOUSEWHEEL:return 0;
    case WM_TIMER:
        if (wp==settings_timer) { KillTimer(window,settings_timer); apply_client(); return 0; }
        break;
    case WM_CLOSE:
        diagnostics::write(diagnostics::Level::info,"ui.close","settings window closed");
        KillTimer(window,settings_timer); apply_client(); DestroyWindow(window); return 0;
    case WM_DESTROY:
        statistics_.reset(); tabs_=nullptr; client_=nullptr; controls_.clear();
        release_unused_icon_cache();
        chart_settings_label_=nullptr;chart_checks_.clear();
        cpu_label_=nullptr;cpu_temperature_label_=nullptr;cpu_checks_.clear();cpu_topology_ids_.clear();
        disk_checks_.fill(nullptr);disk_total_check_=nullptr;shortcuts_label_=nullptr;
        if (client_font_) { DeleteObject(client_font_); client_font_=nullptr; } activity(); return 0;
    }
    return DefWindowProcW(window,msg,wp,lp);
}
LRESULT Overlay::message(UINT msg,WPARAM wp,LPARAM lp) {
    if (taskbar_created_ && msg==taskbar_created_) { add_tray(); return 0; }
    switch (msg) {
    case WM_ERASEBKGND: return 1;
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_TIMER:
        if(wp==overlay_animation_timer) {
            const auto now=GetTickCount64();
            const auto elapsed=now-row_animation_started_;
            const bool finished=!row_animation_active_ || elapsed>=row_animation_duration_ms_;
            // Keep handling messages if the previous software frame was expensive.
            if(!finished && now<row_animation_next_frame_tick_) return 0;
            const float progress=finished?1.0f:overlay_smoothstep(
                static_cast<float>(elapsed)/static_cast<float>(row_animation_duration_ms_));
            // Don't render sub-pixel movements. Ease-in/out otherwise wastes frames
            // while the rows have not visibly moved since their last paint.
            float largest_pixel_shift=0.0f;
            if(!finished) {
                RECT viewport{};GetClientRect(hwnd_,&viewport);
                const auto first_visible=std::min(scroll_,row_motion_.size());
                const auto last_visible=std::min(row_motion_.size(),
                    first_visible+visible_rows(dip(std::max<LONG>(0,viewport.bottom))));
                for(std::size_t i=first_visible;i<last_visible;++i) {
                    const float travel=std::abs(static_cast<float>(i)-row_motion_[i].start_slot);
                    const float shift=travel*std::abs(progress-row_animation_last_painted_progress_)
                        *row_height()*static_cast<float>(dpi_)/96.0f;
                    largest_pixel_shift=std::max(largest_pixel_shift,shift);
                }
            }
            if(finished) {
                KillTimer(hwnd_,overlay_animation_timer);
                row_animation_active_=false;
                previous_rows_.clear();row_motion_.clear();
                clear_row_images(); // No animation textures or extra RAM in idle.
            }
            // WM_PAINT from fresh readings/settings takes precedence over a row-only
            // frame. Both execute on the UI thread, so no shared bitmap races.
            if(visible_ && (finished || largest_pixel_shift>=0.85f) &&
                !GetUpdateRect(hwnd_,nullptr,FALSE)) {
                const auto started=GetTickCount64();
                paint(true);
                const auto frame_cost=GetTickCount64()-started;
                // At most 25 FPS (40 ms between frame starts). Skip late frames
                // when software drawing/UpdateLayeredWindow exceeds the budget.
                row_animation_next_frame_tick_=started+
                    std::clamp<ULONGLONG>(frame_cost+8ULL,40ULL,160ULL);
                if(frame_cost>=45 && !row_animation_slow_frame_reported_) {
                    row_animation_slow_frame_reported_=true;
                    diagnostics::write(diagnostics::Level::warning,"overlay.animation_slow",
                        "render_ms="+std::to_string(frame_cost)+" rows="+std::to_string(rows_.size())+
                        " visible="+std::to_string(visible_rows(dip(bitmap_size_.cy))));
                }
            }
            return 0;
        }
        break;
    case WM_PAINT: { PAINTSTRUCT data{}; BeginPaint(hwnd_,&data); EndPaint(hwnd_,&data); paint(); return 0; }
    case WM_SIZE: if (visible_) InvalidateRect(hwnd_,nullptr,FALSE); return 0;
    case WM_DPICHANGED: {
        dpi_=HIWORD(wp); load_icons(); release_surface(); const auto* r=reinterpret_cast<RECT*>(lp);
        SetWindowPos(hwnd_,nullptr,r->left,r->top,r->right-r->left,r->bottom-r->top,SWP_NOZORDER|SWP_NOACTIVATE); fit_table(); return 0;
    }
    case WM_DISPLAYCHANGE: fit_table(); return 0;
    case WM_NCHITTEST: return manual_movement_?HTCAPTION:HTTRANSPARENT;
    case WM_ENTERSIZEMOVE: dragging_=manual_movement_; return 0;
    case WM_EXITSIZEMOVE:
        if(std::exchange(dragging_,false)) {
            RECT bounds{};
            if(GetWindowRect(hwnd_,&bounds)) {
                if(auto position=overlay_position_from_rect(bounds,
                    MonitorFromRect(&bounds,MONITOR_DEFAULTTONEAREST),pixels(8))) {
                    scene_position_=std::move(position);
                    right_anchor_=bounds.right;
                    fit_table();
                    save_settings();
                    if(scene_dialog_)SendMessageW(scene_dialog_,scene_position_changed,0,
                        reinterpret_cast<LPARAM>(&*scene_position_));
                }
            }
        }
        return 0;
    case WM_HOTKEY:
        diagnostics::write(diagnostics::Level::info,"ui.hotkey","hotkey="+std::to_string(wp));
        if(wp==toggle_hotkey){overlay_running_=!overlay_running_;update_overlay_visibility();}
        else if(wp==client_hotkey)show_client();
        return 0;
    case tray_message:
        if (lp==WM_LBUTTONDBLCLK || lp==NIN_KEYSELECT) show_client(); else if (lp==WM_RBUTTONUP || lp==WM_CONTEXTMENU) tray_menu(); return 0;
    case sample_message: receive(); return 0;
    case foreground_update_message:
        diagnostics::write(diagnostics::Level::trace,"ui.foreground","foreground window changed");
        ++foreground_epoch_;
        update_view(); // Update FPS target and header in every sorting mode.
        if(visible_)InvalidateRect(hwnd_,nullptr,FALSE);
        return 0;
    case icon_message:
        diagnostics::write(diagnostics::Level::trace,"ui.icons","icon notification received");
        icon_posted_=false;
        if(blacklist_dialog_)SendMessageW(blacklist_dialog_,blacklist_icons_ready,0,0);
        if(statistics_)statistics_->icons_ready();
        if (visible_) InvalidateRect(hwnd_,nullptr,FALSE);
        client_stats();return 0;
    case WM_CLOSE: DestroyWindow(hwnd_); return 0;
    case WM_DESTROY: {
        diagnostics::write(diagnostics::Level::info,"ui.stop","overlay shutdown");
        if (foreground_hook_) { UnhookWinEvent(foreground_hook_); foreground_hook_ = nullptr; }
        foreground_update_target = nullptr;
        KillTimer(hwnd_,overlay_animation_timer);
        row_animation_active_=false;
        previous_rows_.clear();row_motion_.clear();
        clear_row_images();
        stopping_=true; save_settings(); worker_.stop(); icons_.reset();
        if (client_) DestroyWindow(client_);
        NOTIFYICONDATAW data{}; data.cbSize=sizeof(data); data.hWnd=hwnd_; data.uID=1;
        if (tray_) Shell_NotifyIconW(NIM_DELETE,&data);
        if (toggle_key_) UnregisterHotKey(hwnd_,toggle_hotkey); if (client_key_) UnregisterHotKey(hwnd_,client_hotkey);
        tray_=false; toggle_key_=false; client_key_=false; PostQuitMessage(exit_code_); return 0;
    }
    }
    return DefWindowProcW(hwnd_,msg,wp,lp);
}
} // namespace

int run_overlay(const Options& options) {
    // Finite diagnostic runs must really render, even alongside the user's monitor.
    Handle instance(CreateMutexW(nullptr, FALSE, options.samples ? nullptr : L"Local\\FluxTop.Monitor.v1"));
    if (!instance.valid()) throw windows_error("Create FluxTop instance mutex");
    if (GetLastError()==ERROR_ALREADY_EXISTS) return 0;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const auto com=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    struct Scope { bool active; ~Scope() { if (active) CoUninitialize(); } } scope{SUCCEEDED(com)};
    INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_STANDARD_CLASSES|ICC_BAR_CLASSES|ICC_TAB_CLASSES|ICC_LISTVIEW_CLASSES|ICC_DATE_CLASSES}; InitCommonControlsEx(&controls);
    if (!options.samples) migrate_autostart_if_needed();
    FreeConsole();
    try { Overlay app(options); return app.run(); }
    catch (const std::exception& error) {
        diagnostics::write(diagnostics::Level::error,"ui.fatal",error.what());
        if (!options.samples) MessageBoxW(nullptr,wide(error.what()).c_str(),L"FluxTop — ошибка запуска",MB_OK|MB_ICONERROR);
        return 1;
    }
}
} // namespace fluxtop
