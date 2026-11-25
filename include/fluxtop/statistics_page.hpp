#pragma once
#include "fluxtop/model.hpp"
#include "fluxtop/statistics_history.hpp"
#include "fluxtop/statistics_chart_controls.hpp"
#include "fluxtop/cpu_view.hpp"
#include "fluxtop/system_graph_history.hpp"
#include <windows.h>
#include <commctrl.h>
#include <array>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <thread>

namespace fluxtop {
// Independent Win32 page. History queries never execute on the UI or sampler thread.
class StatisticsPage {
public:
    StatisticsPage(HWND parent, std::filesystem::path database,
        std::wstring settings_path, unsigned limit,std::shared_ptr<SystemGraphHistory> graph_history);
    ~StatisticsPage();
    StatisticsPage(const StatisticsPage&)=delete;
    StatisticsPage& operator=(const StatisticsPage&)=delete;
    HWND window() const { return hwnd_; }
    void resize(int x,int y,int width,int height);
    void activate(bool active);
    void set_limit(unsigned limit);
    void set_cpu_view(const CpuViewSettings& settings);
    void on_snapshot(std::int64_t timestamp_ms,const std::vector<Process>& grouped,unsigned interval_ms,const Snapshot& system);
private:
    using SystemPoint=SystemChartPoint;
    static LRESULT CALLBACK dispatch(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp);
    static LRESULT CALLBACK hover_dispatch(HWND window,UINT msg,WPARAM wp,LPARAM lp);
    static LRESULT CALLBACK table_dispatch(HWND window,UINT msg,WPARAM wp,LPARAM lp);
    LRESULT message(UINT msg,WPARAM wp,LPARAM lp);
    void make_controls();
    void set_history_limit(unsigned limit);
    void set_uptime_limit(unsigned limit);
    void layout();
    void request(bool uptime_only=false);
    void accept();
    void populate_table();
    void open_table();
    void layout_table_window();
    void update_dates();
    void draw(HDC dc);
    void draw_sidebar(HDC dc);
    // All chart variants share this drawing pipeline. Callers supply geometry,
    // prepared pixel-series and visual modifiers, never GDI drawing loops.
    struct ChartStroke {
        std::vector<POINT> points;
        COLORREF color{};
        bool dotted{};
    };
    struct ChartBar {
        RECT area{};
        COLORREF color{};
    };
    struct ChartStyle {
        RECT area{};
        std::int64_t from{},to{};
        int horizontal_rows{};
        int time_divisions{};
        COLORREF paper{RGB(250,250,250)};
        COLORREF grid{RGB(211,217,220)};
        BYTE fill_opacity{31};
        bool adaptive_fill{};
        bool ink_detail{};
        bool point_markers{true};
        int right_edge_overscan{}; // Live data enters offscreen on the right.
    };
    void draw_chart_core(HDC dc,const ChartStyle& style,
        const std::vector<ChartStroke>& plots,const std::vector<ChartBar>& bars={});
    static ChartStyle mini_chart_style(const RECT& area,bool live);
    HFONT chart_font();
    void set_chart_fill(bool enabled);
    void fill_under(HDC dc,const std::vector<POINT>& points,LONG baseline,COLORREF color,BYTE opacity=31);
    void draw_system_details(HDC dc,unsigned metric);
    void select_disk(wchar_t letter);
    void select_core(std::size_t index);
    std::vector<CpuChartSeries> cpu_metric_series(unsigned metric,const CpuChartSample& sample)const;
    void draw_system_charts(HDC dc,const RECT& area,unsigned kind,std::int64_t from,std::int64_t to,
        bool autoscale,double zoom,const std::deque<SystemPoint>* frozen=nullptr,bool detached=false);
    struct FrozenView;
    void hover_system_charts(unsigned kind,POINT cursor,const RECT& area,std::int64_t from,std::int64_t to,
        const FrozenView& held);
    RECT selected_system_chart_area(unsigned kind) const;
    void show_system_hover(unsigned metric,POINT cursor,const RECT& area,std::int64_t from,std::int64_t to,
        const FrozenView& held);
    void draw_system_hover(HDC dc,const RECT& bounds);
    bool advance_time_view(double elapsed_ms);
    std::int64_t system_view_end() const;
    std::int64_t live_view_end() const;
    void draw_system_graph(HDC dc,const RECT& area,unsigned metric,std::int64_t from,
        std::int64_t to,bool autoscale,double manual_zoom,
        const std::deque<SystemPoint>* frozen=nullptr,bool detached=false);
    void select_section(unsigned section);
    unsigned selected_chart_kind() const;
    bool has_popup() const;
    void refresh_auto_axis(unsigned index,bool immediate);
    void rebuild_live_series();
    void draw_graph(HDC dc,const RECT& area,const std::vector<StatisticsApplication>& apps,
        std::int64_t from,std::int64_t to,const wchar_t* caption,bool is_live,
        double vertical_zoom=1.0,bool detached=false);
    // Cached double-buffer: owns its DC and selected bitmap, resizes on demand.
    struct PaintBuffer {
        HDC dc{};
        HBITMAP bitmap{};
        HGDIOBJ old_bitmap{};
        LONG width{},height{};
        PaintBuffer()=default;
        PaintBuffer(const PaintBuffer&)=delete;
        PaintBuffer& operator=(const PaintBuffer&)=delete;
        ~PaintBuffer();
        void clear() noexcept;
        HDC prepare(HDC reference,LONG width,LONG height);
    };
    struct SystemInfo {
        std::optional<std::uint64_t> ram_used,ram_capacity,gpu_memory,gpu_capacity;
        std::optional<double> rx,tx,cpu_temp,gpu_temp;
        unsigned logical_processors{};
        double collection_ms{};
        bool cpu_board{};
        std::string cpu_source{"unavailable"};
        bool operator==(const SystemInfo&) const = default;
    };
    struct FrozenView {
        bool paused{};
        std::deque<SystemPoint> samples;
        std::vector<StatisticsApplication> apps;
        ChartTimeWindow range;
        double zoom{1.0};
        SystemInfo info;
    };
    ChartTimeWindow chart_range(unsigned kind,const FrozenView& held) const;
    const std::vector<StatisticsApplication>& chart_apps(unsigned kind,const FrozenView& held) const;
    void toggle_chart_pause(unsigned kind,FrozenView& held,HWND control);
    struct PopupState;
    static LRESULT CALLBACK popup_dispatch(HWND window,UINT message,WPARAM wp,LPARAM lp);
    void open_popup(unsigned kind);
    ChartTimeWindow popup_range(const PopupState& popup) const;
    void set_popup_autoscale(PopupState& popup,bool enabled);
    void paint_popup(PopupState& popup,HDC dc);
    COLORREF series_color(const StatisticsApplication& app,std::size_t index) const;
    void mouse(int x,int y);
    // Position the native hover window without re-showing it on every mouse move.
    // Returns true only when the window was previously hidden.
    bool position_tooltip(POINT screen,int offset_x,int offset_y);
    void paint_tooltip(HDC dc);
    void draw_metric_tooltip(HDC dc,const RECT& area,const StatisticsApplication& app,double score,std::int64_t sample_at);
    bool advance_axes(double elapsed_ms);
    HICON application_icon(const std::wstring& path);
    double icon_hue(const std::wstring& key,const std::wstring& path);
    static std::wstring color_key(const StatisticsApplication& app);
    void prepare_colors(const std::vector<StatisticsApplication>& minute);
    COLORREF chart_color(const StatisticsApplication& app) const;
    HPEN graph_pen(COLORREF color,bool dotted);
    std::int64_t custom_timestamp(HWND picker,bool until_end);
    struct PopupState {
        StatisticsPage* owner{};
        unsigned kind{};
        HWND window{};
        HWND auto_toggle{};
        HWND fill_toggle{};
        HWND pause_toggle{};
        FrozenView held;
        bool auto_scale{true};
        double cached_auto_zoom{1.0};
        double target_auto_zoom{1.0};
        bool axis_initialized{false};
        bool axis_dirty{true};
        double zoom_x{1.0},zoom_y{1.0},pan_ms{};
        bool dragging{};
        PaintBuffer paint_buffer;
        POINT last{},cursor{-1,-1};
    };
    PaintBuffer page_buffer_;
    PaintBuffer tooltip_buffer_; // Reusable native tooltip double buffer.
    PaintBuffer fill_color_buffer_; // Reused 1x1 bitmap for GDI alpha blending.
    std::deque<PopupState> popups_{32}; // Append cores; deque keeps native callback pointers stable.
    // FluxTop.SystemDashboard.Stage1: bounded, sampler-fed system history.
    std::shared_ptr<SystemGraphHistory> graph_history_;
    const std::deque<SystemPoint>& system_points_;
    CpuViewSettings cpu_view_;
    ULONGLONG hover_last_frame_{};
    unsigned selected_section_{0}; // CPU, RAM, network, GPU, AutoScore, history, uptime
    wchar_t selected_disk_letter_{};
    std::optional<std::size_t> selected_core_;
    std::vector<IntelCoreTemperature> core_entries_;
    std::vector<std::size_t> core_indices_;
    std::vector<wchar_t> disk_letters_;
    unsigned sidebar_scroll_{};
    unsigned system_range_{0}; // 1, 5, 15, 30 minutes.
    bool system_autoscale_{true};
    // Independent axis easing for embedded and detached system charts (five metrics).
    std::array<std::vector<double>,2> smooth_system_axis_max_{{std::vector<double>(32),std::vector<double>(32)}};
    std::array<std::vector<ULONGLONG>,2> smooth_system_axis_tick_{{std::vector<ULONGLONG>(32),std::vector<ULONGLONG>(32)}};
    HWND system_period_{},system_auto_{},system_popout_{},fill_toggle_{},pause_toggle_{};
    void toggle_section_pause();
    void refresh_pause_control();
    bool fill_enabled_{true}; // Actual alpha blending; shared by embedded and detached graphs.
    double smooth_system_end_{},smooth_live_end_{};
    SystemInfo system_info_;
    std::deque<FrozenView> frozen_views_{32};
    bool system_tip_active_{};
    unsigned system_tip_metric_{};
    SystemPoint system_tip_sample_{};
    SystemInfo system_tip_info_;
    HWND popout_live_{},popout_history_{},popout_table_{};
    HWND auto_live_{},auto_history_{};
    std::array<bool,2> auto_scale_{{true,true}};
    std::array<double,2> cached_auto_zoom_{{1.0,1.0}};
    std::array<double,2> target_auto_zoom_{{1.0,1.0}};
    std::array<bool,2> axis_initialized_{{false,false}};
    bool history_axis_refit_pending_{}; // Commit new period scale only after DB results.
    ULONGLONG axis_tick_{};
    HWND tooltip_{};
    HWND table_window_{},table_window_list_{},table_window_info_{};
    HIMAGELIST table_image_list_{};
    HFONT table_font_{};
    ULONGLONG animation_started_{};
    unsigned animation_duration_ms_{1000};
    std::int64_t last_live_timestamp_{};
    struct LiveEvent { std::int64_t time{}; std::wstring name,path,identity; double score{}; };
    struct Query {
        std::int64_t from{},to{},uptime_from{},uptime_to{};
        unsigned limit{},history_limit{},uptime_limit{};
        bool system{},uptime_only{}; unsigned serial{};
    };
    HWND hwnd_{},period_{},from_{},to_{},uptime_period_{},uptime_from_{},uptime_to_{},
        show_system_{},first_label_{},table_{},limit_label_{},limit_slider_{};
    HWND history_limit_label_{},history_limit_slider_{},uptime_limit_label_{},uptime_limit_slider_{};
    HIMAGELIST image_list_{};
    HFONT font_{};
    UINT controls_font_dpi_{};
    // Fonts and pens are owned by the page and reused during 33-ms animation.
    HFONT paint_font_{};
    std::array<HFONT,3> tooltip_fonts_{};
    std::array<int,3> tooltip_font_heights_{};
    std::map<COLORREF,HPEN> stroke_pens_,detail_pens_;
    std::filesystem::path database_;
    std::wstring settings_;
    unsigned limit_{},history_limit_{},uptime_limit_{};
    bool active_{},system_{};
    int period_index_{},uptime_period_index_{};
    std::int64_t first_record_{};
    std::int64_t last_history_refresh_{};
    std::int64_t window_from_{},window_to_{};
    StatisticsHistory history_;
    struct CurrentReadings {
        std::wstring cpu{L"—"},ram{L"—"},gpu{L"—"},vram{L"—"},network{L"—"};
    };
    std::map<std::wstring,CurrentReadings> current_readings_;
    std::vector<StatisticsApplication> live_apps_;
    std::deque<LiveEvent> live_;
    std::map<std::wstring,HICON> icon_cache_;
    std::map<std::wstring,double> icon_hues_;
    std::map<std::wstring,COLORREF> chart_colors_;
    int hover_section_=-1;
    std::size_t hover_index_{};
    POINT hover_at_{};
    double hover_value_{};
    std::int64_t hover_timestamp_{};
    std::mutex mutex_;
    std::condition_variable_any wake_;
    std::stop_source cancel_query_;
    unsigned generation_{},processed_{};
    Query requested_{};
    std::optional<std::pair<unsigned,StatisticsHistory>> completed_;
    std::jthread worker_;
};
}
