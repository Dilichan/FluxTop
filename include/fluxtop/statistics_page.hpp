#pragma once
#include "fluxtop/model.hpp"
#include "fluxtop/statistics_history.hpp"
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
        std::wstring settings_path, unsigned limit);
    ~StatisticsPage();
    StatisticsPage(const StatisticsPage&)=delete;
    StatisticsPage& operator=(const StatisticsPage&)=delete;
    HWND window() const { return hwnd_; }
    void resize(int x,int y,int width,int height);
    void activate(bool active);
    void set_limit(unsigned limit);
    void on_snapshot(std::int64_t timestamp_ms,const std::vector<Process>& grouped,unsigned interval_ms);
private:
    static LRESULT CALLBACK dispatch(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp);
    static LRESULT CALLBACK hover_dispatch(HWND window,UINT msg,WPARAM wp,LPARAM lp);
    static LRESULT CALLBACK table_dispatch(HWND window,UINT msg,WPARAM wp,LPARAM lp);
    LRESULT message(UINT msg,WPARAM wp,LPARAM lp);
    void make_controls();
    void set_history_limit(unsigned limit);
    void set_uptime_limit(unsigned limit);
    void layout();
    void request();
    void accept();
    void populate_table();
    void open_table();
    void layout_table_window();
    void update_dates();
    void draw(HDC dc);
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
    struct PopupState;
    static LRESULT CALLBACK popup_dispatch(HWND window,UINT message,WPARAM wp,LPARAM lp);
    void open_popup(unsigned kind);
    void popup_range(const PopupState& popup,std::int64_t& from,std::int64_t& to) const;
    void paint_popup(PopupState& popup,HDC dc);
    COLORREF series_color(const StatisticsApplication& app,std::size_t index) const;
    void mouse(int x,int y);
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
    std::array<PopupState,2> popups_{};
    HWND popout_live_{},popout_history_{},popout_table_{};
    HWND auto_live_{},auto_history_{};
    std::array<bool,2> auto_scale_{{true,true}};
    std::array<double,2> cached_auto_zoom_{{1.0,1.0}};
    std::array<double,2> target_auto_zoom_{{1.0,1.0}};
    std::array<bool,2> axis_initialized_{{false,false}};
    ULONGLONG axis_tick_{};
    HWND tooltip_{};
    HWND table_window_{},table_window_list_{},table_window_info_{};
    HIMAGELIST table_image_list_{};
    HFONT table_font_{};
    ULONGLONG animation_started_{};
    unsigned animation_duration_ms_{1000};
    std::int64_t last_live_timestamp_{};
    struct LiveEvent { std::int64_t time{}; std::wstring name,path; double score{}; };
    struct Query { std::int64_t from{},to{}; unsigned limit{},history_limit{},uptime_limit{}; bool system{}; unsigned serial{}; };
    HWND hwnd_{},period_{},from_{},to_{},show_system_{},first_label_{},table_{},limit_label_{},limit_slider_{};
    HWND history_limit_label_{},history_limit_slider_{},uptime_limit_label_{},uptime_limit_slider_{};
    HIMAGELIST image_list_{};
    HFONT font_{};
    UINT controls_font_dpi_{};
    // Fonts and pens are owned by the page and reused during 33-ms animation.
    HFONT paint_font_{},popup_font_{};
    std::array<HFONT,3> tooltip_fonts_{};
    std::array<int,3> tooltip_font_heights_{};
    std::map<COLORREF,HPEN> stroke_pens_,detail_pens_;
    std::filesystem::path database_;
    std::wstring settings_;
    unsigned limit_{},history_limit_{},uptime_limit_{};
    bool active_{},system_{};
    int period_index_{};
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