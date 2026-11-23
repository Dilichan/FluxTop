#include "fluxtop/diagnostics.hpp"
#include "fluxtop/statistics_page.hpp"
#include "fluxtop/history_identity.hpp"
#include "fluxtop/statistics_axis.hpp"
#include "fluxtop/statistics_chart_controls.hpp"
#include "fluxtop/statistics_sidebar_order.hpp"
#include "fluxtop/windows.hpp"
#include "fluxtop/metric_text.hpp"
#include "fluxtop/overlay_ini.hpp"
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <windowsx.h>
#include <algorithm>
#include <array>
#include <iterator>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <limits>
#include <cstdint>
#include <set>
#include <unordered_map>
#include <numeric>

namespace fluxtop {
namespace {
constexpr wchar_t page_class[]=L"FluxTop.Statistics.Page";
constexpr wchar_t hover_class[]=L"FluxTop.Statistics.HoverTooltip.v1";
constexpr wchar_t table_class[]=L"FluxTop.Statistics.DetachedTable.v1";
constexpr UINT ready_message=WM_APP+201;
constexpr UINT_PTR animate_message=2201;
enum : int { period_id=1001,from_id,to_id,system_id,table_id,stats_limit_id,popout_live_id,popout_history_id,popout_table_id,history_limit_id,uptime_limit_id,uptime_period_id,uptime_from_id,uptime_to_id };
// Logical page dimensions; chart labels and Win32 controls share the same grid.
constexpr int page_width=1400, page_height=860;
// One shared right-edge guard for live charts; historical charts remain unchanged.
constexpr int live_tail_guard_px=18;
constexpr int spark_tail_guard_px=8;
constexpr int auto_live_id=1201, auto_history_id=1202, popup_auto_id=1203, system_period_id=1210, system_auto_id=1211, system_popout_id=1212;
constexpr RECT one_minute{310,171,1364,665};
constexpr RECT history_chart{310,171,1364,665};
constexpr std::array<const wchar_t*,6> popup_auto_scale_keys{{
    L"stats_popup_auto_live",L"stats_popup_auto_history",L"stats_popup_auto_cpu",
    L"stats_popup_auto_ram",L"stats_popup_auto_network",L"stats_popup_auto_gpu"}};
// Vertical grid markers are tied to UTC timestamps, not fixed chart columns.
// Advancing the live time window moves the grid and its series by the same pixels.
void draw_time_grid(HDC dc,const RECT& area,std::int64_t from,std::int64_t to,int columns) {
    if(to<=from || columns<=0 || area.right<=area.left)return;
    const auto wanted=std::max<std::int64_t>(1000,(to-from)/columns);
    std::int64_t decade=1000;
    while(decade<wanted/10 && decade<1000000000000LL)decade*=10;
    const auto step=wanted<=decade?decade:
        wanted<=2*decade?2*decade:wanted<=5*decade?5*decade:10*decade;
    // Unix millisecond timestamps are positive for the Windows versions supported here.
    for(auto at=(from/step+1)*step;at<to;at+=step) {
        const auto relative=static_cast<double>(at-from)/static_cast<double>(to-from);
        const auto x=area.left+static_cast<LONG>(std::llround(relative*(area.right-area.left)));
        MoveToEx(dc,x,area.top,nullptr);LineTo(dc,x,area.bottom);
    }
}
// Extend only fresh, observed values to the live viewport boundary. The extension
// is a drawing-only hold; it does not create or persist artificial samples.
void extend_recent_line(std::vector<POINT>& line,LONG right,
    std::int64_t last_sample,std::int64_t view_end,unsigned interval_ms) {
    if(line.empty() || line.back().x>=right || view_end<last_sample)return;
    const auto grace=2*static_cast<std::int64_t>(std::clamp(interval_ms,250U,10000U));
    if(view_end-last_sample>grace)return; // Do not hide stalled sampling.
    line.push_back(POINT{right,line.back().y});
}
// A rolling live window may start between two samples. Cover only a short
// sampling-sized gap at its left edge; never extend stale or historical data.
// This is presentation only: no synthetic sample is stored or exported.
void extend_fresh_left_edge(std::vector<POINT>& line,const RECT& area,
    std::int64_t from,std::int64_t to,unsigned interval_ms) {
    if(line.empty() || to<=from || area.right<=area.left)return;
    const LONG x=line.front().x;
    if(x<=area.left || x>=area.right)return;
    const double gap_ms=static_cast<double>(x-area.left)*static_cast<double>(to-from)/
        static_cast<double>(area.right-area.left);
    const double grace=2.0*static_cast<double>(std::clamp(interval_ms,250U,10000U));
    if(gap_ms<=grace) {
        const LONG y=line.front().y;
        line.insert(line.begin(),POINT{area.left,y});
    }
}
// Ease only the newly sampled endpoint; historical points keep their real values.
double eased_latest_value(double previous,double measured,ULONGLONG animation_start,
    unsigned animation_ms) {
    if(!animation_start)return measured;
    const double fraction=std::clamp(static_cast<double>(GetTickCount64()-animation_start)/
        std::max(1.0,static_cast<double>(animation_ms)),0.0,1.0);
    const double smooth=fraction*fraction*(3.0-2.0*fraction);
    return previous+(measured-previous)*smooth;
}
std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}
SYSTEMTIME local_time(std::int64_t epoch_ms) {
    // Unix epoch -> FILETIME (100ns since 1601), then UTC -> local time.
    const auto ticks=static_cast<std::uint64_t>(std::max<std::int64_t>(0,epoch_ms))*10000ULL+116444736000000000ULL;
    FILETIME file{static_cast<DWORD>(ticks),static_cast<DWORD>(ticks>>32)};
    SYSTEMTIME utc{},local{};
    FileTimeToSystemTime(&file,&utc);
    if(!SystemTimeToTzSpecificLocalTime(nullptr,&utc,&local)) return utc;
    return local;
}
std::wstring local_label(std::int64_t epoch_ms) {
    if(!epoch_ms) return L"—";
    auto st=local_time(epoch_ms);
    wchar_t output[48]{};
    swprintf_s(output,L"%02u.%02u.%04u %02u:%02u",st.wDay,st.wMonth,st.wYear,st.wHour,st.wMinute);
    return output;
}
std::wstring local_sample_label(std::int64_t epoch_ms) {
    if(epoch_ms<=0) return L"—";
    const auto time=local_time(epoch_ms);
    wchar_t output[64]{};
    swprintf_s(output,L"%02u.%02u.%04u %02u:%02u:%02u",
        time.wDay,time.wMonth,time.wYear,time.wHour,time.wMinute,time.wSecond);
    return output;
}
std::wstring span_label(double seconds) {
    const auto secs=static_cast<std::uint64_t>(std::max(0.0,seconds)+.5);
    const auto days=secs/86400,hours=(secs/3600)%24,minutes=(secs/60)%60;
    if(days) return std::to_wstring(days)+L" д "+std::to_wstring(hours)+L" ч";
    if(hours) return std::to_wstring(hours)+L" ч "+std::to_wstring(minutes)+L" мин";
    return std::to_wstring(minutes)+L" мин "+std::to_wstring(secs%60)+L" с";
}
// A readable ink tone derived from hue (white-page contrast is maintained).
COLORREF colored_ink(double hue) {
    hue=std::fmod(hue+360.0,360.0);
    const double chroma=.69*.68, intermediate=chroma*(1-std::abs(std::fmod(hue/60.0,2.0)-1));
    double r=0,g=0,b=0;
    if(hue<60) {r=chroma;g=intermediate;}
    else if(hue<120) {r=intermediate;g=chroma;}
    else if(hue<180) {g=chroma;b=intermediate;}
    else if(hue<240) {g=intermediate;b=chroma;}
    else if(hue<300) {r=intermediate;b=chroma;}
    else {r=chroma;b=intermediate;}
    constexpr double base=.68-.69*.68;
    return RGB(static_cast<int>((r+base)*255),static_cast<int>((g+base)*255),static_cast<int>((b+base)*255));
}
// Hue on [0,360). Low saturation is ignored when inspecting an icon.
double pixel_hue(int r,int g,int b) {
    const int hi=std::max({r,g,b}),lo=std::min({r,g,b});
    const int delta=hi-lo;
    if(!delta) return 0;
    double hue=hi==r?60.0*(g-b)/delta:hi==g?60.0*(b-r)/delta+120:60.0*(r-g)/delta+240;
    return hue<0?hue+360:hue;
}
// The Y-axis must include clipped/interpolated segments, not only points.
double auto_vertical_zoom(const std::vector<StatisticsApplication>& apps,
    std::int64_t from,std::int64_t to) {
    return statistics_auto_vertical_zoom(apps,from,to);
}
// Compatibility names for existing statistics call sites; formatting lives in
// metric_text.hpp and UTF-8 conversion in windows.hpp.
std::wstring stat_decimal(double value) {return metric_text::decimal(value);}
std::wstring stat_percent(std::optional<double> value) {return metric_text::percent(value);}
std::wstring stat_memory(std::optional<std::uint64_t> value) {return metric_text::memory(value);}
std::wstring stat_network(std::optional<double> rx,std::optional<double> tx) {
    return metric_text::network_rate(rx,tx);
}
std::wstring stat_wide(std::string_view text) {return utf8_to_wide(text);}
// Asymmetric easing: expand to fit new peaks quickly, contract slowly after they leave.
double eased_axis(double current,double target,double elapsed_ms) {
    if(!std::isfinite(target)) return current;
    const double duration=target<current?750.0:4200.0;
    const double fraction=1.0-std::exp(-std::clamp(elapsed_ms,0.0,250.0)/duration);
    const double next=current+(target-current)*fraction;
    return std::abs(next-target)<0.002?target:std::clamp(next,1.0,100.0);
}

void label(HDC dc,const wchar_t* text,int x,int y,COLORREF color=RGB(58,56,52)) {
    SetTextColor(dc,color); SetBkMode(dc,TRANSPARENT); TextOutW(dc,x,y,text,static_cast<int>(wcslen(text)));
}
} // namespace
StatisticsPage::PaintBuffer::~PaintBuffer() { clear(); }
void StatisticsPage::PaintBuffer::clear() noexcept {
    if(dc && old_bitmap) SelectObject(dc,old_bitmap);
    if(bitmap) DeleteObject(bitmap);
    if(dc) DeleteDC(dc);
    dc=nullptr;bitmap=nullptr;old_bitmap=nullptr;width=0;height=0;
}
HDC StatisticsPage::PaintBuffer::prepare(HDC reference,LONG w,LONG h) {
    if(w<=0 || h<=0) return nullptr;
    if(dc && bitmap && width==w && height==h) return dc;
    clear();
    dc=CreateCompatibleDC(reference);
    if(!dc) return nullptr;
    bitmap=CreateCompatibleBitmap(reference,w,h);
    if(!bitmap) {clear();return nullptr;}
    old_bitmap=SelectObject(dc,bitmap);
    if(!old_bitmap || old_bitmap==HGDI_ERROR) {old_bitmap=nullptr;clear();return nullptr;}
    width=w;height=h;
    return dc;
}
StatisticsPage::StatisticsPage(HWND parent,std::filesystem::path database,
    std::wstring settings_path,unsigned limit)
    :database_(std::move(database)),settings_(std::move(settings_path)) {
    // The live graph uses the SAME limit as overlay rows, across tabs.
    limit_=std::clamp(limit,1U,100U);
    history_limit_=std::clamp(ini_overlay_read(settings_,L"stats_history_top",limit_),1U,100U);
    uptime_limit_=std::clamp(ini_overlay_read(settings_,L"stats_uptime_top",limit_),1U,100U);
    system_=ini_overlay_read(settings_,L"stats_show_system",0)!=0;
    auto_scale_[0]=ini_overlay_read(settings_,L"stats_auto_live",1)!=0;
    auto_scale_[1]=ini_overlay_read(settings_,L"stats_auto_history",1)!=0;
    selected_section_=std::clamp(ini_overlay_read(settings_,L"stats_selected_section",0),0U,6U);
    const UINT saved_disk=ini_overlay_read(settings_,L"stats_selected_disk",0);
    selected_disk_letter_=saved_disk>=L'A' && saved_disk<=L'Z'?static_cast<wchar_t>(saved_disk):0;
    system_range_=std::clamp(ini_overlay_read(settings_,L"stats_system_range",0),0U,2U);
    system_autoscale_=ini_overlay_read(settings_,L"stats_system_auto",1)!=0;
    fill_enabled_=ini_overlay_read(settings_,L"stats_chart_fill",1)!=0;
    for(unsigned kind=0;kind<popups_.size();++kind) {
        popups_[kind].owner=this;popups_[kind].kind=kind;
    }
    WNDCLASSEXW cls{}; cls.cbSize=sizeof(cls);cls.hInstance=GetModuleHandleW(nullptr);
    cls.lpfnWndProc=dispatch;cls.lpszClassName=page_class;cls.hCursor=LoadCursorW(nullptr,IDC_ARROW);
    cls.hbrBackground=reinterpret_cast<HBRUSH>(COLOR_BTNFACE+1);
    register_window_class(cls,"Register Statistics page");
    hwnd_=CreateWindowExW(WS_EX_CONTROLPARENT,page_class,L"",WS_CHILD|WS_CLIPCHILDREN,
        0,0,page_width,page_height,parent,nullptr,GetModuleHandleW(nullptr),this);
    if(!hwnd_) throw windows_error("Create Statistics page");
    make_controls();
    WNDCLASSEXW hover{};
    hover.cbSize=sizeof(hover);
    hover.hInstance=GetModuleHandleW(nullptr);
    hover.lpfnWndProc=hover_dispatch;
    hover.lpszClassName=hover_class;
    hover.hCursor=LoadCursorW(nullptr,IDC_ARROW);
    register_window_class(hover,"Register statistics tooltip");
    tooltip_=CreateWindowExW(WS_EX_TOPMOST|WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE|WS_EX_TRANSPARENT,
        hover_class,L"",WS_POPUP,0,0,260,68,GetAncestor(hwnd_,GA_ROOT),nullptr,
        GetModuleHandleW(nullptr),this);
    if(!tooltip_) throw windows_error("Create statistics tooltip");
    worker_=std::jthread([this](std::stop_token stop){
        unsigned finished=0;
        while(!stop.stop_requested()) {
            Query q{};std::stop_token cancelled;
            {
                std::unique_lock lock(mutex_);
                wake_.wait(lock,stop,[this,&finished]{return generation_!=finished;});
                if(stop.stop_requested()) break;
                q=requested_;finished=generation_;cancelled=cancel_query_.get_token();
            }
            auto result=read_statistics(database_,q.from,q.to,q.history_limit,q.uptime_limit,
                q.limit,q.uptime_from,q.uptime_to,q.system,cancelled,q.uptime_only);
            if(stop.stop_requested()) break;
            {
                std::lock_guard lock(mutex_);
                if(finished!=generation_) continue; // A newer range was selected.
                completed_.emplace(finished,std::move(result));
            }
            PostMessageW(hwnd_,ready_message,0,0);
        }
    });
}
StatisticsPage::~StatisticsPage() {
    if(hwnd_) KillTimer(hwnd_,animate_message);
    if(table_window_ && IsWindow(table_window_)) DestroyWindow(table_window_);
    if(tooltip_ && IsWindow(tooltip_)) DestroyWindow(tooltip_);
    for(auto& popup:popups_)
        if(popup.window && IsWindow(popup.window)) DestroyWindow(popup.window);
    worker_.request_stop();{std::lock_guard lock(mutex_);cancel_query_.request_stop();}wake_.notify_all();
    if(worker_.joinable()) worker_.join();
    if(hwnd_ && IsWindow(hwnd_)) DestroyWindow(hwnd_);
    if(image_list_) ImageList_Destroy(image_list_);
    for(const auto& [path,icon]:icon_cache_) { (void)path; if(icon) DestroyIcon(icon); }
    page_buffer_.clear();
    for(auto& popup:popups_) popup.paint_buffer.clear();
    for(const auto& [color,pen]:stroke_pens_) { (void)color; DeleteObject(pen); }
    for(const auto& [color,pen]:detail_pens_) { (void)color; DeleteObject(pen); }
    for(const HFONT font:tooltip_fonts_) if(font) DeleteObject(font);
    if(paint_font_) DeleteObject(paint_font_);
    if(popup_font_) DeleteObject(popup_font_);
    if(font_) DeleteObject(font_);
}
void StatisticsPage::make_controls() {
    const auto mod=GetModuleHandleW(nullptr);
    auto add=[&](const wchar_t* class_name,const wchar_t* title,DWORD style,int id)->HWND{
        return CreateWindowExW(0,class_name,title,WS_CHILD|WS_VISIBLE|style,0,0,1,1,
            hwnd_,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),mod,nullptr);
    };
    period_=add(L"COMBOBOX",L"",CBS_DROPDOWNLIST|WS_TABSTOP|WS_VSCROLL,period_id);
    for(const auto* text:{L"За 24 часа",L"За 7 дней",L"Свой период"})
        SendMessageW(period_,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(text));
    SendMessageW(period_,CB_SETCURSEL,0,0);
    from_=add(DATETIMEPICK_CLASSW,L"",DTS_SHORTDATEFORMAT|WS_TABSTOP,from_id);
    to_=add(DATETIMEPICK_CLASSW,L"",DTS_SHORTDATEFORMAT|WS_TABSTOP,to_id);
    auto today=local_time(now_ms());
    SendMessageW(from_,DTM_SETSYSTEMTIME,GDT_VALID,reinterpret_cast<LPARAM>(&today));
    SendMessageW(to_,DTM_SETSYSTEMTIME,GDT_VALID,reinterpret_cast<LPARAM>(&today));
    // Uptime has an independent time range; the history calendar never filters the table.
    uptime_period_=add(L"COMBOBOX",L"",CBS_DROPDOWNLIST|WS_TABSTOP|WS_VSCROLL,uptime_period_id);
    for(const auto* text:{L"За 24 часа",L"За 7 дней",L"Свой период"})
        SendMessageW(uptime_period_,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(text));
    SendMessageW(uptime_period_,CB_SETCURSEL,0,0);
    uptime_from_=add(DATETIMEPICK_CLASSW,L"",DTS_SHORTDATEFORMAT|WS_TABSTOP,uptime_from_id);
    uptime_to_=add(DATETIMEPICK_CLASSW,L"",DTS_SHORTDATEFORMAT|WS_TABSTOP,uptime_to_id);
    SendMessageW(uptime_from_,DTM_SETSYSTEMTIME,GDT_VALID,reinterpret_cast<LPARAM>(&today));
    SendMessageW(uptime_to_,DTM_SETSYSTEMTIME,GDT_VALID,reinterpret_cast<LPARAM>(&today));
    show_system_=add(L"BUTTON",L"Показывать системные приложения",BS_AUTOCHECKBOX|WS_TABSTOP,system_id);
    SendMessageW(show_system_,BM_SETCHECK,system_?BST_CHECKED:BST_UNCHECKED,0);
    limit_label_=add(L"STATIC",L"Приложений в статистике",0,0);
    limit_slider_=add(TRACKBAR_CLASSW,L"",TBS_HORZ|TBS_NOTICKS|WS_TABSTOP,stats_limit_id);
    SendMessageW(limit_slider_,TBM_SETRANGE,TRUE,MAKELPARAM(1,100));
    SendMessageW(limit_slider_,TBM_SETPOS,TRUE,limit_);
    SetWindowTextW(limit_label_,(L"Количество приложений: "+std::to_wstring(limit_)+L" / 100").c_str());
    history_limit_label_=add(L"STATIC",L"История",0,0);
    history_limit_slider_=add(TRACKBAR_CLASSW,L"",TBS_HORZ|TBS_NOTICKS|WS_TABSTOP,history_limit_id);
    SendMessageW(history_limit_slider_,TBM_SETRANGE,TRUE,MAKELPARAM(1,100));
    SendMessageW(history_limit_slider_,TBM_SETPOS,TRUE,history_limit_);
    SetWindowTextW(history_limit_label_,(L"История: "+std::to_wstring(history_limit_)+L" / 100").c_str());
    uptime_limit_label_=add(L"STATIC",L"Аптайм",0,0);
    uptime_limit_slider_=add(TRACKBAR_CLASSW,L"",TBS_HORZ|TBS_NOTICKS|WS_TABSTOP,uptime_limit_id);
    SendMessageW(uptime_limit_slider_,TBM_SETRANGE,TRUE,MAKELPARAM(1,100));
    SendMessageW(uptime_limit_slider_,TBM_SETPOS,TRUE,uptime_limit_);
    SetWindowTextW(uptime_limit_label_,(L"Аптайм: "+std::to_wstring(uptime_limit_)+L" / 100").c_str());
    // Keep the database-bound calendar control, but hide internal storage notes.
    first_label_=add(L"STATIC",L"",0,0);
    ShowWindow(first_label_,SW_HIDE);
    table_=add(WC_LISTVIEWW,L"",LVS_REPORT|LVS_SINGLESEL|LVS_SHOWSELALWAYS|WS_TABSTOP,table_id);
    ListView_SetExtendedListViewStyle(table_,LVS_EX_FULLROWSELECT|LVS_EX_DOUBLEBUFFER);
    for(const auto& col:std::array<std::pair<const wchar_t*,int>,3>{{{L"Приложение",300},{L"Аптайм",140},{L"Последняя активность",255}}}) {
        LVCOLUMNW item{};item.mask=LVCF_WIDTH|LVCF_TEXT;item.cx=col.second;
        item.pszText=const_cast<LPWSTR>(col.first);
        ListView_InsertColumn(table_,Header_GetItemCount(ListView_GetHeader(table_)),&item);
    }
    image_list_=ImageList_Create(16,16,ILC_COLOR32|ILC_MASK,12,12);
    if(image_list_) ListView_SetImageList(table_,image_list_,LVSIL_SMALL);
    popout_live_=add(L"BUTTON",L"Открыть отдельно",WS_TABSTOP,popout_live_id);
    popout_history_=add(L"BUTTON",L"Открыть отдельно",BS_FLAT|WS_TABSTOP,popout_history_id);
    auto_live_=add(L"BUTTON",L"Автомасштаб",BS_AUTOCHECKBOX|WS_TABSTOP,auto_live_id);
    auto_history_=add(L"BUTTON",L"Автомасштаб",BS_AUTOCHECKBOX|WS_TABSTOP,auto_history_id);
    SendMessageW(auto_live_,BM_SETCHECK,auto_scale_[0]?BST_CHECKED:BST_UNCHECKED,0);
    SendMessageW(auto_history_,BM_SETCHECK,auto_scale_[1]?BST_CHECKED:BST_UNCHECKED,0);
    popout_table_=add(L"BUTTON",L"Открыть отдельно",WS_TABSTOP,popout_table_id);
    system_period_=add(L"COMBOBOX",L"",CBS_DROPDOWNLIST|WS_TABSTOP|WS_VSCROLL,system_period_id);
    for(const auto* caption:{L"1 минута",L"5 минут",L"15 минут"})
        SendMessageW(system_period_,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(caption));
    SendMessageW(system_period_,CB_SETCURSEL,system_range_,0);
    system_auto_=add(L"BUTTON",L"Автомасштаб",BS_AUTOCHECKBOX|WS_TABSTOP,system_auto_id);
    SendMessageW(system_auto_,BM_SETCHECK,system_autoscale_?BST_CHECKED:BST_UNCHECKED,0);
    system_popout_=add(L"BUTTON",L"Открыть отдельно",WS_TABSTOP,system_popout_id);
    fill_toggle_=add(L"BUTTON",L"Заливка графика",BS_AUTOCHECKBOX|WS_TABSTOP,system_popout_id+1);
    pause_toggle_=add(L"BUTTON",L"Пауза",WS_TABSTOP,system_popout_id+2);
    SendMessageW(fill_toggle_,BM_SETCHECK,fill_enabled_?BST_CHECKED:BST_UNCHECKED,0);
    layout();refresh_pause_control();update_dates();
}
void StatisticsPage::resize(int x,int y,int width,int height) {
    MoveWindow(hwnd_,x,y,width,height,TRUE);layout();
}
void StatisticsPage::layout() {
    if(!hwnd_ || !period_) return;
    RECT r{};GetClientRect(hwnd_,&r);
    const auto sx=[&](int x){return MulDiv(x,std::max<LONG>(1,r.right),page_width);};
    const auto sy=[&](int y){return MulDiv(y,std::max<LONG>(1,r.bottom),page_height);};
    const auto put=[&](HWND control,int x,int y,int w,int h){
        MoveWindow(control,sx(x),sy(y),sx(w),sy(h),TRUE);
    };
    const bool live=!selected_disk_letter_ && selected_section_==4,history=!selected_disk_letter_ && selected_section_==5,uptime=!selected_disk_letter_ && selected_section_==6;
    const bool system=selected_disk_letter_ || selected_section_<4;
    // Old per-graph options are preserved above the selected chart, never in its popup only.
    put(limit_label_,292,66,260,28);
    put(limit_slider_,546,63,225,33);
    put(auto_live_,792,65,130,28);
    put(popout_live_,1190,64,172,30);
    put(history_limit_label_,292,65,230,28);
    put(history_limit_slider_,500,63,225,33);
    put(auto_history_,792,65,130,28);
    put(popout_history_,1190,64,172,30);
    put(period_,292,111,158,200);
    put(from_,474,112,150,27);
    put(to_,642,112,150,27);
    put(first_label_,292,146,600,18);
    put(uptime_limit_label_,292,66,205,28);
    put(uptime_limit_slider_,490,63,210,33);
    put(show_system_,722,64,229,31);
    put(popout_table_,1190,64,172,30);
    put(uptime_period_,292,111,158,190);
    put(uptime_from_,474,112,150,27);
    put(uptime_to_,642,112,150,27);
    put(table_,292,171,1078,620);
    put(system_period_,292,65,170,200);
    put(system_auto_,540,65,180,30);
    put(system_popout_,1190,64,172,30);
    put(fill_toggle_,1055,114,205,29);
    put(pause_toggle_,855,114,175,29);
    for(HWND control:{limit_label_,limit_slider_,auto_live_,popout_live_})
        ShowWindow(control,live?SW_SHOW:SW_HIDE);
    for(HWND control:{history_limit_label_,history_limit_slider_,auto_history_,popout_history_,period_})
        ShowWindow(control,history?SW_SHOW:SW_HIDE);
    for(HWND control:{from_,to_})
        ShowWindow(control,history?SW_SHOW:SW_HIDE);
    for(HWND control:{uptime_limit_label_,uptime_limit_slider_,show_system_,popout_table_,uptime_period_})
        ShowWindow(control,uptime?SW_SHOW:SW_HIDE);
    for(HWND control:{uptime_from_,uptime_to_})
        ShowWindow(control,uptime?SW_SHOW:SW_HIDE);
    for(HWND control:{system_period_,system_auto_,system_popout_})
        ShowWindow(control,system?SW_SHOW:SW_HIDE);
    ShowWindow(table_,uptime?SW_SHOW:SW_HIDE);
    ShowWindow(fill_toggle_,uptime?SW_HIDE:SW_SHOW);
    ShowWindow(pause_toggle_,uptime?SW_HIDE:SW_SHOW);
    ShowWindow(first_label_,SW_HIDE);
    ListView_SetColumnWidth(table_,0,sx(370));
    ListView_SetColumnWidth(table_,1,sx(165));
    ListView_SetColumnWidth(table_,2,sx(265));
    HFONT stale_font=nullptr;
    const UINT dpi=GetDpiForWindow(hwnd_);
    if(!font_ || controls_font_dpi_!=dpi) {
        HFONT replacement=CreateFontW(-MulDiv(14,dpi,96),0,0,0,FW_NORMAL,0,0,0,
            DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
        if(replacement){stale_font=font_;font_=replacement;controls_font_dpi_=dpi;}
    }
    for(HWND control:{period_,from_,to_,uptime_period_,uptime_from_,uptime_to_,first_label_,
        show_system_,limit_label_,limit_slider_,history_limit_label_,history_limit_slider_,
        uptime_limit_label_,uptime_limit_slider_,table_,popout_live_,popout_history_,
        popout_table_,auto_live_,auto_history_,system_period_,system_auto_,system_popout_,fill_toggle_,pause_toggle_})
        SendMessageW(control,WM_SETFONT,reinterpret_cast<WPARAM>(font_),TRUE);
    if(stale_font) DeleteObject(stale_font);
    ListView_SetBkColor(table_,GetSysColor(COLOR_WINDOW));
    ListView_SetTextBkColor(table_,GetSysColor(COLOR_WINDOW));
    ListView_SetTextColor(table_,GetSysColor(COLOR_WINDOWTEXT));
}
void StatisticsPage::activate(bool active) {
    active_=active;ShowWindow(hwnd_,active?SW_SHOW:SW_HIDE);
    if(!active) page_buffer_.clear(); // Release offscreen memory in other settings tabs.
    if(!active && tooltip_) ShowWindow(tooltip_,SW_HIDE);
    if(!active && !has_popup()) KillTimer(hwnd_,animate_message);
    if(active) request();
}
void StatisticsPage::set_limit(unsigned limit) {
    diagnostics::write(diagnostics::Level::info,"ui.statistics_limit","limit="+std::to_string(limit));
    limit=std::clamp(limit,1U,100U);
    if(limit_==limit) return;
    limit_=limit;
    SendMessageW(limit_slider_,TBM_SETPOS,TRUE,limit_);
    SetWindowTextW(limit_label_,(L"Количество приложений: "+std::to_wstring(limit_)+L" / 100").c_str());
    // Overlay::save_settings() owns the shared [overlay] top key.
    rebuild_live_series();
    if(active_) InvalidateRect(hwnd_,nullptr,FALSE);
    for(auto& popup:popups_) if(popup.window) InvalidateRect(popup.window,nullptr,FALSE);
    if(active_ || table_window_ || popups_[0].window || popups_[1].window) request();
}
void StatisticsPage::set_history_limit(unsigned value) {
    value=std::clamp(value,1U,100U);
    if(value==history_limit_) return;
    history_limit_=value;
    SendMessageW(history_limit_slider_,TBM_SETPOS,TRUE,value);
    SetWindowTextW(history_limit_label_,(L"История: "+std::to_wstring(value)+L" / 100").c_str());
    ini_overlay_write(settings_,L"stats_history_top",static_cast<int>(value));
    history_axis_refit_pending_=true; // New selection changes the history Y-axis peak.
    diagnostics::write(diagnostics::Level::info,"ui.history_limit","limit="+std::to_string(value));
    if(active_ || popups_[1].window) request();
}
void StatisticsPage::set_uptime_limit(unsigned value) {
    value=std::clamp(value,1U,100U);
    if(value==uptime_limit_) return;
    uptime_limit_=value;
    SendMessageW(uptime_limit_slider_,TBM_SETPOS,TRUE,value);
    SetWindowTextW(uptime_limit_label_,(L"Аптайм: "+std::to_wstring(value)+L" / 100").c_str());
    ini_overlay_write(settings_,L"stats_uptime_top",static_cast<int>(value));
    diagnostics::write(diagnostics::Level::info,"ui.uptime_limit","limit="+std::to_string(value));
    if(active_ || table_window_) request();
}
std::int64_t StatisticsPage::custom_timestamp(HWND picker,bool until_end) {
    SYSTEMTIME local{};
    if(SendMessageW(picker,DTM_GETSYSTEMTIME,0,reinterpret_cast<LPARAM>(&local))!=GDT_VALID) return now_ms();
    local.wHour=0;local.wMinute=0;local.wSecond=0;local.wMilliseconds=0;
    if(until_end) {
        FILETIME f{};
        if(SystemTimeToFileTime(&local,&f)) {
            ULARGE_INTEGER ticks{};ticks.LowPart=f.dwLowDateTime;ticks.HighPart=f.dwHighDateTime;
            ticks.QuadPart+=86400ULL*10000000ULL;
            f.dwLowDateTime=ticks.LowPart;f.dwHighDateTime=ticks.HighPart;
            FileTimeToSystemTime(&f,&local);
        }
    }
    SYSTEMTIME utc{};if(!TzSpecificLocalTimeToSystemTime(nullptr,&local,&utc)) return now_ms();
    FILETIME f{};if(!SystemTimeToFileTime(&utc,&f)) return now_ms();
    ULARGE_INTEGER ticks{};ticks.LowPart=f.dwLowDateTime;ticks.HighPart=f.dwHighDateTime;
    return static_cast<std::int64_t>((ticks.QuadPart-116444736000000000ULL)/10000ULL);
}
void StatisticsPage::request(bool uptime_only) {
    diagnostics::write(diagnostics::Level::trace,"ui.statistics","statistics query requested");
    if(!active_ && !has_popup() && !table_window_) return;
    const auto now=now_ms();
    const auto range=[&](int index,HWND begin_picker,HWND end_picker){
        std::int64_t begin=index==0?now-86400000:now-604800000;
        std::int64_t end=now+1000;
        if(index==2) {
            begin=custom_timestamp(begin_picker,false);
            end=custom_timestamp(end_picker,true);
        }
        if(first_record_) begin=std::max(begin,first_record_);
        end=std::min(end,now+86400000);
        return std::pair<std::int64_t,std::int64_t>{begin,end};
    };
    const auto [from,to]=range(period_index_,from_,to_);
    const auto [uptime_from,uptime_to]=range(uptime_period_index_,uptime_from_,uptime_to_);
    // A date range may be empty independently; do not disable the other section.
    if(!uptime_only) {window_from_=from;window_to_=to;}
    {
        std::lock_guard lock(mutex_);
        cancel_query_.request_stop();cancel_query_=std::stop_source{};
        requested_={from,to,uptime_from,uptime_to,limit_,history_limit_,uptime_limit_,system_,uptime_only,++generation_};
    }
    wake_.notify_all();
    if(!uptime_only) last_history_refresh_=now;
}
void StatisticsPage::update_dates() {
    const bool custom=period_index_==2;
    const bool uptime_custom=uptime_period_index_==2;
    EnableWindow(from_,custom);EnableWindow(to_,custom);
    EnableWindow(uptime_from_,uptime_custom);EnableWindow(uptime_to_,uptime_custom);
    if(hwnd_) layout();
    if(first_record_) {
        SYSTEMTIME bounds[2]{local_time(first_record_),local_time(now_ms())};
        for(HWND picker:{from_,to_,uptime_from_,uptime_to_})
            SendMessageW(picker,DTM_SETRANGE,GDTR_MIN|GDTR_MAX,reinterpret_cast<LPARAM>(bounds));
        SetWindowTextW(first_label_,L"");
    }
}
void StatisticsPage::accept() {
    std::optional<std::pair<unsigned,StatisticsHistory>> item;
    bool uptime_only=false;
    {
        std::lock_guard lock(mutex_);
        if(completed_ && completed_->first==generation_) {
            uptime_only=requested_.uptime_only;
            item.swap(completed_);
        }
    }
    if(!item) return;
    if(uptime_only) {
        // Do not rebuild chart series, reset axis easing, or invalidate graph pixels.
        history_.uptime=std::move(item->second.uptime);
        populate_table();
        return;
    }
    const bool first_history_data=history_.history.empty() && !item->second.history.empty();
    history_=std::move(item->second);
    if(first_history_data)history_axis_refit_pending_=true;
    if(history_.first_record_ms && first_record_!=history_.first_record_ms) {
        first_record_=history_.first_record_ms;update_dates();
    }
    rebuild_live_series();
    if(history_axis_refit_pending_) {
        refresh_auto_axis(1,true);
        history_axis_refit_pending_=false;
    }
    populate_table();InvalidateRect(hwnd_,nullptr,FALSE);
    for(auto& popup:popups_) if(popup.window) InvalidateRect(popup.window,nullptr,FALSE);
}
void StatisticsPage::populate_table() {
    // Render the same selected SQLite rows into the tab and the detached table.
    const auto fill=[this](HWND list,HIMAGELIST images) {
        if(!list || !IsWindow(list)) return;
        SendMessageW(list,WM_SETREDRAW,FALSE,0);
        ListView_DeleteAllItems(list);
        if(images) ImageList_RemoveAll(images);
        std::map<std::wstring,int> icons;
        for(std::size_t i=0;i<history_.uptime.size();++i) {
            const auto& app=history_.uptime[i];
            LVITEMW item{};
            item.mask=LVIF_TEXT|LVIF_IMAGE;
            item.iItem=static_cast<int>(i);
            item.pszText=const_cast<wchar_t*>(app.name.c_str());
            item.iImage=-1;
            if(images && !app.path.empty()) {
                if(const auto found=icons.find(app.path);found!=icons.end()) item.iImage=found->second;
                else if(HICON icon=application_icon(app.path)) {
                    item.iImage=ImageList_AddIcon(images,icon);
                    icons.emplace(app.path,item.iImage);
                }
            }
            const auto row=ListView_InsertItem(list,&item);
            if(row<0) continue;
            auto uptime=span_label(app.observed_seconds);
            auto last=local_label(app.last_seen_ms);
            ListView_SetItemText(list,row,1,uptime.data());
            ListView_SetItemText(list,row,2,last.data());
        }
        SendMessageW(list,WM_SETREDRAW,TRUE,0);
        InvalidateRect(list,nullptr,FALSE);
    };
    fill(table_,image_list_);
    fill(table_window_list_,table_image_list_);
    if(table_window_info_) {
        const auto line=L"Наблюдаемый аптайм  ·  "+std::to_wstring(history_.uptime.size())+
            L" приложений  ·  лимит "+std::to_wstring(uptime_limit_);
        SetWindowTextW(table_window_info_,line.c_str());
    }
}

void StatisticsPage::open_table() {
    if(table_window_ && IsWindow(table_window_)) {
        ShowWindow(table_window_,SW_RESTORE);
        SetForegroundWindow(table_window_);
        return;
    }
    WNDCLASSEXW cls{};
    cls.cbSize=sizeof(cls);
    cls.hInstance=GetModuleHandleW(nullptr);
    cls.lpfnWndProc=table_dispatch;
    cls.lpszClassName=table_class;
    cls.hCursor=LoadCursorW(nullptr,IDC_ARROW);
    cls.hbrBackground=GetSysColorBrush(COLOR_BTNFACE);
    register_window_class(cls,"Register detached uptime table");
    table_window_=CreateWindowExW(0,table_class,L"FluxTop — аптайм приложений",
        WS_OVERLAPPEDWINDOW|WS_VISIBLE|WS_CLIPCHILDREN,CW_USEDEFAULT,CW_USEDEFAULT,980,560,
        nullptr,nullptr,GetModuleHandleW(nullptr),this);
    if(!table_window_) throw windows_error("Create detached uptime table");
    ShowWindow(table_window_,SW_SHOW);
    populate_table();
    request();
}

void StatisticsPage::layout_table_window() {
    if(!table_window_ || !table_window_list_ || !IsWindow(table_window_list_)) return;
    RECT r{};GetClientRect(table_window_,&r);
    const int width=std::max(0,static_cast<int>(r.right-r.left));
    const int height=std::max(0,static_cast<int>(r.bottom-r.top));
    // The detached table does not need a status/instruction strip.
    if(table_window_info_) ShowWindow(table_window_info_,SW_HIDE);
    MoveWindow(table_window_list_,16,12,std::max(0,width-32),std::max(0,height-28),TRUE);
    const int available=std::max(450,width-58);
    ListView_SetColumnWidth(table_window_list_,0,available*47/100);
    ListView_SetColumnWidth(table_window_list_,1,available*21/100);
    ListView_SetColumnWidth(table_window_list_,2,available*32/100);
}

LRESULT CALLBACK StatisticsPage::table_dispatch(HWND window,UINT msg,WPARAM wp,LPARAM lp) {
    auto* page=reinterpret_cast<StatisticsPage*>(GetWindowLongPtrW(window,GWLP_USERDATA));
    if(msg==WM_NCCREATE) {
        page=static_cast<StatisticsPage*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        page->table_window_=window;
        SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(page));
        return TRUE;
    }
    if(!page) return DefWindowProcW(window,msg,wp,lp);
    switch(msg) {
    case WM_CREATE: {
        page->table_window_info_=CreateWindowExW(0,L"STATIC",L"Наблюдаемый аптайм",
            WS_CHILD|WS_VISIBLE|SS_LEFT,0,0,1,1,window,nullptr,GetModuleHandleW(nullptr),nullptr);
        page->table_window_list_=CreateWindowExW(WS_EX_CLIENTEDGE,WC_LISTVIEWW,L"",
            WS_CHILD|WS_VISIBLE|WS_TABSTOP|LVS_REPORT|LVS_SHOWSELALWAYS|
            LVS_SINGLESEL|LVS_SHAREIMAGELISTS,0,0,1,1,
            window,nullptr,GetModuleHandleW(nullptr),nullptr);
        if(!page->table_window_info_ || !page->table_window_list_) return -1;
        ListView_SetExtendedListViewStyle(page->table_window_list_,
            LVS_EX_FULLROWSELECT|LVS_EX_DOUBLEBUFFER);
        for(const wchar_t* title:{L"Приложение",L"Аптайм",L"Последняя активность"}) {
            LVCOLUMNW col{};col.mask=LVCF_TEXT|LVCF_WIDTH;
            col.pszText=const_cast<wchar_t*>(title);col.cx=180;
            ListView_InsertColumn(page->table_window_list_,
                Header_GetItemCount(ListView_GetHeader(page->table_window_list_)),&col);
        }
        page->table_image_list_=ImageList_Create(16,16,ILC_COLOR32|ILC_MASK,16,16);
        if(page->table_image_list_)
            ListView_SetImageList(page->table_window_list_,page->table_image_list_,LVSIL_SMALL);
        page->table_font_=CreateFontW(-16,0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
        if(page->table_font_) for(HWND child:{page->table_window_info_,page->table_window_list_})
            SendMessageW(child,WM_SETFONT,reinterpret_cast<WPARAM>(page->table_font_),TRUE);
        page->layout_table_window();
        return 0;
    }
    case WM_SIZE: page->layout_table_window();return 0;
    case WM_GETMINMAXINFO: {
        auto* bounds=reinterpret_cast<MINMAXINFO*>(lp);
        bounds->ptMinTrackSize={540,300};return 0;
    }
    case WM_CLOSE:DestroyWindow(window);return 0;
    case WM_DESTROY:
        if(page->table_window_list_)
            ListView_SetImageList(page->table_window_list_,nullptr,LVSIL_SMALL);
        if(page->table_image_list_) {ImageList_Destroy(page->table_image_list_);page->table_image_list_=nullptr;}
        if(page->table_font_) {DeleteObject(page->table_font_);page->table_font_=nullptr;}
        page->table_window_=nullptr;page->table_window_list_=nullptr;page->table_window_info_=nullptr;
        return 0;
    }
    return DefWindowProcW(window,msg,wp,lp);
}

HICON StatisticsPage::application_icon(const std::wstring& path) {
    if(path.empty()) return nullptr;
    if(const auto it=icon_cache_.find(path);it!=icon_cache_.end()) return it->second;
    if(icon_cache_.size()>=256) {
        for(const auto& [name,icon]:icon_cache_) { (void)name; if(icon) DestroyIcon(icon); }
        icon_cache_.clear();
    }
    SHFILEINFOW info{};
    DWORD_PTR ok=SHGetFileInfoW(path.c_str(),0,&info,sizeof(info),SHGFI_ICON|SHGFI_SMALLICON);
    if(!ok) ok=SHGetFileInfoW(L"sample.exe",FILE_ATTRIBUTE_NORMAL,&info,sizeof(info),
        SHGFI_ICON|SHGFI_SMALLICON|SHGFI_USEFILEATTRIBUTES);
    auto icon=ok?info.hIcon:nullptr;
    icon_cache_.emplace(path,icon);return icon;
}
bool StatisticsPage::has_popup() const {
    return std::any_of(popups_.begin(),popups_.end(),[](const auto& p){return p.window!=nullptr;});
}
unsigned StatisticsPage::selected_popup_kind() const {
    constexpr std::array<unsigned,6> mapping{{2,3,4,5,0,1}};
    return mapping[std::min<std::size_t>(selected_section_,5)];
}
void StatisticsPage::select_disk(wchar_t letter) {
    if(letter<L'A' || letter>L'Z' || selected_disk_letter_==letter)return;
    selected_disk_letter_=letter;
    ini_overlay_write(settings_,L"stats_selected_disk",static_cast<int>(letter));
    system_hover_={-1,-1};system_tip_active_=false;hover_section_=-1;
    if(tooltip_)ShowWindow(tooltip_,SW_HIDE);
    refresh_pause_control();
    layout();InvalidateRect(hwnd_,nullptr,FALSE);
}
void StatisticsPage::select_section(unsigned index) {
    if(index>=7 || (index==selected_section_ && !selected_disk_letter_))return;
    selected_disk_letter_=0;
    ini_overlay_write(settings_,L"stats_selected_disk",0);
    selected_section_=index;
    refresh_pause_control();
    ini_overlay_write(settings_,L"stats_selected_section",static_cast<int>(index));
    hover_section_=-1;
    system_tip_active_=false;
    if(tooltip_)ShowWindow(tooltip_,SW_HIDE);
    system_hover_={-1,-1};
    layout();
    if(index>=4)request();
    InvalidateRect(hwnd_,nullptr,FALSE);
}
void StatisticsPage::on_snapshot(std::int64_t timestamp_ms,const std::vector<Process>& grouped,
    unsigned interval_ms,const Snapshot& system) {
    if(!active_ && !has_popup() && !table_window_) return;
    // A standalone uptime table only needs periodic database refreshes, not
    // rebuilding and repainting the one-minute graph for every sample.
    if(!active_ && !has_popup()) {
        if(now_ms()-last_history_refresh_>30000) request();
        return;
    }
    std::vector<wchar_t> found_letters;
    found_letters.reserve(system.disks.size());
    for(const auto& disk:system.disks)found_letters.push_back(disk.letter);
    if(found_letters!=disk_letters_) {
        disk_letters_=std::move(found_letters);
        const auto max_scroll=static_cast<unsigned>(disk_letters_.size());
        sidebar_scroll_=std::min(sidebar_scroll_,max_scroll);
        if(selected_disk_letter_ && std::find(disk_letters_.begin(),disk_letters_.end(),selected_disk_letter_)==disk_letters_.end()) {
            selected_disk_letter_=0;
            ini_overlay_write(settings_,L"stats_selected_disk",0);
            refresh_pause_control();
            layout();
        }
    }
    SystemPoint point;point.time=timestamp_ms;
    for(const auto& disk:system.disks)
        if(disk.letter>=L'A' && disk.letter<=L'Z')
            point.disks[disk.letter-L'A']=disk.active_percent;
    point.cpu_temperature=system.cpu_temperature_c;
    for(const auto& disk:system.disks)
        if(disk.letter>=L'A' && disk.letter<=L'Z')
            point.disk_temperatures[disk.letter-L'A']=disk.temperature_c;
    point.values[0]=system.total_cpu_percent;
    if(system.total_ram_used && system.total_ram_capacity && *system.total_ram_capacity)
        point.values[1]=100.0*static_cast<double>(*system.total_ram_used)/static_cast<double>(*system.total_ram_capacity);
    if(system.total_rx_bytes_per_second && system.total_tx_bytes_per_second)
        point.values[2]=8.0*(*system.total_rx_bytes_per_second+*system.total_tx_bytes_per_second)/1000000.0;
    point.values[3]=system.total_gpu_percent;
    if(system.total_gpu_memory && system.total_gpu_memory_capacity && *system.total_gpu_memory_capacity)
        point.values[4]=100.0*static_cast<double>(*system.total_gpu_memory)/
            static_cast<double>(*system.total_gpu_memory_capacity);
    system_info_.ram_used=system.total_ram_used;
    system_info_.ram_capacity=system.total_ram_capacity;
    system_info_.gpu_memory=system.total_gpu_memory;
    system_info_.gpu_capacity=system.total_gpu_memory_capacity;
    system_info_.rx=system.total_rx_bytes_per_second;
    system_info_.tx=system.total_tx_bytes_per_second;
    system_info_.cpu_temp=system.cpu_temperature_c;
    system_info_.gpu_temp=system.gpu_temperature_c;
    system_info_.cpu_board=system.cpu_temperature_board;
    system_info_.cpu_source=system.cpu_temperature_source;
    system_info_.logical_processors=system.logical_processors;
    system_info_.collection_ms=system.collection_ms;
    system_points_.push_back(point);
    if(smooth_system_end_<=0.0)smooth_system_end_=static_cast<double>(timestamp_ms);
    if(smooth_live_end_<=0.0)smooth_live_end_=static_cast<double>(timestamp_ms);
    while(!system_points_.empty() && system_points_.front().time<timestamp_ms-900000)
        system_points_.pop_front();
    while(system_points_.size()>900U)system_points_.pop_front();
    animation_started_=GetTickCount64();
    animation_duration_ms_=std::clamp(interval_ms,250U,10000U);
    last_live_timestamp_=timestamp_ms;
    if(active_ || has_popup()) SetTimer(hwnd_,animate_message,33,nullptr);
    struct Ranked { const Process* process; double score; };
    std::vector<Ranked> sorted;
    sorted.reserve(grouped.size());
    // One UTF-8 conversion per metric row, and one AutoScore evaluation per app.
    current_readings_.clear();
    for(const auto& p:grouped) {
        StatisticsApplication identity;
        identity.name=stat_wide(p.name);
        identity.path=p.exe_path?stat_wide(*p.exe_path):L"";
        identity.identity=stat_wide(history_identity(p));
        if(statistics_is_system(identity.name,identity.path)) continue;
        CurrentReadings values;
        values.cpu=stat_percent(p.cpu_percent);
        values.ram=stat_memory(p.working_set);
        values.gpu=stat_percent(p.gpu_percent);
        values.vram=stat_memory(p.dedicated_gpu_bytes);
        values.network=stat_network(p.rx_bytes_per_second,p.tx_bytes_per_second);
        current_readings_.emplace(color_key(identity),std::move(values));
        if(history_score_available(p))
            sorted.push_back({&p,auto_rank(p,31U).score});
    }
    // The bubble keeps showing up-to-date readings even when the cursor rests
    // on the same sample. One invalidation per real snapshot, not per animation.
    if(tooltip_ && IsWindowVisible(tooltip_))InvalidateRect(tooltip_,nullptr,FALSE);
    std::stable_sort(sorted.begin(),sorted.end(),[](const Ranked& a,const Ranked& b){
        return a.score>b.score;
    });
    // Keep up to 100 live series so increasing stats_top does not need a new collection.
    for(std::size_t i=0;i<std::min<std::size_t>(sorted.size(),100U);++i) {
        const auto* p=sorted[i].process;
        live_.push_back({timestamp_ms,stat_wide(p->name),p->exe_path?stat_wide(*p->exe_path):L"",
            stat_wide(history_identity(*p)),sorted[i].score});
    }
    while(!live_.empty() && live_.front().time<timestamp_ms-60000) live_.pop_front();
    // Bounds a burst of very short sampling intervals or an unexpected clock rollback.
    while(live_.size()>10000) live_.pop_front();
    rebuild_live_series();
    if(now_ms()-last_history_refresh_>30000) request();
    if(active_) InvalidateRect(hwnd_,nullptr,FALSE);
    for(auto& popup:popups_) if(popup.window) InvalidateRect(popup.window,nullptr,FALSE);
}
double StatisticsPage::icon_hue(const std::wstring& key,const std::wstring& path) {
    if(const auto found=icon_hues_.find(key);found!=icon_hues_.end()) return found->second;
    // Stable pseudo-random fallback for white/monochrome or missing icons.
    std::uint32_t hash=2166136261U;
    for(wchar_t ch:key) {hash^=static_cast<std::uint32_t>(ch);hash*=16777619U;}
    double hue=static_cast<double>(hash%360U);
    if(auto icon=application_icon(path)) {
        HDC dc=CreateCompatibleDC(nullptr);
        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth=32;bmi.bmiHeader.biHeight=-32;
        bmi.bmiHeader.biPlanes=1;bmi.bmiHeader.biBitCount=32;bmi.bmiHeader.biCompression=BI_RGB;
        void* pixels=nullptr;
        HBITMAP bitmap=dc?CreateDIBSection(dc,&bmi,DIB_RGB_COLORS,&pixels,nullptr,0):nullptr;
        if(dc && bitmap && pixels) {
            auto old=SelectObject(dc,bitmap);
            std::fill_n(static_cast<std::uint32_t*>(pixels),32*32,0xffffffffU);
            if(DrawIconEx(dc,0,0,icon,32,32,0,nullptr,DI_NORMAL)) {
                std::array<double,24> weights{};
                for(unsigned i=0;i<32*32;++i) {
                    const auto px=static_cast<std::uint32_t*>(pixels)[i];
                    const int r=static_cast<int>((px>>16)&255U),g=static_cast<int>((px>>8)&255U),b=static_cast<int>(px&255U);
                    const int high=std::max({r,g,b}),low=std::min({r,g,b}),delta=high-low;
                    if(high>242 || high<30 || delta<35) continue;
                    const auto candidate=pixel_hue(r,g,b);
                    const auto bin=std::min(23,static_cast<int>(candidate/15.0));
                    weights[static_cast<std::size_t>(bin)]+=static_cast<double>(delta)/255;
                }
                const auto best=std::max_element(weights.begin(),weights.end());
                if(best!=weights.end() && *best>1.0)
                    hue=15.0*(std::distance(weights.begin(),best)+.5);
            }
            SelectObject(dc,old);
        }
        if(bitmap) DeleteObject(bitmap);
        if(dc) DeleteDC(dc);
    }
    icon_hues_[key]=hue;
    return hue;
}
std::wstring StatisticsPage::color_key(const StatisticsApplication& app) {
    return !app.identity.empty()?app.identity:
        (app.path.empty()?L"name:"+app.name:L"path:"+app.path);
}
void StatisticsPage::prepare_colors(const std::vector<StatisticsApplication>& minute) {
    // Assign the SAME colour to an executable in both live and history charts.
    std::map<std::wstring,std::wstring> paths;
    for(const auto* series:std::array<const std::vector<StatisticsApplication>*,2>{&minute,&history_.history})
        for(const auto& app:*series) paths.emplace(color_key(app),app.path);
    chart_colors_.clear();
    std::vector<double> used;
    const double gap=std::max(3.0,14.0-0.11*static_cast<double>(paths.size()));
    for(const auto& [key,path]:paths) {
        double hue=icon_hue(key,path);
        for(int attempt=0;attempt<48;++attempt) {
            const auto collision=std::any_of(used.begin(),used.end(),[&](double other){
                const double delta=std::abs(hue-other);
                return std::min(delta,360.0-delta)<gap;
            });
            if(!collision) break;
            hue=std::fmod(hue+47.0,360.0);
        }
        used.push_back(hue);
        chart_colors_.emplace(key,colored_ink(hue));
    }
}
COLORREF StatisticsPage::chart_color(const StatisticsApplication& app) const {
    if(const auto it=chart_colors_.find(color_key(app));it!=chart_colors_.end()) return it->second;
    return colored_ink(210);
}
COLORREF StatisticsPage::series_color(const StatisticsApplication& app,std::size_t index) const {
    (void)index;
    return chart_color(app);
}
HPEN StatisticsPage::graph_pen(COLORREF color,bool dotted) {
    auto& cache=dotted?detail_pens_:stroke_pens_;
    if(const auto found=cache.find(color);found!=cache.end()) return found->second;
    // Bound GDI handles even across long sessions with changing graph palettes.
    if(cache.size()>=256) {
        for(const auto& [rgb,pen]:cache) { (void)rgb; DeleteObject(pen); }
        cache.clear();
    }
    HPEN pen=CreatePen(dotted?PS_DOT:PS_SOLID,dotted?1:2,color);
    if(pen) cache.emplace(color,pen);
    return pen;
}
// Shared visual renderer: background, time grid, alpha fills and all strokes.
// Chart providers only prepare series and set ChartStyle; no alternate paint path.
void StatisticsPage::draw_chart_core(HDC dc,const ChartStyle& style,
    const std::vector<ChartStroke>& input_plots,const std::vector<ChartBar>& bars) {
    const RECT& area=style.area;
    if(area.right<=area.left || area.bottom<=area.top)return;
    // Transform the ENTIRE live series before its fill and stroke are drawn.
    // The same clip rectangle hides new samples and GDI fill seams outside
    // the right edge. No blank margin is introduced on the visible chart.
    const int guard=std::clamp(style.right_edge_overscan,0,24);
    std::vector<ChartStroke> projected;
    if(guard>0) {
        projected.reserve(input_plots.size());
        for(const auto& source:input_plots) {
            projected.push_back(source);
            for(auto& point:projected.back().points) {
                point.x=static_cast<LONG>(std::llround(statistics_chart_project_x(
                    static_cast<double>(point.x),static_cast<double>(area.left),
                    static_cast<double>(area.right),static_cast<double>(guard))));
            }
        }
    }
    const std::vector<ChartStroke>& plots=guard>0?projected:input_plots;
    const HBRUSH paper=CreateSolidBrush(style.paper);
    if(paper) {FillRect(dc,&area,paper);DeleteObject(paper);}
    const int saved=SaveDC(dc);
    if(!saved)return;
    IntersectClipRect(dc,area.left,area.top,area.right,area.bottom);
    if(style.horizontal_rows>0 || style.time_divisions>0) {
        const HPEN grid=graph_pen(style.grid,true);
        const HGDIOBJ previous=grid?SelectObject(dc,grid):nullptr;
        if(style.horizontal_rows>0)for(int row=0;row<=style.horizontal_rows;++row) {
            const LONG y=area.top+(area.bottom-area.top)*row/style.horizontal_rows;
            MoveToEx(dc,area.left,y,nullptr);LineTo(dc,area.right,y);
        }
        if(style.time_divisions>0) {
            // Time grid shares the projected X scale with the series.
            const RECT grid_area{area.left,area.top,area.right+guard,area.bottom};
            draw_time_grid(dc,grid_area,style.from,style.to,style.time_divisions);
        }
        if(grid)SelectObject(dc,previous);
    }
    if(fill_enabled_ && style.fill_opacity>0 && !plots.empty()) {
        BYTE opacity=style.fill_opacity;
        if(style.adaptive_fill)
            opacity=static_cast<BYTE>(std::min<std::size_t>(opacity,
                std::max<std::size_t>(1,72/plots.size())));
        // Draw every fill before every stroke, preserving overlapping curves.
        for(const auto& plot:plots)fill_under(dc,plot.points,area.bottom,plot.color,opacity);
    }
    // Bars are drawn through the same clipping, background and renderer.
    for(const auto& bar:bars) {
        const HBRUSH brush=CreateSolidBrush(bar.color);
        if(brush) {FillRect(dc,&bar.area,brush);DeleteObject(brush);}
    }
    SelectObject(dc,GetStockObject(NULL_BRUSH));
    for(const auto& plot:plots) {
        const auto& line=plot.points;
        if(line.empty())continue;
        const HPEN stroke=graph_pen(plot.color,false);
        const HGDIOBJ previous=stroke?SelectObject(dc,stroke):nullptr;
        if(line.size()>1)
            Polyline(dc,line.data(),static_cast<int>(line.size()));
        else if(style.point_markers)
            Ellipse(dc,line[0].x-2,line[0].y-2,line[0].x+2,line[0].y+2);
        if(stroke)SelectObject(dc,previous);
        if(style.ink_detail && line.size()>1) {
            const HPEN detail=graph_pen(plot.color,true);
            const HGDIOBJ previous_detail=detail?SelectObject(dc,detail):nullptr;
            Polyline(dc,line.data(),static_cast<int>(line.size()));
            if(detail)SelectObject(dc,previous_detail);
        }
    }
    RestoreDC(dc,saved);
}
void StatisticsPage::draw_graph(HDC dc,const RECT& area,const std::vector<StatisticsApplication>& apps,
    std::int64_t from,std::int64_t to,const wchar_t* caption,bool is_live,
    double vertical_zoom,bool detached) {
    if(to<=from)to=from+1;
    // AutoScore only maps time/value samples to pixels; all painting uses the core.
    std::vector<ChartStroke> plots;
    plots.reserve(apps.size());
    // Exceedances are visual markers only; line values and hover data stay raw.
    std::vector<LONG> clipped_peak_x;
    const double axis_ceiling=vertical_zoom>0.0?100.0/vertical_zoom:100.0;
    for(std::size_t i=0;i<apps.size();++i) {
        const auto& points=apps[i].points;
        if(points.empty())continue;
        std::vector<POINT> line;
        line.reserve(points.size()+2);
        const auto lower=std::lower_bound(points.begin(),points.end(),from,
            [](const auto& point,std::int64_t time){return point.first<time;});
        const auto first=lower==points.begin()?lower:std::prev(lower);
        const auto upper=std::upper_bound(lower,points.end(),to,
            [](std::int64_t time,const auto& point){return time<point.first;});
        const auto last=upper==points.end()?upper:std::next(upper);
        for(auto point=first;point!=last;++point) {
            const auto [at,score]=*point;
            const auto fraction=static_cast<double>(at-from)/static_cast<double>(to-from);
            const auto jitter=.9*std::sin(static_cast<double>(at%5003)*.019+i*2.3);
            const auto xx=static_cast<double>(area.left)+fraction*(area.right-area.left);
            if(std::isfinite(score) && score>axis_ceiling+0.001 && at>=from && at<=to &&
                xx>=area.left && xx<=area.right) {
                const LONG position=static_cast<LONG>(std::lround(xx));
                if(std::none_of(clipped_peak_x.begin(),clipped_peak_x.end(),
                    [position](LONG other){return std::abs(other-position)<7;}))
                    clipped_peak_x.push_back(position);
            }
            const auto yy=static_cast<double>(area.bottom)-
                std::clamp(score*vertical_zoom,0.0,100.0)/100.0*(area.bottom-area.top);
            line.push_back(POINT{static_cast<LONG>(std::lround(std::clamp(xx,-1000000.0,1000000.0))),
                static_cast<LONG>(std::lround(yy+jitter))});
        }
        // Keep the original live-series interpolation, without introducing fake samples.
        if(is_live && line.size()>1 && std::prev(last)->first==last_live_timestamp_
            && points.back().first==last_live_timestamp_ && animation_started_) {
            const auto elapsed=GetTickCount64()-animation_started_;
            const double t=std::clamp(static_cast<double>(elapsed)/animation_duration_ms_,0.0,1.0);
            const auto previous=line[line.size()-2],final=line.back();
            line.back().x=previous.x+static_cast<LONG>(std::lround((final.x-previous.x)*t));
            line.back().y=previous.y+static_cast<LONG>(std::lround((final.y-previous.y)*t));
        }
        // Prevent a one-frame gap when the oldest sample leaves the rolling minute.
        if(is_live)extend_fresh_left_edge(line,area,from,to,animation_duration_ms_);
        // All current AutoScore series meet the same right edge as the system series.
        if(is_live && !line.empty() && points.back().first==last_live_timestamp_)
            extend_recent_line(line,area.right,points.back().first,to,animation_duration_ms_);
        if(!line.empty())plots.push_back({std::move(line),series_color(apps[i],i)});
    }
    ChartStyle style{};
    style.area=area;style.from=from;style.to=to;
    style.horizontal_rows=4;style.time_divisions=8;
    style.grid=RGB(210,217,221);
    style.adaptive_fill=true;style.ink_detail=true;
    style.right_edge_overscan=is_live && to>=live_view_end()-1000?live_tail_guard_px:0;
    draw_chart_core(dc,style,plots);
    // Small downward chevrons identify original readings clipped by robust
    // auto-zoom. Reuse the normal chart geometry and preserve GDI object state.
    if(!clipped_peak_x.empty()) {
        const HPEN marker=CreatePen(PS_SOLID,2,RGB(165,70,37));
        if(marker) {
            const HGDIOBJ previous=SelectObject(dc,marker);
            for(const LONG x:clipped_peak_x) {
                MoveToEx(dc,x-3,area.top+3,nullptr);LineTo(dc,x,area.top+7);
                LineTo(dc,x+3,area.top+3);
            }
            SelectObject(dc,previous);
            DeleteObject(marker);
        }
    }
    label(dc,caption,area.left,area.top-28,RGB(64,63,61));
    const auto top_value=std::to_wstring(static_cast<int>(std::lround(100.0/vertical_zoom)));
    label(dc,top_value.c_str(),area.left-33,area.top-3);
    label(dc,L"0",area.left-15,area.bottom-13);
    if(apps.empty() || std::none_of(apps.begin(),apps.end(),[](const auto& a){return !a.points.empty();}))
        label(dc,is_live?L"Ожидание данных…":L"Нет данных за выбранный период",
            area.left+20,area.top+55,RGB(119,111,98));
    const auto start_label=is_live && !detached?std::wstring(L"−60 секунд"):local_label(from);
    const auto end_label=is_live && !detached?std::wstring(L"сейчас"):local_label(to-1);
    label(dc,start_label.c_str(),area.left,area.bottom+3,RGB(97,91,82));
    SIZE extent{};
    GetTextExtentPoint32W(dc,end_label.c_str(),static_cast<int>(end_label.size()),&extent);
    label(dc,end_label.c_str(),area.right-extent.cx,area.bottom+3,RGB(97,91,82));
}
// Refit from the same series and period that draw_graph() uses. Switching
// the checkbox back on snaps immediately; subsequent samples retain easing.
void StatisticsPage::refresh_auto_axis(unsigned index,bool immediate) {
    if(index>=auto_scale_.size())return;
    const auto anchor=last_live_timestamp_>0?last_live_timestamp_:now_ms();
    const double desired=index==0?
        auto_vertical_zoom(live_apps_,anchor-60000,anchor):
        auto_vertical_zoom(history_.history,window_from_,window_to_);
    target_auto_zoom_[index]=desired;
    if(immediate || !axis_initialized_[index]) {
        cached_auto_zoom_[index]=desired;
        axis_initialized_[index]=true;
    }
    if(auto_scale_[index] && !immediate && (active_ || has_popup()) &&
        std::abs(cached_auto_zoom_[index]-desired)>0.002)
        SetTimer(hwnd_,animate_message,33,nullptr);
}
void StatisticsPage::rebuild_live_series() {
    const auto now=now_ms();
    std::vector<StatisticsApplication> minute=history_.recent;
    // Index applications once. The old search walked every series for each
    // in-memory event (up to 10,000 events in the bounded minute queue).
    std::unordered_map<std::wstring,std::size_t> positions;
    positions.reserve(minute.size()+128);
    for(std::size_t i=0;i<minute.size();++i) positions.try_emplace(color_key(minute[i]),i);
    // Merge the last minute read from SQLite with fresh in-memory samples.
    for(const auto& event:live_) {
        if(event.time<now-60000) continue;
        const auto& identity=event.identity;
        auto found=positions.find(identity);
        if(found==positions.end()) {
            StatisticsApplication app;app.name=event.name;app.path=event.path;app.identity=identity;
            minute.push_back(std::move(app));
            found=positions.emplace(identity,minute.size()-1).first;
        }
        // The database query returns ordered points. Maintain that invariant
        // while merging to avoid a linear search per event and per series.
        auto& points=minute[found->second].points;
        const auto prev=std::lower_bound(points.begin(),points.end(),event.time,
            [](const auto& p,std::int64_t stamp){return p.first<stamp;});
        if(prev!=points.end() && prev->first==event.time) prev->second=event.score;
        else points.insert(prev,{event.time,event.score});
    }
    for(auto& app:minute) {
        auto& p=app.points;
        p.erase(std::remove_if(p.begin(),p.end(),[&](const auto& v){return v.first<now-60000;}),p.end());
        std::sort(p.begin(),p.end());
    }
    std::stable_sort(minute.begin(),minute.end(),[](const auto& a,const auto& b){
        const auto peak=[](const auto& x){double s=0;for(const auto& p:x.points)s=std::max(s,p.second);return s;};
        return peak(a)>peak(b);
    });
    if(minute.size()>limit_) minute.resize(limit_);
    live_apps_=minute; // Mouse hit-testing uses the exact graph order/series colors.
    prepare_colors(live_apps_);
    // Only new snapshots and completed database queries update the axis target.
    refresh_auto_axis(0,false);
    if(!history_axis_refit_pending_)refresh_auto_axis(1,false);
    for(auto& popup:popups_) popup.axis_dirty=true;
    if(active_ || popups_[0].window || popups_[1].window)
        SetTimer(hwnd_,animate_message,33,nullptr);
}
// FluxTop.SystemDashboard.VisualRefinement.v1: continuous viewport, two GPU series, true hover bubble.
std::int64_t StatisticsPage::system_view_end() const {
    if(smooth_system_end_>0.0)return static_cast<std::int64_t>(std::llround(smooth_system_end_));
    return system_points_.empty()?now_ms():system_points_.back().time;
}
std::int64_t StatisticsPage::live_view_end() const {
    if(smooth_live_end_>0.0)return static_cast<std::int64_t>(std::llround(smooth_live_end_));
    return last_live_timestamp_>0?last_live_timestamp_:now_ms();
}
bool StatisticsPage::advance_time_view(double elapsed_ms) {
    // Do not stop the viewport when it catches up with the latest collected sample:
    // time must keep passing between samples, independent of the polling interval.
    // Sidebar sparklines remain live even on History, Uptime and paused details.
    // The selected detail chart retains its independent pause and time range.
    const bool sidebar_live=active_;
    bool live_popup=false;
    for(const auto& popup:popups_)if(popup.window && !popup.paused && popup.kind!=1) {
        live_popup=true;break;
    }
    if(!sidebar_live && !live_popup)return false;
    const double current_wall_time=static_cast<double>(now_ms());
    const double frame=std::clamp(elapsed_ms,1.0,100.0);
    const auto advance=[&](double& cursor,double latest_sample) {
        if(latest_sample<=0.0)return;
        const double target=std::max(current_wall_time,latest_sample);
        // A hidden tab or sleeping computer may have missed many frames.
        if(cursor<=0.0 || !std::isfinite(cursor) || std::abs(target-cursor)>5000.0) {
            cursor=target;return;
        }
        // Move forward every frame. Only adjust the phase gradually so a new
        // sampler tick never teleports the fill, line or timestamp grid.
        const double predicted=cursor+frame;
        const double correction=std::clamp((target-predicted)*.12,-frame*.5,frame*1.5);
        cursor=predicted+correction;
    };
    if(!system_points_.empty())
        advance(smooth_system_end_,static_cast<double>(system_points_.back().time));
    if(last_live_timestamp_>0)advance(smooth_live_end_,static_cast<double>(last_live_timestamp_));
    return smooth_system_end_>0.0 || smooth_live_end_>0.0;
}
void StatisticsPage::fill_under(HDC dc,const std::vector<POINT>& line,LONG baseline,
    COLORREF color,BYTE opacity) {
    if(!fill_enabled_ || line.size()<2 || opacity==0)return;
    // Alpha-composite into the existing chart pixels instead of drawing an opaque
    // pastel polygon. Other curves and grid remain visible through this fill.
    RECT bounds{};
    if(GetClipBox(dc,&bounds)==ERROR)return;
    LONG minimum_x=line.front().x,maximum_x=line.front().x,minimum_y=baseline;
    for(const auto& point:line) {
        minimum_x=std::min(minimum_x,point.x);
        maximum_x=std::max(maximum_x,point.x);
        minimum_y=std::min(minimum_y,point.y);
    }
    const LONG left=std::max(minimum_x,bounds.left);
    const LONG top=std::max(minimum_y,bounds.top);
    const LONG right=std::min(maximum_x,bounds.right);
    const LONG bottom=std::min(baseline,bounds.bottom);
    if(left>=right || top>=bottom)return;
    std::vector<POINT> polygon;
    polygon.reserve(line.size()+2);
    polygon.push_back({line.front().x,baseline});
    polygon.insert(polygon.end(),line.begin(),line.end());
    polygon.push_back({line.back().x,baseline});
    // ExtSelectClipRgn expects DEVICE coordinates, while the page uses an
    // anisotropic map mode. Without this conversion, resized graphs clip badly.
    if(!LPtoDP(dc,polygon.data(),static_cast<int>(polygon.size())))return;
    const HRGN mask=CreatePolygonRgn(polygon.data(),static_cast<int>(polygon.size()),WINDING);
    if(!mask)return;
    const int state=SaveDC(dc);
    if(state!=0) {
        if(ExtSelectClipRgn(dc,mask,RGN_AND)!=ERROR) {
            // PaintBuffer caches one 1x1 bitmap; no full-frame alpha surface and
            // no repeated CreateCompatibleDC/CreateCompatibleBitmap per series.
            HDC source=fill_color_buffer_.prepare(dc,1,1);
            if(source && SetPixelV(source,0,0,color)) {
                BLENDFUNCTION blend{};
                blend.BlendOp=AC_SRC_OVER;
                blend.SourceConstantAlpha=opacity;
                blend.AlphaFormat=0; // Constant alpha; no premultiplied source pixels.
                GdiAlphaBlend(dc,left,top,right-left,bottom-top,source,0,0,1,1,blend);
            }
        }
        RestoreDC(dc,state);
    }
    DeleteObject(mask);
}

// FluxTop.SystemDashboard.Stage1: sidebar+in-page chart; old popouts remain available.
void StatisticsPage::draw_sidebar(HDC dc) {
    const std::array<const wchar_t*,7> names{{L"ЦП",L"Память",L"Сеть",L"GPU",L"Авто",L"История",L"Аптайм"}};
    const std::array<COLORREF,7> colors{{RGB(14,132,177),RGB(67,105,181),RGB(184,55,114),
        RGB(131,54,156),RGB(34,133,123),RGB(85,112,142),RGB(99,123,96)}};
    const std::int64_t latest=system_view_end();
    for(unsigned section=0;section<names.size();++section) {
        const int y=18+(static_cast<int>(statistics_sidebar_section_row(
            section,static_cast<unsigned>(disk_letters_.size())))-static_cast<int>(sidebar_scroll_))*114;
        if(y>=830 || y+105<=18)continue;
        const RECT item{14,y,266,y+105};
        HBRUSH face=CreateSolidBrush(!selected_disk_letter_ && section==selected_section_?RGB(225,233,241):RGB(245,246,247));
        FillRect(dc,&item,face);DeleteObject(face);
        HBRUSH border=CreateSolidBrush(!selected_disk_letter_ && section==selected_section_?RGB(52,115,156):RGB(206,211,214));
        FrameRect(dc,&item,border);DeleteObject(border);
        label(dc,names[section],26,y+9,RGB(32,44,52));
        std::wstring reading=L"—";
        if(section<4 && !system_points_.empty()) {
            const auto& value=system_points_.back().values[section];
            if(value)reading=stat_decimal(*value)+(section==2?L" Mbps":L"%");
        } else if(section==4)reading=L"1 мин";
        else if(section==5)reading=L"История";
        else if(section==6)reading=std::to_wstring(history_.uptime.size())+L" прил.";
        RECT value_area{92,y+7,250,y+30};
        SetTextColor(dc,colors[section]);SetBkMode(dc,TRANSPARENT);
        DrawTextW(dc,reading.c_str(),-1,&value_area,DT_RIGHT|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS);
        const RECT spark{26,y+38,249,y+94};
        std::vector<POINT> points,second_gpu_line,second_cpu_temperature_line;
        std::vector<ChartBar> bars;
        const auto add=[&](std::int64_t at,double value,std::int64_t begin,std::int64_t end,double max_y){
            if(at<begin || at>end || end<=begin || max_y<=0)return;
            const auto x=spark.left+static_cast<LONG>((at-begin)*(spark.right-spark.left)/static_cast<double>(end-begin));
            const auto ypoint=spark.bottom-2-static_cast<LONG>(std::clamp(value/max_y,0.0,1.0)*(spark.bottom-spark.top-5));
            points.push_back({x,ypoint});
        };
        if(section<4) {
            double max_y=section==2?1.0:100.0;
            if(section==2)for(const auto& sample:system_points_)
                if(sample.values[2])max_y=std::max(max_y,*sample.values[2]*1.15);
            std::optional<double> previous_measurement;
            for(const auto& sample:system_points_)if(sample.values[section]) {
                const double actual=*sample.values[section];
                const double displayed=(&sample==&system_points_.back() && previous_measurement)?
                    eased_latest_value(*previous_measurement,actual,
                        animation_started_,animation_duration_ms_):actual;
                add(sample.time,displayed,latest-60000,latest,max_y);
                previous_measurement=actual;
            }
            if(!system_points_.empty() && system_points_.back().values[section])
                extend_recent_line(points,spark.right,system_points_.back().time,
                    latest,animation_duration_ms_);
            double previous_vram=0.0;
            if(section==3)for(const auto& sample:system_points_)if(sample.values[4] &&
                sample.time>=latest-60000 && sample.time<=latest) {
                const auto x=spark.left+static_cast<LONG>((sample.time-(latest-60000))*
                    (spark.right-spark.left)/60000.0);
                const double actual=*sample.values[4];
                const double displayed=(!second_gpu_line.empty() &&
                    &sample==&system_points_.back())?
                    eased_latest_value(previous_vram,actual,
                        animation_started_,animation_duration_ms_):actual;
                const auto ypoint=spark.bottom-2-static_cast<LONG>(
                    std::clamp(displayed/100.0,0.0,1.0)*(spark.bottom-spark.top-5));
                second_gpu_line.push_back({x,ypoint});
                previous_vram=actual;
            }
            if(section==3 && !system_points_.empty() && system_points_.back().values[4])
                extend_recent_line(second_gpu_line,spark.right,system_points_.back().time,
                    latest,animation_duration_ms_);
            // The CPU sidebar mirrors GPU: two independent curves share the
            // same compact renderer. CPU load and package temperature use
            // separate 0..100 scales and no additional sensor reads.
            if(section==0) {
                std::optional<double> previous_temperature;
                for(const auto& sample:system_points_)if(sample.cpu_temperature &&
                    sample.time>=latest-60000 && sample.time<=latest) {
                    const double actual=*sample.cpu_temperature;
                    const double displayed=(&sample==&system_points_.back() && previous_temperature)?
                        eased_latest_value(*previous_temperature,actual,
                            animation_started_,animation_duration_ms_):actual;
                    const auto x=spark.left+static_cast<LONG>(
                        (sample.time-(latest-60000))*(spark.right-spark.left)/60000.0);
                    const auto ypoint=spark.bottom-2-static_cast<LONG>(
                        std::clamp(displayed/100.0,0.0,1.0)*(spark.bottom-spark.top-5));
                    second_cpu_temperature_line.push_back({x,ypoint});
                    previous_temperature=actual;
                }
                if(!system_points_.empty() && system_points_.back().cpu_temperature)
                    extend_recent_line(second_cpu_temperature_line,spark.right,
                        system_points_.back().time,latest,animation_duration_ms_);
            }
        } else if(section==4 || section==5) {
            const auto& apps=section==4?live_apps_:history_.history;
            const auto start=section==4?latest-60000:window_from_;
            const auto end=section==4?latest:window_to_;
            if(!apps.empty())for(const auto& point:apps.front().points)
                add(point.first,point.second,start,end,100.0);
        } else if(section==6 && !history_.uptime.empty()) {
            double max_time=1;
            for(const auto& app:history_.uptime)max_time=std::max(max_time,app.observed_seconds);
            const std::size_t count=std::min<std::size_t>(history_.uptime.size(),10);
            for(std::size_t i=0;i<count;++i) {
                const auto x=spark.left+static_cast<int>(i)*((spark.right-spark.left)/10);
                const auto height=static_cast<int>((spark.bottom-spark.top-5)*history_.uptime[i].observed_seconds/max_time);
                RECT bar{x,spark.bottom-height,x+12,spark.bottom};
                bars.push_back({bar,colors[section]});
            }
        }
        // Apply the same short-gap rule to live sidebar series; uptime and
        // historical mini-charts retain their exact data boundaries.
        if(section<5) {
            extend_fresh_left_edge(points,spark,latest-60000,latest,animation_duration_ms_);
            if(section==3)
                extend_fresh_left_edge(second_gpu_line,spark,latest-60000,latest,animation_duration_ms_);
            if(section==0)
                extend_fresh_left_edge(second_cpu_temperature_line,spark,latest-60000,latest,animation_duration_ms_);
        }
        // Mini-charts use the very same renderer, with a white paper and no grid.
        // GPU and CPU pairs use the shared painter (both fills below both strokes).
        std::vector<ChartStroke> plots;
        plots.reserve(section==0 || section==3?2U:1U);
        if(points.size()>1)plots.push_back({std::move(points),colors[section]});
        if(section==0 && second_cpu_temperature_line.size()>1)
            plots.push_back({std::move(second_cpu_temperature_line),RGB(235,75,43)});
        if(section==3 && second_gpu_line.size()>1)
            plots.push_back({std::move(second_gpu_line),RGB(194,89,178)});
        ChartStyle style{};
        style.area=spark;style.paper=RGB(255,255,255);
        style.fill_opacity=26;style.point_markers=false;
        style.right_edge_overscan=section<5?spark_tail_guard_px:0;
        draw_chart_core(dc,style,plots,bars);
    }
    for(std::size_t i=0;i<disk_letters_.size();++i) {
        const wchar_t letter=disk_letters_[i];
        const unsigned index=static_cast<unsigned>(letter-L'A');
        const int y=18+(static_cast<int>(statistics_sidebar_disk_row(static_cast<unsigned>(i)))
            -static_cast<int>(sidebar_scroll_))*114;
        if(y>=830 || y+105<=18)continue;
        const RECT item{14,y,266,y+105};
        const bool chosen=selected_disk_letter_==letter;
        HBRUSH face=CreateSolidBrush(chosen?RGB(225,233,241):RGB(245,246,247));
        FillRect(dc,&item,face);DeleteObject(face);
        HBRUSH border=CreateSolidBrush(chosen?RGB(52,115,156):RGB(206,211,214));
        FrameRect(dc,&item,border);DeleteObject(border);
        const std::wstring name=metric_text::disk_name(letter);
        label(dc,name.c_str(),26,y+9,RGB(32,44,52));
        const auto latest_value=system_points_.empty()?std::optional<double>{}:system_points_.back().disks[index];
        const auto reading=metric_text::disk_activity(latest_value);
        RECT box{92,y+7,250,y+30};
        SetTextColor(dc,RGB(42,132,108));SetBkMode(dc,TRANSPARENT);
        DrawTextW(dc,reading.c_str(),-1,&box,DT_RIGHT|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS);
        const RECT spark{26,y+38,249,y+94};
        std::vector<POINT> points;
        std::optional<double> before;
        for(const auto& point:system_points_) {
            if(point.time<latest-60000 || point.time>latest || !point.disks[index])continue;
            const double measured=*point.disks[index];
            const double displayed=&point==&system_points_.back() && before?
                eased_latest_value(*before,measured,animation_started_,animation_duration_ms_):measured;
            const LONG x=spark.left+static_cast<LONG>((point.time-(latest-60000))*
                (spark.right-spark.left)/60000.0);
            const LONG yy=spark.bottom-2-static_cast<LONG>(std::clamp(displayed/100.0,0.0,1.0)*
                (spark.bottom-spark.top-5));
            points.push_back({x,yy});before=measured;
        }
        if(!system_points_.empty() && system_points_.back().disks[index])
            extend_recent_line(points,spark.right,system_points_.back().time,latest,animation_duration_ms_);
        extend_fresh_left_edge(points,spark,latest-60000,latest,animation_duration_ms_);
        std::vector<ChartStroke> plots;
        if(points.size()>1)plots.push_back({std::move(points),RGB(42,132,108)});
        ChartStyle style{};style.area=spark;style.paper=RGB(255,255,255);
        style.fill_opacity=26;style.point_markers=false;
        style.right_edge_overscan=spark_tail_guard_px;
        draw_chart_core(dc,style,plots);
    }
}
void StatisticsPage::draw_system_graph(HDC dc,const RECT& area,unsigned metric,
    std::int64_t from,std::int64_t to,bool autoscale,double manual_zoom,POINT hover,
    const std::deque<SystemPoint>* frozen,bool detached) {
    const auto& samples=frozen?*frozen:system_points_;
    (void)hover; // The old on-graph readings have moved to the native hover bubble.
    if(metric>=32)return;
    constexpr std::array<COLORREF,5> ink{{RGB(14,132,177),RGB(67,105,181),RGB(184,55,114),
        RGB(131,54,156),RGB(194,89,178)}};
    const unsigned count=1U;
    double max_y=metric==2?1.0:100.0;
    if(autoscale) {
        max_y=metric==2?0.1:5.0;
        for(const auto& sample:samples)if(sample.time>=from && sample.time<=to)
            for(unsigned s=0;s<count;++s) {
                const auto value=metric+s==31?sample.cpu_temperature:
                    (metric+s<5?sample.values[metric+s]:sample.disks[metric+s-5]);
                if(value)max_y=std::max(max_y,*value*(metric==2?1.2:1.12));
            }
        if(metric!=2)max_y=std::min(100.0,std::ceil(max_y/5.0)*5.0);
    } else max_y/=std::clamp(manual_zoom,1.0,100.0);
    max_y=std::max(max_y,.001);
    if(autoscale && !frozen) {
        const unsigned context=detached?1U:0U;
        auto& shown=smooth_system_axis_max_[context][metric];
        auto& previous_tick=smooth_system_axis_tick_[context][metric];
        const auto tick=GetTickCount64();
        if(shown<=0.0 || !std::isfinite(shown) || previous_tick==0 ||
            tick-previous_tick>1000ULL)shown=max_y;
        else if(tick>previous_tick) {
            const double ms=std::min(100.0,static_cast<double>(tick-previous_tick));
            const double blend=1.0-std::exp(-ms/240.0);
            shown+=(max_y-shown)*blend;
            if(std::abs(shown-max_y)<.01)shown=max_y;
        }
        previous_tick=tick;
        max_y=shown;
    }
    if(to<=from)to=from+1;
    // System metrics only prepare contiguous pixel segments. Missing readings
    // terminate segments rather than bridging gaps with fabricated values.
    std::vector<ChartStroke> plots;
    plots.reserve(8);
    for(unsigned s=0;s<count;++s) {
        const unsigned index=metric+s;
        std::vector<POINT> line;
        std::optional<double> previous_measurement;
        std::int64_t segment_end=0;
        const auto finish_segment=[&]() {
            if(line.empty())return;
            if(!frozen) {
                extend_fresh_left_edge(line,area,from,to,animation_duration_ms_);
                if(!samples.empty() && segment_end==samples.back().time)
                    extend_recent_line(line,area.right,segment_end,to,animation_duration_ms_);
            }
            plots.push_back({std::move(line),index==31?RGB(211,93,59):(index<ink.size()?ink[index]:RGB(42,132,108))});
            line.clear();
            previous_measurement.reset();
            segment_end=0;
        };
        for(const auto& sample:samples) {
            const auto value=index==31?sample.cpu_temperature:
                (index<5?sample.values[index]:sample.disks[index-5]);
            if(sample.time<from || sample.time>to || !value) {
                finish_segment();continue;
            }
            const double fraction=static_cast<double>(sample.time-from)/static_cast<double>(to-from);
            const double actual=*value;
            double displayed=actual;
            if(!frozen && !samples.empty() && &sample==&samples.back() && previous_measurement)
                displayed=eased_latest_value(*previous_measurement,actual,
                    animation_started_,animation_duration_ms_);
            const double normalized=std::clamp(displayed/max_y,0.0,1.0);
            line.push_back({area.left+static_cast<LONG>(fraction*(area.right-area.left)),
                area.bottom-static_cast<LONG>(normalized*(area.bottom-area.top))});
            previous_measurement=actual;
            segment_end=sample.time;
        }
        finish_segment();
    }
    ChartStyle style{};
    style.area=area;style.from=from;style.to=to;
    style.horizontal_rows=5;style.time_divisions=10;
    style.right_edge_overscan=!frozen && to>=system_view_end()-1000?live_tail_guard_px:0;
    draw_chart_core(dc,style,plots);
    const auto cap=stat_decimal(max_y)+(metric==2?L" Mbps":(metric==31?L" °C":L"%"));
    label(dc,cap.c_str(),area.left,area.top-24,RGB(76,81,84));
    label(dc,L"0",area.left,area.bottom+4,RGB(76,81,84));
    const auto a=local_label(from),b=local_label(to);
    label(dc,a.c_str(),area.left,area.bottom+24);
    SIZE extent{};GetTextExtentPoint32W(dc,b.c_str(),static_cast<int>(b.size()),&extent);
    label(dc,b.c_str(),area.right-extent.cx,area.bottom+24);
    if(samples.empty())label(dc,L"Ожидание показаний Windows…",area.left+20,area.top+50,RGB(120,119,116));
}

void StatisticsPage::draw_system_details(HDC dc,unsigned metric) {
    if(metric>=4)return;
    const int top=731;
    const auto& held=frozen_views_[metric];
    const auto& info=held.paused?held.info:system_info_;
    const auto& samples=held.paused?held.samples:system_points_;
    const auto& sample=samples.empty()?SystemPoint{}:samples.back();
    const auto percent=[&](unsigned index)->std::wstring {
        return sample.values[index]?stat_decimal(*sample.values[index])+L"%":L"—";
    };
    std::wstring one,two,three;
    if(metric==0){
        one=L"Загрузка ЦП: "+percent(0)+L"   Логических процессоров: "+std::to_wstring(info.logical_processors);
        two=L"Длительность опроса: "+stat_decimal(info.collection_ms)+L" мс";
        three=L"Температура ЦП: "+metric_text::temperature(info.cpu_temp)+(info.cpu_temp?L" ("+metric_text::cpu_sensor_name(info.cpu_source,info.cpu_board)+L")":L"");
    } else if(metric==1) {
        one=L"Используется RAM: "+stat_memory(info.ram_used)+L" / "+stat_memory(info.ram_capacity)+L"   "+percent(1);
        const std::optional<std::uint64_t> free_mem=info.ram_capacity && info.ram_used &&
            *info.ram_capacity>=*info.ram_used?std::optional<std::uint64_t>(*info.ram_capacity-*info.ram_used):std::nullopt;
        two=L"Доступно: "+stat_memory(free_mem);
        three=L"Системная память Windows (не сумма Working Set приложений)";
    } else if(metric==2) {
        one=L"Сеть · Rx/Tx: "+stat_network(info.rx,info.tx);
        two=L"Приём: "+(info.rx?stat_decimal(*info.rx*8.0/1000000.0)+L" Mbps":L"—");
        three=L"Передача: "+(info.tx?stat_decimal(*info.tx*8.0/1000000.0)+L" Mbps":L"—");
    } else {
        one=L"GPU: "+percent(3)+L"   Использование VRAM: "+percent(4);
        two=L"Видеопамять: "+stat_memory(info.gpu_memory)+L" / "+stat_memory(info.gpu_capacity);
        three=L"Температура GPU: "+metric_text::temperature(info.gpu_temp);
    }
    label(dc,one.c_str(),310,top,RGB(45,52,60));
    label(dc,two.c_str(),310,top+29,RGB(65,72,80));
    label(dc,three.c_str(),310,top+58,RGB(65,72,80));
    if(metric==3){
        const HBRUSH gpu=CreateSolidBrush(RGB(131,54,156));
        const RECT gpu_mark{1035,top,1047,top+12};FillRect(dc,&gpu_mark,gpu);DeleteObject(gpu);
        const HBRUSH vram=CreateSolidBrush(RGB(194,89,178));
        const RECT vram_mark{1035,top+25,1047,top+37};FillRect(dc,&vram_mark,vram);DeleteObject(vram);
        label(dc,L"GPU",1052,top-4);label(dc,L"VRAM",1052,top+21);
    }
}

void StatisticsPage::refresh_pause_control() {
    if(!pause_toggle_ || (!selected_disk_letter_ && selected_section_>=6))return;
    const unsigned index=selected_disk_letter_
        ?6U+static_cast<unsigned>(selected_disk_letter_-L'A'):selected_section_;
    SetWindowTextW(pause_toggle_,frozen_views_[index].paused?L"Продолжить":L"Пауза");
}
void StatisticsPage::toggle_section_pause() {
    const bool disk=selected_disk_letter_!=0;
    if(!disk && selected_section_>=6)return;
    const unsigned index=disk?6U+static_cast<unsigned>(selected_disk_letter_-L'A'):selected_section_;
    auto& state=frozen_views_[index];
    if(state.paused) {
        state=FrozenView{};
    } else {
        state.paused=true;
        if(disk || selected_section_<4) {
            state.samples=system_points_; // Immutable copy: future sampler ticks cannot move this chart.
            state.info=system_info_;
            state.to=system_view_end();
            state.from=state.to-chart_system_duration(system_range_);
        } else {
            state.apps=selected_section_==4?live_apps_:history_.history;
            const auto axis=selected_section_-4;
            state.zoom=chart_effective_zoom(auto_scale_[axis],cached_auto_zoom_[axis],1.0);
            state.to=selected_section_==4?live_view_end():window_to_;
            state.from=selected_section_==4?state.to-60000:window_from_;
        }
    }
    refresh_pause_control();
    system_tip_active_=false;hover_section_=-1;
    if(tooltip_)ShowWindow(tooltip_,SW_HIDE);
    InvalidateRect(hwnd_,nullptr,FALSE);
}
void StatisticsPage::draw(HDC dc) {
    RECT physical{};GetClientRect(hwnd_,&physical);
    SetMapMode(dc,MM_ANISOTROPIC);SetWindowExtEx(dc,page_width,page_height,nullptr);
    SetViewportExtEx(dc,std::max<LONG>(1,physical.right),std::max<LONG>(1,physical.bottom),nullptr);
    const RECT background{0,0,page_width,page_height};
    FillRect(dc,&background,GetSysColorBrush(COLOR_BTNFACE));
    if(!paint_font_)paint_font_=CreateFontW(-16,0,0,0,FW_SEMIBOLD,0,0,0,DEFAULT_CHARSET,
        0,0,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
    const auto old=SelectObject(dc,paint_font_?paint_font_:GetStockObject(DEFAULT_GUI_FONT));
    draw_sidebar(dc);
    const std::array<const wchar_t*,7> titles{{L"ЦП",L"ПАМЯТЬ",L"СЕТЬ",L"ГРАФИЧЕСКИЙ ПРОЦЕССОР",
        L"ПОСЛЕДНЯЯ МИНУТА · AutoScore",L"ИСТОРИЯ · AutoScore",L"НАБЛЮДАЕМЫЙ АПТАЙМ"}};
    if(selected_disk_letter_) {
        const auto label_text=metric_text::disk_name(selected_disk_letter_)+L" · АКТИВНОСТЬ, %";
        label(dc,label_text.c_str(),292,18,RGB(40,47,55));
    } else label(dc,titles[selected_section_],292,18,RGB(40,47,55));
    // Only one graph is visible; the original range, auto-scale and popout controls
    // remain above the graph and keep their existing settings and handlers.
    const auto now=live_view_end();
    if(selected_disk_letter_) {
        const auto disk_index=static_cast<unsigned>(selected_disk_letter_-L'A');
        const auto& held=frozen_views_[6U+disk_index];
        const auto range=chart_system_duration(system_range_);
        const auto right=held.paused?held.to:system_view_end();
        const auto begin=chart_window(held.paused,{held.from,held.to},{right-range,right}).from;
        const auto* samples=held.paused?&held.samples:nullptr;
        draw_system_graph(dc,one_minute,5U+disk_index,
            begin,right,system_autoscale_,1.0,system_hover_,samples);
        const auto& series=held.paused?held.samples:system_points_;
        const auto reading=series.empty()?std::optional<double>{}:
            series.back().disks[disk_index];
        const auto& disk_temperature=series.empty()?std::optional<double>{}:series.back().disk_temperatures[disk_index];
        const auto detail=L"Активность логического диска "+metric_text::disk_name(selected_disk_letter_)+
            L": "+metric_text::disk_activity(reading)+L"   Температура: "+metric_text::temperature(disk_temperature);
        label(dc,detail.c_str(),310,731,RGB(45,52,60));
    } else if(selected_section_<4) {
        const auto range=chart_system_duration(system_range_);
        const auto& held=frozen_views_[selected_section_];
        const auto right=held.paused?held.to:system_view_end();
        const auto begin=chart_window(held.paused,{held.from,held.to},{right-range,right}).from;
        const auto* samples=held.paused?&held.samples:nullptr;
        if(selected_section_==0) {
            const RECT cpu{310,190,1364,372};
            const RECT temperature{310,458,1364,640};
            label(dc,L"Загрузка CPU",438,162,RGB(14,132,177));
            label(dc,L"Температура CPU",438,430,RGB(211,93,59));
            draw_system_graph(dc,cpu,0,begin,right,system_autoscale_,1.0,system_hover_,samples);
            draw_system_graph(dc,temperature,31,begin,right,system_autoscale_,1.0,system_hover_,samples);
        } else if(selected_section_==3) {
            const RECT gpu{310,190,1364,372};
            const RECT vram{310,458,1364,640};
            label(dc,L"Загрузка GPU",438,162,RGB(131,54,156));
            label(dc,L"Использование VRAM",438,430,RGB(194,89,178));
            draw_system_graph(dc,gpu,3,begin,right,system_autoscale_,1.0,system_hover_,samples);
            draw_system_graph(dc,vram,4,begin,right,system_autoscale_,1.0,system_hover_,samples);
        } else draw_system_graph(dc,one_minute,selected_section_,begin,right,
            system_autoscale_,1.0,system_hover_,samples);
        draw_system_details(dc,selected_section_);
    } else if(selected_section_==4) {
        const auto& held=frozen_views_[4];
        const auto& series=held.paused?held.apps:live_apps_;
        const auto end=held.paused?held.to:now;
        const auto begin=held.paused?held.from:end-60000;
        draw_graph(dc,one_minute,series,begin,end,L"",!held.paused,
            held.paused?held.zoom:(auto_scale_[0]?cached_auto_zoom_[0]:1.0));
    } else if(selected_section_==5) {
        const auto& held=frozen_views_[5];
        draw_graph(dc,history_chart,held.paused?held.apps:history_.history,
            held.paused?held.from:window_from_,held.paused?held.to:window_to_,L"",false,
            held.paused?held.zoom:(auto_scale_[1]?cached_auto_zoom_[1]:1.0));
        if(!history_.error.empty())label(dc,history_.error.c_str(),292,819,RGB(162,50,40));
    } else label(dc,L"Таблица аптайма сохраняет независимые фильтры и период.",292,813,RGB(100,105,108));
    SelectObject(dc,old);
}
void StatisticsPage::mouse(int x,int y) {
    RECT physical{};GetClientRect(hwnd_,&physical);
    if(physical.right<=0 || physical.bottom<=0) return;
    x=MulDiv(x,page_width,physical.right);y=MulDiv(y,page_height,physical.bottom);
    system_hover_={x,y};
    if(selected_disk_letter_) {
        const auto disk_index=static_cast<unsigned>(selected_disk_letter_-L'A');
        const auto& held=frozen_views_[6U+disk_index];
        const auto range=chart_system_duration(system_range_);
        const auto right=held.paused?held.to:system_view_end();
        const auto begin=chart_window(held.paused,{held.from,held.to},{right-range,right}).from;
        show_system_hover(5U+disk_index,system_hover_,one_minute,begin,right,
            held.paused?&held.samples:nullptr);
        return;
    }
    if(selected_section_<4) {
        const auto range=chart_system_duration(system_range_);
        const auto& held=frozen_views_[selected_section_];
        const auto right=held.paused?held.to:system_view_end();
        const auto begin=chart_window(held.paused,{held.from,held.to},{right-range,right}).from;
        const auto* samples=held.paused?&held.samples:nullptr;
        if(selected_section_==0) {
            const RECT cpu{310,190,1364,372};
            const RECT temperature{310,458,1364,640};
            if(y>=temperature.top && y<=temperature.bottom)
                show_system_hover(31,system_hover_,temperature,begin,right,samples);
            else show_system_hover(0,system_hover_,cpu,begin,right,samples);
        } else if(selected_section_==3) {
            const RECT gpu{310,190,1364,372};
            const RECT vram{310,458,1364,640};
            if(y>=vram.top && y<=vram.bottom)
                show_system_hover(4,system_hover_,vram,begin,right,samples);
            else show_system_hover(3,system_hover_,gpu,begin,right,samples);
        } else show_system_hover(selected_section_,system_hover_,one_minute,begin,right,samples);
        return;
    }
    system_tip_active_=false;
    const bool in_live=selected_section_==4 && x>=one_minute.left && x<=one_minute.right
        && y>=one_minute.top && y<=one_minute.bottom;
    const bool in_history=selected_section_==5 && x>=history_chart.left && x<=history_chart.right
        && y>=history_chart.top && y<=history_chart.bottom;
    if(!in_live && !in_history) {
        if(hover_section_!=-1) {hover_section_=-1;if(tooltip_) ShowWindow(tooltip_,SW_HIDE);}
        return;
    }
    const auto& held=frozen_views_[in_live?4U:5U];
    const auto& chart=held.paused?held.apps:(in_live?live_apps_:history_.history);
    const auto& area=in_live?one_minute:history_chart;
    const auto now=live_view_end();
    const auto from=held.paused?held.from:(in_live?now-60000:window_from_);
    const auto to=held.paused?held.to:(in_live?now:window_to_);
    const double zoom=held.paused?held.zoom:
        chart_effective_zoom(auto_scale_[in_live?0:1],cached_auto_zoom_[in_live?0:1],1.0);
    const double graph_width=static_cast<double>(area.right-area.left)+
        (in_live && !held.paused && to>=live_view_end()-1000?live_tail_guard_px:0);
    double best=9.0*9.0;
    int section=-1;std::size_t selected=0;double score=0;std::int64_t sample_at=0;
    if(to>from) for(std::size_t i=0;i<chart.size();++i) {
        const auto& pts=chart[i].points;
        for(std::size_t j=0;j<pts.size();++j) {
            const double xpos=area.left+static_cast<double>(pts[j].first-from)/static_cast<double>(to-from)*graph_width;
            if(pts[j].first>=from && pts[j].first<=to) {
                const double ypos=area.bottom-std::clamp(pts[j].second*zoom,0.0,100.0)/100*(area.bottom-area.top);
                const double dist=(xpos-x)*(xpos-x)+(ypos-y)*(ypos-y);
                if(dist<best) {best=dist;section=in_live?0:1;selected=i;score=pts[j].second;sample_at=pts[j].first;}
            }
            if(j==0) continue;
            const auto& prev=pts[j-1];
            const double old_x=area.left+static_cast<double>(prev.first-from)/static_cast<double>(to-from)*graph_width;
            if(xpos<=old_x || x<old_x || x>xpos) continue;
            const double t=(x-old_x)/(xpos-old_x);
            const double line_score=prev.second+(pts[j].second-prev.second)*t;
            const double line_y=area.bottom-std::clamp(line_score*zoom,0.0,100.0)/100*(area.bottom-area.top);
            const double d=(line_y-y)*(line_y-y);
            if(d<best){best=d;section=in_live?0:1;selected=i;score=line_score;sample_at=prev.first+static_cast<std::int64_t>(static_cast<double>(pts[j].first-prev.first)*t);}
        }
    }
    const bool content_changed=hover_section_!=section || hover_index_!=selected ||
        hover_timestamp_!=sample_at || std::abs(hover_value_-score)>0.01;
    if(content_changed || hover_at_.x!=x || hover_at_.y!=y) {
        hover_section_=section;hover_index_=selected;hover_value_=score;hover_timestamp_=sample_at;hover_at_={x,y};
        if(section<0 && tooltip_)ShowWindow(tooltip_,SW_HIDE);
        else if(tooltip_) {
            POINT screen{};GetCursorPos(&screen);
            const bool newly_shown=position_tooltip(screen,18,20);
            if(content_changed || newly_shown)InvalidateRect(tooltip_,nullptr,FALSE);
        }
    }
}
bool StatisticsPage::advance_axes(double elapsed_ms) {
    bool animating=false;
    for(std::size_t index=0;index<2;++index) {
        if(!auto_scale_[index]) continue;
        const double target=target_auto_zoom_[index];
        cached_auto_zoom_[index]=eased_axis(cached_auto_zoom_[index],target,elapsed_ms);
        if(std::abs(cached_auto_zoom_[index]-target)>0.002) animating=true;
    }
    for(auto& popup:popups_) {
        if(!popup.window || !popup.auto_scale) continue;
        popup.cached_auto_zoom=eased_axis(popup.cached_auto_zoom,popup.target_auto_zoom,elapsed_ms);
        if(std::abs(popup.cached_auto_zoom-popup.target_auto_zoom)>0.002) animating=true;
    }
    return animating;
}
void StatisticsPage::draw_metric_tooltip(HDC dc,const RECT& area,const StatisticsApplication& app,double score,std::int64_t sample_at) {
    // FluxTop unified tooltip with current metrics v1; shared across graph windows.
    const int left=area.left, top=area.top;
    const double scale=std::max(0.6,static_cast<double>(area.right-area.left)/420.0);
    const auto unit=[&](int n){return static_cast<int>(std::lround(n*scale));};
    HBRUSH paper=CreateSolidBrush(RGB(250,248,239));
    FillRect(dc,&area,paper);DeleteObject(paper);
    HBRUSH edge=CreateSolidBrush(RGB(151,137,112));
    FrameRect(dc,&area,edge);DeleteObject(edge);
    RECT accent{left+unit(1),area.bottom-unit(5),area.right-unit(1),area.bottom-unit(1)};
    HBRUSH color=CreateSolidBrush(chart_color(app));FillRect(dc,&accent,color);DeleteObject(color);
    if(HICON icon=application_icon(app.path))
        DrawIconEx(dc,left+unit(14),top+unit(24),icon,unit(34),unit(34),0,nullptr,DI_NORMAL);
    const std::array<int,3> font_heights{unit(19),unit(16),unit(14)};
    for(std::size_t i=0;i<font_heights.size();++i) {
        if(tooltip_fonts_[i] && tooltip_font_heights_[i]==font_heights[i]) continue;
        HFONT replacement=CreateFontW(-font_heights[i],0,0,0,i==2?FW_NORMAL:FW_SEMIBOLD,
            FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY,DEFAULT_PITCH,i==2?L"Segoe UI":L"Segoe Print");
        if(replacement) {
            if(tooltip_fonts_[i]) DeleteObject(tooltip_fonts_[i]);
            tooltip_fonts_[i]=replacement;tooltip_font_heights_[i]=font_heights[i];
        }
    }
    const HFONT title_font=tooltip_fonts_[0];
    const HFONT score_font=tooltip_fonts_[1];
    const HFONT info_font=tooltip_fonts_[2];
    const HGDIOBJ original=SelectObject(dc,title_font?title_font:GetStockObject(DEFAULT_GUI_FONT));
    SetBkMode(dc,TRANSPARENT);SetTextColor(dc,RGB(53,49,43));
    RECT name{left+unit(59),top+unit(8),area.right-unit(12),top+unit(39)};
    DrawTextW(dc,app.name.c_str(),-1,&name,DT_LEFT|DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS|DT_NOPREFIX);
    SelectObject(dc,score_font?score_font:GetStockObject(DEFAULT_GUI_FONT));
    wchar_t reading[80]{};swprintf_s(reading,L"AutoScore: %.1f / 100",score);
    SetTextColor(dc,chart_color(app));
    RECT rating{left+unit(59),top+unit(38),area.right-unit(12),top+unit(66)};
    DrawTextW(dc,reading,-1,&rating,DT_LEFT|DT_SINGLELINE|DT_VCENTER|DT_NOPREFIX);
    SelectObject(dc,info_font?info_font:GetStockObject(DEFAULT_GUI_FONT));
    SetTextColor(dc,RGB(75,71,64));
    const auto found=current_readings_.find(color_key(app));
    const CurrentReadings empty{};
    const auto& values=found==current_readings_.end()?empty:found->second;
    RECT separator{left+unit(13),top+unit(101),area.right-unit(13),top+unit(102)};
    HBRUSH rule=CreateSolidBrush(RGB(221,215,199));FillRect(dc,&separator,rule);DeleteObject(rule);
    const auto line=[&](const std::wstring& content,int y){
        RECT cell{left+unit(16),top+unit(y),area.right-unit(12),top+unit(y+25)};
        DrawTextW(dc,content.c_str(),-1,&cell,DT_LEFT|DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS|DT_NOPREFIX);
    };
    // The timestamp belongs to the hovered chart point; resource readings are current.
    line(L"Замер: "+local_sample_label(sample_at),73);
    line(L"Сейчас · CPU: "+values.cpu+L"    GPU: "+values.gpu,106);
    line(L"RAM: "+values.ram+L"       VRAM: "+values.vram,132);
    line(L"Rx/Tx: "+values.network,158);
    SelectObject(dc,original);
}
bool StatisticsPage::position_tooltip(POINT screen,int offset_x,int offset_y) {
    if(!tooltip_)return false;
    const UINT dpi=GetDpiForWindow(tooltip_);
    const int width=MulDiv(420,static_cast<int>(dpi),96);
    const int height=MulDiv(198,static_cast<int>(dpi),96);
    int x=screen.x+MulDiv(offset_x,static_cast<int>(dpi),96);
    int y=screen.y+MulDiv(offset_y,static_cast<int>(dpi),96);
    MONITORINFO monitor{sizeof(monitor)};
    if(GetMonitorInfoW(MonitorFromPoint(screen,MONITOR_DEFAULTTONEAREST),&monitor)) {
        x=std::clamp(x,static_cast<int>(monitor.rcWork.left),
            std::max(static_cast<int>(monitor.rcWork.left),static_cast<int>(monitor.rcWork.right)-width));
        y=std::clamp(y,static_cast<int>(monitor.rcWork.top),
            std::max(static_cast<int>(monitor.rcWork.top),static_cast<int>(monitor.rcWork.bottom)-height));
    }
    const bool visible=IsWindowVisible(tooltip_)!=FALSE;
    RECT previous{};
    const bool measured=GetWindowRect(tooltip_,&previous)!=FALSE;
    if(!visible || !measured || previous.left!=x || previous.top!=y ||
        previous.right-previous.left!=width || previous.bottom-previous.top!=height)
        SetWindowPos(tooltip_,HWND_TOPMOST,x,y,width,height,
            SWP_NOACTIVATE|SWP_NOZORDER|(visible?0U:SWP_SHOWWINDOW));
    return !visible;
}
void StatisticsPage::show_system_hover(unsigned metric,POINT cursor,const RECT& area,
    std::int64_t from,std::int64_t to,const std::deque<SystemPoint>* frozen) {
    if(metric>=32 || cursor.x<area.left || cursor.x>area.right || cursor.y<area.top ||
        cursor.y>area.bottom || to<=from || !tooltip_) {
        system_tip_active_=false;
        if(tooltip_ && hover_section_<0)ShowWindow(tooltip_,SW_HIDE);
        return;
    }
    const int guard=!frozen && to>=system_view_end()-1000?live_tail_guard_px:0;
    const double original_x=statistics_chart_unproject_x(
        static_cast<double>(cursor.x),static_cast<double>(area.left),
        static_cast<double>(area.right),static_cast<double>(guard));
    const auto time=from+static_cast<std::int64_t>((original_x-area.left)*
        static_cast<double>(to-from)/std::max<LONG>(1,area.right-area.left));
    const SystemPoint* closest=nullptr;
    const auto& samples=frozen?*frozen:system_points_;
    for(const auto& sample:samples) {
        const auto reading=metric==31?sample.cpu_temperature:
            (metric<5?sample.values[metric]:sample.disks[metric-5]);
        if(sample.time<from || sample.time>to || !reading)continue;
        if(!closest || std::abs(sample.time-time)<std::abs(closest->time-time))closest=&sample;
    }
    if(!closest){system_tip_active_=false;ShowWindow(tooltip_,SW_HIDE);return;}
    const bool content_changed=!system_tip_active_ || system_tip_metric_!=metric ||
        system_tip_sample_.time!=closest->time || system_tip_sample_.values!=closest->values || system_tip_sample_.disks!=closest->disks || system_tip_sample_.cpu_temperature!=closest->cpu_temperature;
    system_tip_active_=true;system_tip_metric_=metric;system_tip_sample_=*closest;
    POINT screen{};GetCursorPos(&screen);
    const bool newly_shown=position_tooltip(screen,16,18);
    if(content_changed || newly_shown)InvalidateRect(tooltip_,nullptr,FALSE);
}
void StatisticsPage::draw_system_hover(HDC dc,const RECT& bounds){
    const std::array<COLORREF,5> colors{{RGB(14,132,177),RGB(67,105,181),RGB(184,55,114),RGB(131,54,156),RGB(194,89,178)}};
    const std::array<const wchar_t*,5> names{{L"ЦП",L"Память",L"Сеть",L"GPU",L"VRAM"}};
    const HBRUSH paper=CreateSolidBrush(RGB(250,248,239));FillRect(dc,&bounds,paper);DeleteObject(paper);
    const HBRUSH edge=CreateSolidBrush(RGB(151,137,112));FrameRect(dc,&bounds,edge);DeleteObject(edge);
    const COLORREF color=system_tip_metric_==31?RGB(211,93,59):(system_tip_metric_>=5?RGB(42,132,108):colors[std::min<unsigned>(4,system_tip_metric_)]);
    const HBRUSH accent=CreateSolidBrush(color);
    const RECT bar{bounds.left+2,bounds.bottom-6,bounds.right-2,bounds.bottom-2};
    FillRect(dc,&bar,accent);DeleteObject(accent);
    SetBkMode(dc,TRANSPARENT);SetTextColor(dc,RGB(53,49,43));
    RECT header{bounds.left+18,bounds.top+9,bounds.right-16,bounds.top+39};
    const auto title=system_tip_metric_==31?std::wstring(L"Температура ЦП"):(system_tip_metric_>=5?
        metric_text::disk_name(static_cast<wchar_t>(L'A'+system_tip_metric_-5)):
        std::wstring(names[system_tip_metric_]));
    DrawTextW(dc,title.c_str(),-1,&header,DT_SINGLELINE|DT_VCENTER|DT_NOPREFIX);
    const auto value=[&](unsigned index,const wchar_t* unit)->std::wstring {
        const auto reading=index==31?system_tip_sample_.cpu_temperature:(index<5?system_tip_sample_.values[index]:system_tip_sample_.disks[index-5]);
        return reading?stat_decimal(*reading)+unit:L"—";
    };
    std::wstring primary=value(system_tip_metric_,system_tip_metric_==2?L" Mbps":(system_tip_metric_==31?L" °C":L"%"));
    if(system_tip_metric_==3)primary+=L"   VRAM: "+value(4,L"%");
    SetTextColor(dc,color);
    RECT main{bounds.left+18,bounds.top+41,bounds.right-12,bounds.top+75};
    DrawTextW(dc,primary.c_str(),-1,&main,DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS|DT_NOPREFIX);
    const HBRUSH separator=CreateSolidBrush(RGB(221,215,199));
    const RECT rule{bounds.left+13,bounds.top+101,bounds.right-13,bounds.top+102};
    FillRect(dc,&rule,separator);DeleteObject(separator);
    SetTextColor(dc,RGB(75,71,64));
    const auto write=[&](const std::wstring& text,int top){
        RECT cell{bounds.left+18,bounds.top+top,bounds.right-14,bounds.top+top+25};
        DrawTextW(dc,text.c_str(),-1,&cell,DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS|DT_NOPREFIX);
    };
    write(L"Замер: "+local_sample_label(system_tip_sample_.time),75);
    if(system_tip_metric_==31) {
        write(L"CPU Package / резервный датчик платы",108);
    } else if(system_tip_metric_>=5) {
        write(L"Активность логического диска, %: "+metric_text::disk_activity(system_tip_sample_.disks[system_tip_metric_-5]),108);
        // Disk utilization already appears above; no redundant PID disclaimer.
    } else if(system_tip_metric_==0) {
        write(L"ЦП: "+value(0,L"%")+L" · Потоков: "+std::to_wstring(system_info_.logical_processors),108);
        write(L"Опрос: "+stat_decimal(system_info_.collection_ms)+L" мс",135);
    } else if(system_tip_metric_==1) {
        write(L"RAM: "+stat_memory(system_info_.ram_used)+L" / "+stat_memory(system_info_.ram_capacity),108);
        write(L"Использование: "+value(1,L"%"),135);
    } else if(system_tip_metric_==2) {
        write(L"Rx/Tx: "+stat_network(system_info_.rx,system_info_.tx),108);
        write(L"Включая LAN/loopback",135);
    } else {
        write(L"GPU: "+value(3,L"%")+L" · VRAM: "+value(4,L"%"),108);
        write(L"VRAM: "+stat_memory(system_info_.gpu_memory)+L" / "+stat_memory(system_info_.gpu_capacity),135);
    }
}

void StatisticsPage::paint_tooltip(HDC dc) {
    if(system_tip_active_) {RECT rect{};GetClientRect(tooltip_,&rect);draw_system_hover(dc,rect);return;}
    if(hover_section_<0) return;
    const unsigned section=hover_section_==0?4U:5U;
    const auto& held=frozen_views_[section];
    const auto& source=held.paused?held.apps:(hover_section_==0?live_apps_:history_.history);
    if(hover_index_>=source.size()) return;
    RECT bounds{};GetClientRect(tooltip_,&bounds);
    draw_metric_tooltip(dc,bounds,source[hover_index_],hover_value_,hover_timestamp_);
}
LRESULT CALLBACK StatisticsPage::hover_dispatch(HWND window,UINT msg,WPARAM wp,LPARAM lp) {
    auto* page=reinterpret_cast<StatisticsPage*>(GetWindowLongPtrW(window,GWLP_USERDATA));
    if(msg==WM_NCCREATE) {
        page=static_cast<StatisticsPage*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(page));
        return TRUE;
    }
    if(!page) return DefWindowProcW(window,msg,wp,lp);
    switch(msg) {
    case WM_ERASEBKGND:return 1;
    case WM_MOUSEACTIVATE:return MA_NOACTIVATE;
    case WM_NCHITTEST:return HTTRANSPARENT;
    case WM_PAINT: {
        PAINTSTRUCT ps{};const HDC dc=BeginPaint(window,&ps);
        RECT bounds{};GetClientRect(window,&bounds);
        if(bounds.right>0 && bounds.bottom>0) {
            if(const HDC buffer=page->tooltip_buffer_.prepare(dc,bounds.right,bounds.bottom)) {
                FillRect(buffer,&bounds,GetSysColorBrush(COLOR_BTNFACE));
                page->paint_tooltip(buffer);
                BitBlt(dc,0,0,bounds.right,bounds.bottom,buffer,0,0,SRCCOPY);
            } else {
                FillRect(dc,&bounds,GetSysColorBrush(COLOR_BTNFACE));
                page->paint_tooltip(dc);
            }
        }
        EndPaint(window,&ps);return 0;
    }
    }
    return DefWindowProcW(window,msg,wp,lp);
}
LRESULT CALLBACK StatisticsPage::dispatch(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp) {
    auto* self=reinterpret_cast<StatisticsPage*>(GetWindowLongPtrW(hwnd,GWLP_USERDATA));
    if(msg==WM_NCCREATE) {
        self=static_cast<StatisticsPage*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        self->hwnd_=hwnd;
        SetWindowLongPtrW(hwnd,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));
        return TRUE;
    }
    return self?self->message(msg,wp,lp):DefWindowProcW(hwnd,msg,wp,lp);
}
LRESULT StatisticsPage::message(UINT msg,WPARAM wp,LPARAM lp) {
    switch(msg) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps{};const auto dc=BeginPaint(hwnd_,&ps);
        // A back buffer avoids flicker at one-second updates.
        RECT r{};GetClientRect(hwnd_,&r);
        if(r.right>0 && r.bottom>0) {
            if(const HDC buffer=page_buffer_.prepare(dc,r.right,r.bottom)) {
                const int sidebar_width=MulDiv(280,r.right,page_width);
                const bool sidebar_only=paint_font_ && ps.rcPaint.left>=0 &&
                    ps.rcPaint.right<=sidebar_width;
                if(sidebar_only) {
                    const int state=SaveDC(buffer);
                    SetMapMode(buffer,MM_ANISOTROPIC);
                    SetWindowExtEx(buffer,page_width,page_height,nullptr);
                    SetViewportExtEx(buffer,r.right,r.bottom,nullptr);
                    const RECT paper{0,0,280,page_height};
                    FillRect(buffer,&paper,GetSysColorBrush(COLOR_BTNFACE));
                    const auto old=SelectObject(buffer,paint_font_);
                    draw_sidebar(buffer);
                    SelectObject(buffer,old);
                    if(state)RestoreDC(buffer,state);
                    BitBlt(dc,0,0,sidebar_width,r.bottom,buffer,0,0,SRCCOPY);
                } else {
                    draw(buffer);
                    BitBlt(dc,0,0,r.right,r.bottom,buffer,0,0,SRCCOPY);
                }
            } else draw(dc); // No invisible page if a GDI allocation fails.
        }
        EndPaint(hwnd_,&ps);return 0;
    }
    case WM_LBUTTONUP: {
        RECT physical{};GetClientRect(hwnd_,&physical);
        if(physical.right && physical.bottom) {
            const int x=MulDiv(GET_X_LPARAM(lp),page_width,physical.right);
            const int y=MulDiv(GET_Y_LPARAM(lp),page_height,physical.bottom);
            if(x>=14 && x<=266 && y>=18) {
                const auto row=static_cast<unsigned>((y-18)/114)+sidebar_scroll_;
                const auto top=18+static_cast<int>(row-sidebar_scroll_)*114;
                if(y<top+105) {
                    if(const auto item=statistics_sidebar_item_at(
                        row,static_cast<unsigned>(disk_letters_.size()))) {
                        if(item->kind==StatisticsSidebarKind::section)
                            select_section(item->index);
                        else select_disk(disk_letters_[item->index]);
                    }
                }
            }
        }
        return 0;
    }
    case WM_MOUSEWHEEL: {
        POINT screen{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};
        ScreenToClient(hwnd_,&screen);
        RECT viewport{};GetClientRect(hwnd_,&viewport);
        const int x=viewport.right?MulDiv(screen.x,page_width,viewport.right):screen.x;
        if(x>=14 && x<=266) {
            const unsigned limit=static_cast<unsigned>(disk_letters_.size());
            const int direction=GET_WHEEL_DELTA_WPARAM(wp)<0?1:-1;
            sidebar_scroll_=static_cast<unsigned>(std::clamp(
                static_cast<int>(sidebar_scroll_)+direction,0,static_cast<int>(limit)));
            InvalidateRect(hwnd_,nullptr,FALSE);return 0;
        }
        break;
    }
    case WM_MOUSEMOVE: {
        TRACKMOUSEEVENT tracking{sizeof(tracking),TME_LEAVE,hwnd_,0};
        TrackMouseEvent(&tracking);
        mouse(GET_X_LPARAM(lp),GET_Y_LPARAM(lp));return 0;
    }
    case WM_TIMER:
        if(wp==animate_message) {
            const bool animating=animation_started_
                && GetTickCount64()-animation_started_<animation_duration_ms_;
            // The sampler controls the AutoScore cadence; this timer only animates the axis and line.
            const auto tick=GetTickCount64();
            const double elapsed=axis_tick_?std::clamp(static_cast<double>(tick-axis_tick_),1.0,100.0):33.0;
            axis_tick_=tick;
            const bool axis_animating=advance_axes(elapsed);
            const bool viewport_animating=advance_time_view(elapsed);
            const bool live_page=selected_disk_letter_
                ?!frozen_views_[6U+static_cast<unsigned>(selected_disk_letter_-L'A')].paused
                :(selected_section_<5 && !frozen_views_[selected_section_].paused);
            if(active_ && (axis_animating ||
                (live_page && (animating || viewport_animating)))) {
                InvalidateRect(hwnd_,nullptr,FALSE);
            } else if(active_ && viewport_animating) {
                RECT bounds{};GetClientRect(hwnd_,&bounds);
                const RECT sidebar{0,0,MulDiv(280,bounds.right,page_width),bounds.bottom};
                InvalidateRect(hwnd_,&sidebar,FALSE);
            }
            for(auto& popup:popups_)if(popup.window && (axis_animating ||
                (!popup.paused && popup.kind!=1 && (animating || viewport_animating))))
                InvalidateRect(popup.window,nullptr,FALSE);
            if((!animating && !axis_animating && !viewport_animating) ||
                (!active_ && !has_popup()))
                KillTimer(hwnd_,animate_message);
            return 0;
        }
        break;
    case WM_HSCROLL:
        if(reinterpret_cast<HWND>(lp)==limit_slider_) {
            const auto selected=static_cast<unsigned>(SendMessageW(limit_slider_,TBM_GETPOS,0,0));
            SetWindowTextW(limit_label_,(L"Количество приложений: "+std::to_wstring(selected)+L" / 100").c_str());
            if(LOWORD(wp)!=TB_THUMBTRACK && selected!=limit_) {
                set_limit(selected);
                // The parent updates its overlay setting and trackbar on the UI thread.
                PostMessageW(GetParent(hwnd_),WM_APP+215,selected,0);
            }
            return 0;
        }
        if(reinterpret_cast<HWND>(lp)==history_limit_slider_) {
            const auto selected=static_cast<unsigned>(SendMessageW(history_limit_slider_,TBM_GETPOS,0,0));
            SetWindowTextW(history_limit_label_,(L"История: "+std::to_wstring(selected)+L" / 100").c_str());
            if(LOWORD(wp)!=TB_THUMBTRACK) set_history_limit(selected);
            return 0;
        }
        if(reinterpret_cast<HWND>(lp)==uptime_limit_slider_) {
            const auto selected=static_cast<unsigned>(SendMessageW(uptime_limit_slider_,TBM_GETPOS,0,0));
            SetWindowTextW(uptime_limit_label_,(L"Аптайм: "+std::to_wstring(selected)+L" / 100").c_str());
            if(LOWORD(wp)!=TB_THUMBTRACK) set_uptime_limit(selected);
            return 0;
        }
        break;
    case WM_MOUSELEAVE:
        system_hover_={-1,-1};
        system_tip_active_=false;
        hover_section_=-1;
        if(tooltip_) ShowWindow(tooltip_,SW_HIDE);
        return 0;
    case WM_SIZE: if(hwnd_) {layout();InvalidateRect(hwnd_,nullptr,FALSE);} return 0;
    case ready_message: accept();return 0;
    case WM_COMMAND:
        if(LOWORD(wp)==system_popout_id+2 && HIWORD(wp)==BN_CLICKED) {
            toggle_section_pause();return 0;
        }
        if(LOWORD(wp)==system_popout_id+1 && HIWORD(wp)==BN_CLICKED) {
            fill_enabled_=SendMessageW(fill_toggle_,BM_GETCHECK,0,0)==BST_CHECKED;
            ini_overlay_write(settings_,L"stats_chart_fill",fill_enabled_);
            for(auto& popup:popups_)if(popup.window) {
                if(popup.fill_toggle)SendMessageW(popup.fill_toggle,BM_SETCHECK,
                    fill_enabled_?BST_CHECKED:BST_UNCHECKED,0);
                InvalidateRect(popup.window,nullptr,FALSE);
            }
            InvalidateRect(hwnd_,nullptr,FALSE);return 0;
        }
        if(HIWORD(wp)==BN_CLICKED && (LOWORD(wp)==auto_live_id || LOWORD(wp)==auto_history_id)) {
            const unsigned index=LOWORD(wp)==auto_live_id?0U:1U;
            const HWND control=index==0?auto_live_:auto_history_;
            auto_scale_[index]=SendMessageW(control,BM_GETCHECK,0,0)==BST_CHECKED;
            if(auto_scale_[index] && (index!=1 || !history_axis_refit_pending_))
                refresh_auto_axis(index,true);
            auto& held=frozen_views_[4U+index];
            if(held.paused)
                held.zoom=chart_effective_zoom(auto_scale_[index],
                    auto_vertical_zoom(held.apps,held.from,held.to),1.0);
            ini_overlay_write(settings_,index==0?L"stats_auto_live":L"stats_auto_history",
                auto_scale_[index]);
            InvalidateRect(hwnd_,nullptr,FALSE);
            return 0;
        }
        if(HIWORD(wp)==BN_CLICKED && LOWORD(wp)==popout_live_id) {open_popup(0);return 0;}
        if(HIWORD(wp)==BN_CLICKED && LOWORD(wp)==popout_history_id) {open_popup(1);return 0;}
        if(HIWORD(wp)==BN_CLICKED && LOWORD(wp)==popout_table_id) {open_table();return 0;}
        if(LOWORD(wp)==system_period_id && HIWORD(wp)==CBN_SELCHANGE) {
            system_range_=static_cast<unsigned>(std::clamp<int>(
                static_cast<int>(SendMessageW(system_period_,CB_GETCURSEL,0,0)),0,2));
            ini_overlay_write(settings_,L"stats_system_range",static_cast<int>(system_range_));
            InvalidateRect(hwnd_,nullptr,FALSE);return 0;
        }
        if(LOWORD(wp)==system_auto_id && HIWORD(wp)==BN_CLICKED) {
            system_autoscale_=SendMessageW(system_auto_,BM_GETCHECK,0,0)==BST_CHECKED;
            ini_overlay_write(settings_,L"stats_system_auto",system_autoscale_);
            InvalidateRect(hwnd_,nullptr,FALSE);return 0;
        }
        if(LOWORD(wp)==system_popout_id && HIWORD(wp)==BN_CLICKED) {
            open_popup(selected_disk_letter_
                ?6U+static_cast<unsigned>(selected_disk_letter_-L'A')
                :selected_popup_kind());return 0;
        }
        if(LOWORD(wp)==period_id && HIWORD(wp)==CBN_SELCHANGE) {
            period_index_=static_cast<int>(SendMessageW(period_,CB_GETCURSEL,0,0));
            history_axis_refit_pending_=true; // Refit only after the new DB result.
            update_dates();layout();request();return 0;
        }
        if(LOWORD(wp)==uptime_period_id && HIWORD(wp)==CBN_SELCHANGE) {
            uptime_period_index_=static_cast<int>(SendMessageW(uptime_period_,CB_GETCURSEL,0,0));
            update_dates();layout();request();return 0;
        }
        if(LOWORD(wp)==system_id && HIWORD(wp)==BN_CLICKED) {
            system_=SendMessageW(show_system_,BM_GETCHECK,0,0)==BST_CHECKED;
            ini_overlay_write(settings_,L"stats_show_system",system_);
            request(true);return 0; // Requery only uptime; graphs retain their data and positions.
        }
        break;
    case WM_NOTIFY: {
        const auto* hdr=reinterpret_cast<NMHDR*>(lp);
        if(hdr->code==DTN_DATETIMECHANGE &&
            (((hdr->idFrom==from_id || hdr->idFrom==to_id) && period_index_==2) ||
             ((hdr->idFrom==uptime_from_id || hdr->idFrom==uptime_to_id) && uptime_period_index_==2))) {
            if((hdr->idFrom==from_id || hdr->idFrom==to_id) && period_index_==2)
                history_axis_refit_pending_=true;
            request();return 0;
        }
        break;
    }
    }
    return DefWindowProcW(hwnd_,msg,wp,lp);
}
#include "statistics_popout.inc"
} // namespace fluxtop
