#include "fluxtop/diagnostics.hpp"
#include "fluxtop/statistics_page.hpp"
#include "fluxtop/statistics_axis.hpp"
#include "fluxtop/windows.hpp"
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

namespace fluxtop {
namespace {
constexpr wchar_t page_class[]=L"FluxTop.Statistics.Page";
constexpr wchar_t hover_class[]=L"FluxTop.Statistics.HoverTooltip.v1";
constexpr wchar_t table_class[]=L"FluxTop.Statistics.DetachedTable.v1";
constexpr UINT ready_message=WM_APP+201;
constexpr UINT_PTR animate_message=2201;
enum : int { period_id=1001,from_id,to_id,system_id,table_id,stats_limit_id,popout_live_id,popout_history_id,popout_table_id,history_limit_id,uptime_limit_id,uptime_period_id,uptime_from_id,uptime_to_id };
// Logical page dimensions; chart labels and Win32 controls share the same grid.
constexpr int page_width=860, page_height=860;
constexpr int auto_live_id=1201, auto_history_id=1202, popup_auto_id=1203;
constexpr RECT one_minute{54,96,828,238};
constexpr RECT history_chart{54,402,828,544};
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
// Display the same units as the overlay; unavailable readings remain unknown.
std::wstring stat_decimal(double value) {
    wchar_t buffer[64]{};
    swprintf_s(buffer,L"%.1f",value);
    return buffer;
}
std::wstring stat_percent(std::optional<double> value) {
    return value && std::isfinite(*value) ? stat_decimal(*value)+L"%" : L"—";
}
std::wstring stat_memory(std::optional<std::uint64_t> bytes) {
    if(!bytes) return L"—";
    const double mib=static_cast<double>(*bytes)/1048576.0;
    return mib>=1024.0?stat_decimal(mib/1024.0)+L" GiB":stat_decimal(mib)+L" MiB";
}
std::wstring stat_network(std::optional<double> rx,std::optional<double> tx) {
    const auto format=[](std::optional<double> bytes)->std::wstring {
        return bytes && std::isfinite(*bytes)?stat_decimal(*bytes*8.0/1000000.0):L"—";
    };
    return format(rx)+L" / "+format(tx)+L" Mbps";
}
std::wstring stat_wide(std::string_view text) {
    if(text.empty()) return {};
    const int count=MultiByteToWideChar(CP_UTF8,0,text.data(),static_cast<int>(text.size()),nullptr,0);
    if(count<=0) return L"?";
    std::wstring result(static_cast<std::size_t>(count),L'\0');
    MultiByteToWideChar(CP_UTF8,0,text.data(),static_cast<int>(text.size()),result.data(),count);
    return result;
}
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
    history_limit_=static_cast<unsigned>(std::clamp(GetPrivateProfileIntW(L"overlay",L"stats_history_top",
        static_cast<int>(limit_),settings_.c_str()),1U,100U));
    uptime_limit_=static_cast<unsigned>(std::clamp(GetPrivateProfileIntW(L"overlay",L"stats_uptime_top",
        static_cast<int>(limit_),settings_.c_str()),1U,100U));
    system_=GetPrivateProfileIntW(L"overlay",L"stats_show_system",0,settings_.c_str())!=0;
    auto_scale_[0]=GetPrivateProfileIntW(L"overlay",L"stats_auto_live",1,settings_.c_str())!=0;
    auto_scale_[1]=GetPrivateProfileIntW(L"overlay",L"stats_auto_history",1,settings_.c_str())!=0;
    popups_[0].owner=this;popups_[0].kind=0;
    popups_[1].owner=this;popups_[1].kind=1;
    WNDCLASSEXW cls{}; cls.cbSize=sizeof(cls);cls.hInstance=GetModuleHandleW(nullptr);
    cls.lpfnWndProc=dispatch;cls.lpszClassName=page_class;cls.hCursor=LoadCursorW(nullptr,IDC_ARROW);
    cls.hbrBackground=reinterpret_cast<HBRUSH>(COLOR_BTNFACE+1);
    if(!RegisterClassExW(&cls) && GetLastError()!=ERROR_CLASS_ALREADY_EXISTS)
        throw windows_error("Register Statistics page");
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
    if(!RegisterClassExW(&hover) && GetLastError()!=ERROR_CLASS_ALREADY_EXISTS)
        throw windows_error("Register statistics tooltip");
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
    layout();update_dates();
}
void StatisticsPage::resize(int x,int y,int width,int height) {
    MoveWindow(hwnd_,x,y,width,height,TRUE);layout();
}
void StatisticsPage::layout() {
    if(!hwnd_ || !period_) return;
    RECT r{};GetClientRect(hwnd_,&r);
    const auto sx=[&](int x){return MulDiv(x,r.right,page_width);};
    const auto sy=[&](int y){return MulDiv(y,r.bottom,page_height);};
    const auto put=[&](HWND w,int x,int y,int width,int height){
        MoveWindow(w,sx(x),sy(y),sx(width),sy(height),TRUE);
    };
    // Every section has its own heading, controls, then plot/list.
    // Live chart: share this trackbar's value with overlay's "Количество приложений".
    put(show_system_,418,607,265,35);
    put(limit_label_,18,43,248,27);
    put(limit_slider_,274,40,210,35);
    put(auto_live_,530,41,148,29);
    put(popout_live_,696,41,140,29);
    // Historical AutoScore: independent limit and calendar immediately above chart.
    put(history_limit_label_,18,296,147,27);
    put(history_limit_slider_,166,292,231,35);
    put(auto_history_,530,294,148,29);
    put(popout_history_,696,294,140,29);
    put(period_,18,345,165,230);
    put(from_,210,346,150,27);
    put(to_,385,346,150,27);
    put(first_label_,18,379,824,20);
    // Uptime: own application limit, own date range, detached-table action.
    put(uptime_limit_label_,18,611,147,27);
    put(uptime_limit_slider_,166,607,231,35);
    put(popout_table_,696,568,140,29);
    put(uptime_period_,18,650,165,190);
    put(uptime_from_,210,651,150,27);
    put(uptime_to_,385,651,150,27);
    put(table_,18,702,816,144);
    ListView_SetColumnWidth(table_,0,sx(360));
    ListView_SetColumnWidth(table_,1,sx(155));
    ListView_SetColumnWidth(table_,2,sx(282));
    // Child controls must receive a fresh font after the settings window changes DPI.
    HFONT stale_font=nullptr;
    const UINT current_dpi=GetDpiForWindow(hwnd_);
    if(!font_ || controls_font_dpi_!=current_dpi) {
        HFONT replacement=CreateFontW(-MulDiv(14,current_dpi,96),0,0,0,FW_NORMAL,0,0,0,
            DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
        if(replacement) {stale_font=font_;font_=replacement;controls_font_dpi_=current_dpi;}
    }
    for(auto child:{period_,from_,to_,uptime_period_,uptime_from_,uptime_to_,first_label_,
        show_system_,limit_label_,limit_slider_,history_limit_label_,history_limit_slider_,
        uptime_limit_label_,uptime_limit_slider_,table_,popout_live_,popout_history_,
        popout_table_,auto_live_,auto_history_})
        SendMessageW(child,WM_SETFONT,reinterpret_cast<WPARAM>(font_),TRUE);
    if(stale_font) DeleteObject(stale_font);
    ListView_SetBkColor(table_,GetSysColor(COLOR_WINDOW));
    ListView_SetTextBkColor(table_,GetSysColor(COLOR_WINDOW));
    ListView_SetTextColor(table_,GetSysColor(COLOR_WINDOWTEXT));
}
void StatisticsPage::activate(bool active) {
    active_=active;ShowWindow(hwnd_,active?SW_SHOW:SW_HIDE);
    if(!active) page_buffer_.clear(); // Release offscreen memory in other settings tabs.
    if(!active && tooltip_) ShowWindow(tooltip_,SW_HIDE);
    if(!active && !popups_[0].window) KillTimer(hwnd_,animate_message);
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
    WritePrivateProfileStringW(L"overlay",L"stats_history_top",std::to_wstring(value).c_str(),settings_.c_str());
    diagnostics::write(diagnostics::Level::info,"ui.history_limit","limit="+std::to_string(value));
    if(active_ || popups_[1].window) request();
}
void StatisticsPage::set_uptime_limit(unsigned value) {
    value=std::clamp(value,1U,100U);
    if(value==uptime_limit_) return;
    uptime_limit_=value;
    SendMessageW(uptime_limit_slider_,TBM_SETPOS,TRUE,value);
    SetWindowTextW(uptime_limit_label_,(L"Аптайм: "+std::to_wstring(value)+L" / 100").c_str());
    WritePrivateProfileStringW(L"overlay",L"stats_uptime_top",std::to_wstring(value).c_str(),settings_.c_str());
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
    if(!active_ && !popups_[0].window && !popups_[1].window && !table_window_) return;
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
    history_=std::move(item->second);
    if(history_.first_record_ms && first_record_!=history_.first_record_ms) {
        first_record_=history_.first_record_ms;update_dates();
    }
    rebuild_live_series();
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
    if(!RegisterClassExW(&cls) && GetLastError()!=ERROR_CLASS_ALREADY_EXISTS)
        throw windows_error("Register detached uptime table");
    table_window_=CreateWindowExW(0,table_class,L"FluxTop — аптайм приложений",
        WS_OVERLAPPEDWINDOW|WS_VISIBLE,CW_USEDEFAULT,CW_USEDEFAULT,980,560,
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
void StatisticsPage::on_snapshot(std::int64_t timestamp_ms,const std::vector<Process>& grouped,unsigned interval_ms) {
    if(!active_ && !popups_[0].window && !popups_[1].window && !table_window_) return;
    // A standalone uptime table only needs periodic database refreshes, not
    // rebuilding and repainting the one-minute graph for every sample.
    if(!active_ && !popups_[0].window && !popups_[1].window) {
        if(now_ms()-last_history_refresh_>30000) request();
        return;
    }
    animation_started_=GetTickCount64();
    animation_duration_ms_=std::clamp(interval_ms,250U,10000U);
    last_live_timestamp_=timestamp_ms;
    if(active_ || popups_[0].window) SetTimer(hwnd_,animate_message,33,nullptr);
    struct Ranked { const Process* process; double score; };
    std::vector<Ranked> sorted;
    sorted.reserve(grouped.size());
    // One UTF-8 conversion per metric row, and one AutoScore evaluation per app.
    current_readings_.clear();
    for(const auto& p:grouped) {
        StatisticsApplication identity;
        identity.name=stat_wide(p.name);
        identity.path=p.exe_path?stat_wide(*p.exe_path):L"";
        if(statistics_is_system(identity.name,identity.path)) continue;
        CurrentReadings values;
        values.cpu=stat_percent(p.cpu_percent);
        values.ram=stat_memory(p.working_set);
        values.gpu=stat_percent(p.gpu_percent);
        values.vram=stat_memory(p.dedicated_gpu_bytes);
        values.network=stat_network(p.rx_bytes_per_second,p.tx_bytes_per_second);
        current_readings_.emplace(color_key(identity),std::move(values));
        sorted.push_back({&p,auto_rank(p,31U).score});
    }
    std::stable_sort(sorted.begin(),sorted.end(),[](const Ranked& a,const Ranked& b){
        return a.score>b.score;
    });
    // Keep up to 100 live series so increasing stats_top does not need a new collection.
    for(std::size_t i=0;i<std::min<std::size_t>(sorted.size(),100U);++i) {
        const auto* p=sorted[i].process;
        live_.push_back({timestamp_ms,stat_wide(p->name),p->exe_path?stat_wide(*p->exe_path):L"",sorted[i].score});
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
    return app.path.empty()?L"name:"+app.name:L"path:"+app.path;
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
void StatisticsPage::draw_graph(HDC dc,const RECT& area,const std::vector<StatisticsApplication>& apps,
    std::int64_t from,std::int64_t to,const wchar_t* caption,bool is_live,
    double vertical_zoom,bool detached) {
    const HBRUSH paper=CreateSolidBrush(RGB(250,250,250));
    FillRect(dc,&area,paper);DeleteObject(paper);
    const auto original=SelectObject(dc,GetStockObject(NULL_BRUSH));
    const HPEN grid=graph_pen(RGB(210,217,221),true);
    auto old=grid?SelectObject(dc,grid):GetCurrentObject(dc,OBJ_PEN);
    for(int row=0;row<=4;++row) {
        const auto y=area.top+(area.bottom-area.top)*row/4;
        MoveToEx(dc,area.left,y,nullptr);LineTo(dc,area.right,y);
    }
    for(int col=0;col<=6;++col) {
        const auto x=area.left+(area.right-area.left)*col/6;
        MoveToEx(dc,x,area.top,nullptr);LineTo(dc,x,area.bottom);
    }
    if(grid) SelectObject(dc,old);
    label(dc,caption,area.left,area.top-28,RGB(64,63,61));
    const auto top_value=std::to_wstring(static_cast<int>(std::lround(100.0/vertical_zoom)));
    label(dc,top_value.c_str(),area.left-33,area.top-3);
    label(dc,L"0",area.left-15,area.bottom-13);
    if(to<=from) to=from+1;
    const int clip=SaveDC(dc);
    IntersectClipRect(dc,area.left,area.top,area.right,area.bottom);
    for(std::size_t i=0;i<apps.size();++i) {
        const auto& points=apps[i].points;
        if(points.empty()) continue;
        std::vector<POINT> line;
        line.reserve(points.size());
        // Plot only the visible time span plus a boundary point on each side.
        // This matters when a detached chart is zoomed into a long history.
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
            const auto yy=static_cast<double>(area.bottom)-std::clamp(score*vertical_zoom,0.0,100.0)/100.0*(area.bottom-area.top);
            line.push_back(POINT{static_cast<LONG>(std::lround(std::clamp(xx,-1000000.0,1000000.0))),
                static_cast<LONG>(std::lround(yy+jitter))});
        }
        // Only the newest sampled segment is revealed progressively. No fake samples
        // are generated and the underlying AutoScore history remains unchanged.
        if(is_live && line.size()>1 && std::prev(last)->first==last_live_timestamp_
            && points.back().first==last_live_timestamp_ && animation_started_) {
            const auto elapsed=GetTickCount64()-animation_started_;
            const double t=std::clamp(static_cast<double>(elapsed)/animation_duration_ms_,0.0,1.0);
            const auto previous=line[line.size()-2],final=line.back();
            line.back().x=previous.x+static_cast<LONG>(std::lround((final.x-previous.x)*t));
            line.back().y=previous.y+static_cast<LONG>(std::lround((final.y-previous.y)*t));
        }
        const auto ink=series_color(apps[i],i);
        const HPEN stroke=graph_pen(ink,false);
        if(stroke) old=SelectObject(dc,stroke);
        if(line.size()>1) Polyline(dc,line.data(),static_cast<int>(line.size()));
        else Ellipse(dc,line[0].x-2,line[0].y-2,line[0].x+2,line[0].y+2);
        if(stroke) SelectObject(dc,old);
        // Small uneven ink marks evoke hand-drawn notebook charts without a graphics engine.
        if(line.size()>1) {
            const HPEN detail=graph_pen(ink,true);
            if(detail) old=SelectObject(dc,detail);
            Polyline(dc,line.data(),static_cast<int>(line.size()));
            if(detail) SelectObject(dc,old);
        }
    }
    RestoreDC(dc,clip);
    if(apps.empty() || std::none_of(apps.begin(),apps.end(),[](const auto& a){return !a.points.empty();}))
        label(dc,is_live?L"Ожидание данных…":L"Нет данных за выбранный период",
            area.left+20,area.top+55,RGB(119,111,98));
    const auto start_label=is_live && !detached?std::wstring(L"−60 секунд"):local_label(from);
    const auto end_label=is_live && !detached?std::wstring(L"сейчас"):local_label(to-1);
    label(dc,start_label.c_str(),area.left,area.bottom+3,RGB(97,91,82));
    SIZE extent{};
    GetTextExtentPoint32W(dc,end_label.c_str(),static_cast<int>(end_label.size()),&extent);
    label(dc,end_label.c_str(),area.right-extent.cx,area.bottom+3,RGB(97,91,82));
    SelectObject(dc,original);
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
        const auto identity=event.path.empty()?L"name:"+event.name:L"path:"+event.path;
        auto found=positions.find(identity);
        if(found==positions.end()) {
            StatisticsApplication app;app.name=event.name;app.path=event.path;
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
    // Auto-axis limits only change with new data, not WM_MOUSEMOVE/WM_PAINT.
    const auto anchor=last_live_timestamp_>0?last_live_timestamp_:now_ms();
    const std::array<double,2> desired{{auto_vertical_zoom(live_apps_,anchor-60000,anchor),
        auto_vertical_zoom(history_.history,window_from_,window_to_)}};
    for(std::size_t index=0;index<2;++index) {
        target_auto_zoom_[index]=desired[index];
        if(!axis_initialized_[index]) {
            cached_auto_zoom_[index]=desired[index];
            axis_initialized_[index]=true;
        }
    }
    for(auto& popup:popups_) popup.axis_dirty=true;
    if(active_ || popups_[0].window || popups_[1].window)
        SetTimer(hwnd_,animate_message,33,nullptr);
}
void StatisticsPage::draw(HDC dc) {
    RECT physical{};GetClientRect(hwnd_,&physical);
    SetMapMode(dc,MM_ANISOTROPIC);SetWindowExtEx(dc,page_width,page_height,nullptr);
    SetViewportExtEx(dc,std::max<LONG>(1,physical.right),std::max<LONG>(1,physical.bottom),nullptr);
    // StatisticsPage native-theme refinement v2: only chart areas are custom-drawn.
    const RECT background{0,0,page_width,page_height};
    FillRect(dc,&background,GetSysColorBrush(COLOR_BTNFACE));
    if(!paint_font_) paint_font_=CreateFontW(-15,0,0,0,FW_SEMIBOLD,0,0,0,DEFAULT_CHARSET,0,0,
        CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
    const auto old_font=SelectObject(dc,paint_font_?paint_font_:GetStockObject(DEFAULT_GUI_FONT));
    // FluxTop stable auto-axis / separate popout window v2:
    // The same sample timestamp is used by graph painting and hit-testing.
    // Mouse movement must not scroll the axis or change the auto-scale.
    const auto now=last_live_timestamp_>0?last_live_timestamp_:now_ms();
    const double live_zoom=auto_scale_[0]?cached_auto_zoom_[0]:1.0;
    const double history_zoom=auto_scale_[1]?cached_auto_zoom_[1]:1.0;
    // Titles sit ABOVE each section's controls; charts no longer own their heading rows.
    label(dc,L"ПОСЛЕДНЯЯ МИНУТА · AutoScore",18,14);
    label(dc,L"ИСТОРИЯ · СРЕДНИЙ БАЛЛ AutoScore",18,263);
    label(dc,L"НАБЛЮДАЕМЫЙ АПТАЙМ ПРИЛОЖЕНИЙ",18,575);
    draw_graph(dc,one_minute,live_apps_,now-60000,now,L"",true,live_zoom);
    draw_graph(dc,history_chart,history_.history,window_from_,window_to_,L"",false,history_zoom);
    if(!history_.error.empty()) label(dc,history_.error.c_str(),18,552,RGB(162,50,40));
    // Tooltip is a topmost popup, never painted underneath native controls.
    SelectObject(dc,old_font);
}
void StatisticsPage::mouse(int x,int y) {
    RECT physical{};GetClientRect(hwnd_,&physical);
    if(physical.right<=0 || physical.bottom<=0) return;
    x=MulDiv(x,page_width,physical.right);y=MulDiv(y,page_height,physical.bottom);
    const bool in_live=y>=one_minute.top && y<=one_minute.bottom;
    const bool in_history=y>=history_chart.top && y<=history_chart.bottom;
    if(!in_live && !in_history) {
        if(hover_section_!=-1) {hover_section_=-1;if(tooltip_) ShowWindow(tooltip_,SW_HIDE);}
        return;
    }
    const auto& chart=in_live?live_apps_:history_.history;
    const auto& area=in_live?one_minute:history_chart;
    const auto now=last_live_timestamp_>0?last_live_timestamp_:now_ms();
    const auto from=in_live?now-60000:window_from_;
    const auto to=in_live?now:window_to_;
    const double zoom=auto_scale_[in_live?0:1]?cached_auto_zoom_[in_live?0:1]:1.0;
    double best=9.0*9.0;
    int section=-1;std::size_t selected=0;double score=0;std::int64_t sample_at=0;
    if(to>from) for(std::size_t i=0;i<chart.size();++i) {
        const auto& pts=chart[i].points;
        for(std::size_t j=0;j<pts.size();++j) {
            const double xpos=area.left+static_cast<double>(pts[j].first-from)/static_cast<double>(to-from)*(area.right-area.left);
            if(pts[j].first>=from && pts[j].first<=to) {
                const double ypos=area.bottom-std::clamp(pts[j].second*zoom,0.0,100.0)/100*(area.bottom-area.top);
                const double dist=(xpos-x)*(xpos-x)+(ypos-y)*(ypos-y);
                if(dist<best) {best=dist;section=in_live?0:1;selected=i;score=pts[j].second;sample_at=pts[j].first;}
            }
            if(j==0) continue;
            const auto& prev=pts[j-1];
            const double old_x=area.left+static_cast<double>(prev.first-from)/static_cast<double>(to-from)*(area.right-area.left);
            if(xpos<=old_x || x<old_x || x>xpos) continue;
            const double t=(x-old_x)/(xpos-old_x);
            const double line_score=prev.second+(pts[j].second-prev.second)*t;
            const double line_y=area.bottom-std::clamp(line_score*zoom,0.0,100.0)/100*(area.bottom-area.top);
            const double d=(line_y-y)*(line_y-y);
            if(d<best){best=d;section=in_live?0:1;selected=i;score=line_score;sample_at=prev.first+static_cast<std::int64_t>(static_cast<double>(pts[j].first-prev.first)*t);}
        }
    }
    if(hover_section_!=section || hover_index_!=selected || hover_at_.x!=x || hover_at_.y!=y || hover_timestamp_!=sample_at) {
        hover_section_=section;hover_index_=selected;hover_value_=score;hover_timestamp_=sample_at;hover_at_={x,y};
        if(section<0 && tooltip_) ShowWindow(tooltip_,SW_HIDE);
        else if(tooltip_) {
            POINT cursor{};GetCursorPos(&cursor);
            const UINT dpi=GetDpiForWindow(tooltip_);
            const int tip_width=MulDiv(420,static_cast<int>(dpi),96);
            const int tip_height=MulDiv(198,static_cast<int>(dpi),96);
            int left=cursor.x+MulDiv(18,static_cast<int>(dpi),96);
            int top=cursor.y+MulDiv(20,static_cast<int>(dpi),96);
            MONITORINFO monitor{sizeof(monitor)};
            if(GetMonitorInfoW(MonitorFromPoint(cursor,MONITOR_DEFAULTTONEAREST),&monitor)) {
                left=std::clamp(left,static_cast<int>(monitor.rcWork.left),
                    std::max(static_cast<int>(monitor.rcWork.left),static_cast<int>(monitor.rcWork.right)-tip_width));
                top=std::clamp(top,static_cast<int>(monitor.rcWork.top),
                    std::max(static_cast<int>(monitor.rcWork.top),static_cast<int>(monitor.rcWork.bottom)-tip_height));
            }
            SetWindowPos(tooltip_,HWND_TOPMOST,left,top,tip_width,tip_height,SWP_NOACTIVATE|SWP_SHOWWINDOW);
            InvalidateRect(tooltip_,nullptr,FALSE);
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
void StatisticsPage::paint_tooltip(HDC dc) {
    if(hover_section_<0) return;
    const auto& source=hover_section_==0?live_apps_:history_.history;
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
        page->paint_tooltip(dc);
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
                draw(buffer);BitBlt(dc,0,0,r.right,r.bottom,buffer,0,0,SRCCOPY);
            } else draw(dc); // No invisible page if a GDI allocation fails.
        }
        EndPaint(hwnd_,&ps);return 0;
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
            if(active_) InvalidateRect(hwnd_,nullptr,FALSE);
            for(auto& popup:popups_) if(popup.window) InvalidateRect(popup.window,nullptr,FALSE);
            if((!animating && !axis_animating) ||
                (!active_ && !popups_[0].window && !popups_[1].window))
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
        hover_section_=-1;
        if(tooltip_) ShowWindow(tooltip_,SW_HIDE);
        return 0;
    case WM_SIZE: if(hwnd_) {layout();InvalidateRect(hwnd_,nullptr,FALSE);} return 0;
    case ready_message: accept();return 0;
    case WM_COMMAND:
        if(HIWORD(wp)==BN_CLICKED && (LOWORD(wp)==auto_live_id || LOWORD(wp)==auto_history_id)) {
            const unsigned index=LOWORD(wp)==auto_live_id?0U:1U;
            const HWND control=index==0?auto_live_:auto_history_;
            auto_scale_[index]=SendMessageW(control,BM_GETCHECK,0,0)==BST_CHECKED;
            WritePrivateProfileStringW(L"overlay",index==0?L"stats_auto_live":L"stats_auto_history",
                auto_scale_[index]?L"1":L"0",settings_.c_str());
            InvalidateRect(hwnd_,nullptr,FALSE);
            return 0;
        }
        if(HIWORD(wp)==BN_CLICKED && LOWORD(wp)==popout_live_id) {open_popup(0);return 0;}
        if(HIWORD(wp)==BN_CLICKED && LOWORD(wp)==popout_history_id) {open_popup(1);return 0;}
        if(HIWORD(wp)==BN_CLICKED && LOWORD(wp)==popout_table_id) {open_table();return 0;}
        if(LOWORD(wp)==period_id && HIWORD(wp)==CBN_SELCHANGE) {
            period_index_=static_cast<int>(SendMessageW(period_,CB_GETCURSEL,0,0));
            update_dates();request();return 0;
        }
        if(LOWORD(wp)==uptime_period_id && HIWORD(wp)==CBN_SELCHANGE) {
            uptime_period_index_=static_cast<int>(SendMessageW(uptime_period_,CB_GETCURSEL,0,0));
            update_dates();request();return 0;
        }
        if(LOWORD(wp)==system_id && HIWORD(wp)==BN_CLICKED) {
            system_=SendMessageW(show_system_,BM_GETCHECK,0,0)==BST_CHECKED;
            WritePrivateProfileStringW(L"overlay",L"stats_show_system",system_?L"1":L"0",settings_.c_str());
            request(true);return 0; // Requery only uptime; graphs retain their data and positions.
        }
        break;
    case WM_NOTIFY: {
        const auto* hdr=reinterpret_cast<NMHDR*>(lp);
        if(hdr->code==DTN_DATETIMECHANGE &&
            (((hdr->idFrom==from_id || hdr->idFrom==to_id) && period_index_==2) ||
             ((hdr->idFrom==uptime_from_id || hdr->idFrom==uptime_to_id) && uptime_period_index_==2))) {
            request();return 0;
        }
        break;
    }
    }
    return DefWindowProcW(hwnd_,msg,wp,lp);
}
#include "statistics_popout.inc"
} // namespace fluxtop