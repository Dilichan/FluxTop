#pragma once
#include "fluxtop/model.hpp"
#include "fluxtop/statistics_history.hpp"
#include <windows.h>
#include <commctrl.h>
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
    void on_snapshot(std::int64_t timestamp_ms,const std::vector<Process>& grouped);
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
    void draw_graph(HDC dc,const RECT& area,const std::vector<StatisticsApplication>& apps,
        std::int64_t from,std::int64_t to,const wchar_t* caption,bool is_live);
    void mouse(int x,int y);
    void paint_tooltip(HDC dc);
    HICON application_icon(const std::wstring& path);
    std::int64_t custom_timestamp(HWND picker,bool until_end);
    struct LiveEvent { std::int64_t time{}; std::wstring name,path; double score{}; };
    struct Query { std::int64_t from{},to{}; unsigned limit{}; bool system{}; unsigned serial{}; };
    HWND hwnd_{},period_{},from_{},to_{},show_system_{},first_label_{},table_{};
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