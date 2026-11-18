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
    LRESULT message(UINT msg,WPARAM wp,LPARAM lp);
    void make_controls();
    void layout();
    void request();
    void accept();
    void populate_table();
    void update_dates();
    void draw(HDC dc);
    void rebuild_live_series();
    void draw_graph(HDC dc,const RECT& area,const std::vector<StatisticsApplication>& apps,
        std::int64_t from,std::int64_t to,const wchar_t* caption,bool is_live,
        double vertical_zoom=1.0,bool detached=false);
    struct PopupState;
    static LRESULT CALLBACK popup_dispatch(HWND window,UINT message,WPARAM wp,LPARAM lp);
    void open_popup(unsigned kind);
    void popup_range(const PopupState& popup,std::int64_t& from,std::int64_t& to) const;
    void paint_popup(PopupState& popup,HDC dc);
    COLORREF series_color(const StatisticsApplication& app,std::size_t index) const;
    void mouse(int x,int y);
    void paint_tooltip(HDC dc);
    HICON application_icon(const std::wstring& path);
    double icon_hue(const std::wstring& key,const std::wstring& path);
    static std::wstring color_key(const StatisticsApplication& app);
    void prepare_colors(const std::vector<StatisticsApplication>& minute);
    COLORREF chart_color(const StatisticsApplication& app) const;
    std::int64_t custom_timestamp(HWND picker,bool until_end);
    struct PopupState {
        StatisticsPage* owner{};
        unsigned kind{};
        HWND window{};
        double zoom_x{1.0},zoom_y{1.0},pan_ms{};
        bool dragging{};
        POINT last{},cursor{-1,-1};
    };
    std::array<PopupState,2> popups_{};
    HWND popout_live_{},popout_history_{};
    ULONGLONG animation_started_{};
    unsigned animation_duration_ms_{1000};
    std::int64_t last_live_timestamp_{};
    struct LiveEvent { std::int64_t time{}; std::wstring name,path; double score{}; };
    struct Query { std::int64_t from{},to{}; unsigned limit{}; bool system{}; unsigned serial{}; };
    HWND hwnd_{},period_{},from_{},to_{},show_system_{},first_label_{},table_{},limit_label_{},limit_slider_{};
    HIMAGELIST image_list_{};
    HFONT font_{};
    std::filesystem::path database_;
    std::wstring settings_;
    unsigned limit_{};
    bool active_{},system_{};
    int period_index_{};
    std::int64_t first_record_{};
    std::int64_t last_history_refresh_{};
    std::int64_t window_from_{},window_to_{};
    StatisticsHistory history_;
    std::vector<StatisticsApplication> live_apps_;
    std::deque<LiveEvent> live_;
    std::map<std::wstring,HICON> icon_cache_;
    std::map<std::wstring,double> icon_hues_;
    std::map<std::wstring,COLORREF> chart_colors_;
    int hover_section_=-1;
    std::size_t hover_index_{};
    POINT hover_at_{};
    double hover_value_{};
    std::mutex mutex_;
    std::condition_variable_any wake_;
    std::stop_source cancel_query_;
    unsigned generation_{},processed_{};
    Query requested_{};
    std::optional<std::pair<unsigned,StatisticsHistory>> completed_;
    std::jthread worker_;
};
}