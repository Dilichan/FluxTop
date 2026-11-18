#include "fluxtop/statistics_page.hpp"
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

namespace fluxtop {
namespace {
constexpr wchar_t page_class[]=L"FluxTop.Statistics.Page";
constexpr UINT ready_message=WM_APP+201;
enum : int { period_id=1001,from_id,to_id,system_id,table_id };
constexpr RECT one_minute{42,145,545,285};
constexpr RECT history_chart{42,325,545,465};
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
std::wstring span_label(double seconds) {
    const auto secs=static_cast<std::uint64_t>(std::max(0.0,seconds)+.5);
    const auto days=secs/86400,hours=(secs/3600)%24,minutes=(secs/60)%60;
    if(days) return std::to_wstring(days)+L" д "+std::to_wstring(hours)+L" ч";
    if(hours) return std::to_wstring(hours)+L" ч "+std::to_wstring(minutes)+L" мин";
    return std::to_wstring(minutes)+L" мин "+std::to_wstring(secs%60)+L" с";
}
COLORREF ink(std::size_t index) {
    static constexpr COLORREF palette[]={RGB(31,86,137),RGB(175,64,42),RGB(60,117,75),
        RGB(135,69,140),RGB(172,120,38),RGB(42,130,138),RGB(116,80,55),RGB(188,64,115)};
    return palette[index%std::size(palette)];
}
void label(HDC dc,const wchar_t* text,int x,int y,COLORREF color=RGB(58,56,52)) {
    SetTextColor(dc,color); SetBkMode(dc,TRANSPARENT); TextOutW(dc,x,y,text,static_cast<int>(wcslen(text)));
}
} // namespace
StatisticsPage::StatisticsPage(HWND parent,std::filesystem::path database,
    std::wstring settings_path,unsigned limit)
    :database_(std::move(database)),settings_(std::move(settings_path)),limit_(std::clamp(limit,1U,100U)) {
    system_=GetPrivateProfileIntW(L"overlay",L"stats_show_system",0,settings_.c_str())!=0;
    WNDCLASSEXW cls{}; cls.cbSize=sizeof(cls);cls.hInstance=GetModuleHandleW(nullptr);
    cls.lpfnWndProc=dispatch;cls.lpszClassName=page_class;cls.hCursor=LoadCursorW(nullptr,IDC_ARROW);
    cls.hbrBackground=reinterpret_cast<HBRUSH>(COLOR_WINDOW+1);
    if(!RegisterClassExW(&cls) && GetLastError()!=ERROR_CLASS_ALREADY_EXISTS)
        throw windows_error("Register Statistics page");
    hwnd_=CreateWindowExW(WS_EX_CONTROLPARENT,page_class,L"",WS_CHILD|WS_CLIPCHILDREN,
        0,0,560,760,parent,nullptr,GetModuleHandleW(nullptr),this);
    if(!hwnd_) throw windows_error("Create Statistics page");
    make_controls();
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
            auto result=read_statistics(database_,q.from,q.to,q.limit,q.system,cancelled);
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
    worker_.request_stop();{std::lock_guard lock(mutex_);cancel_query_.request_stop();}wake_.notify_all();
    if(worker_.joinable()) worker_.join();
    if(hwnd_ && IsWindow(hwnd_)) DestroyWindow(hwnd_);
    if(image_list_) ImageList_Destroy(image_list_);
    for(const auto& [path,icon]:icon_cache_) { (void)path; if(icon) DestroyIcon(icon); }
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
    show_system_=add(L"BUTTON",L"Показывать системные приложения",BS_AUTOCHECKBOX|WS_TABSTOP,system_id);
    SendMessageW(show_system_,BM_SETCHECK,system_?BST_CHECKED:BST_UNCHECKED,0);
    first_label_=add(L"STATIC",L"Первый снимок в БД: определяем…",0,0);
    table_=add(WC_LISTVIEWW,L"",LVS_REPORT|LVS_SINGLESEL|LVS_SHOWSELALWAYS|WS_BORDER|WS_TABSTOP,table_id);
    ListView_SetExtendedListViewStyle(table_,LVS_EX_FULLROWSELECT|LVS_EX_DOUBLEBUFFER);
    for(const auto& col:std::array<std::pair<const wchar_t*,int>,3>{{{L"Приложение",235},{L"Аптайм",112},{L"Последняя активность",170}}}) {
        LVCOLUMNW item{};item.mask=LVCF_WIDTH|LVCF_TEXT;item.cx=col.second;
        item.pszText=const_cast<LPWSTR>(col.first);
        ListView_InsertColumn(table_,Header_GetItemCount(ListView_GetHeader(table_)),&item);
    }
    image_list_=ImageList_Create(16,16,ILC_COLOR32|ILC_MASK,12,12);
    if(image_list_) ListView_SetImageList(table_,image_list_,LVSIL_SMALL);
    layout();update_dates();
}
void StatisticsPage::resize(int x,int y,int width,int height) {
    MoveWindow(hwnd_,x,y,width,height,TRUE);layout();
}
void StatisticsPage::layout() {
    if(!hwnd_ || !period_) return;
    RECT r{};GetClientRect(hwnd_,&r);
    const auto sx=[&](int x){return MulDiv(x,r.right,560);};
    const auto sy=[&](int y){return MulDiv(y,r.bottom,760);};
    const auto put=[&](HWND w,int x,int y,int width,int height){
        MoveWindow(w,sx(x),sy(y),sx(width),sy(height),TRUE);
    };
    put(period_,16,24,150,225); // CBS_DROPDOWNLIST needs popup height, not only edit height.
    put(from_,180,25,140,25);put(to_,335,25,140,25);
    put(first_label_,16,68,535,25);
    put(show_system_,16,483,325,30);
    put(table_,16,526,528,226);
    if(!font_) font_=CreateFontW(-MulDiv(13,GetDpiForWindow(hwnd_),96),0,0,0,FW_NORMAL,0,0,0,
        DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
    for(auto child:{period_,from_,to_,first_label_,show_system_,table_})
        SendMessageW(child,WM_SETFONT,reinterpret_cast<WPARAM>(font_),TRUE);
}
void StatisticsPage::activate(bool active) {
    active_=active;ShowWindow(hwnd_,active?SW_SHOW:SW_HIDE);
    if(active) request();
}
void StatisticsPage::set_limit(unsigned limit) {
    limit=std::clamp(limit,1U,100U);
    if(limit_==limit) return;
    limit_=limit;
    if(active_) request();
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
void StatisticsPage::request() {
    if(!active_) return;
    const auto now=now_ms();
    std::int64_t from=period_index_==0?now-86400000:now-604800000;
    std::int64_t to=now+1000;
    if(period_index_==2) {
        from=custom_timestamp(from_,false);
        to=custom_timestamp(to_,true);
    }
    if(first_record_) from=std::max(from,first_record_);
    to=std::min(to,now+86400000);
    if(to<=from) { SetWindowTextW(first_label_,L"Период пуст: выберите более позднюю дату окончания");return; }
    window_from_=from;window_to_=to;
    {
        std::lock_guard lock(mutex_);
        cancel_query_.request_stop();cancel_query_=std::stop_source{};
        requested_={from,to,limit_,system_,++generation_};
    }
    wake_.notify_all();
    last_history_refresh_=now;
}
void StatisticsPage::update_dates() {
    const bool custom=period_index_==2;
    EnableWindow(from_,custom);EnableWindow(to_,custom);
    if(first_record_) {
        SYSTEMTIME bounds[2]{local_time(first_record_),local_time(now_ms())};
        SendMessageW(from_,DTM_SETRANGE,GDTR_MIN|GDTR_MAX,reinterpret_cast<LPARAM>(bounds));
        SendMessageW(to_,DTM_SETRANGE,GDTR_MIN|GDTR_MAX,reinterpret_cast<LPARAM>(bounds));
        const auto line=std::wstring(L"Первая запись в БД: ")+local_label(first_record_)
            +L"   ·   Баллы доступны с момента обновления FluxTop";
        SetWindowTextW(first_label_,line.c_str());
    }
}
void StatisticsPage::accept() {
    std::optional<std::pair<unsigned,StatisticsHistory>> item;
    {
        std::lock_guard lock(mutex_);
        if(completed_ && completed_->first==generation_) item.swap(completed_);
    }
    if(!item) return;
    history_=std::move(item->second);
    if(history_.first_record_ms && first_record_!=history_.first_record_ms) {
        first_record_=history_.first_record_ms;update_dates();
    }
    populate_table();InvalidateRect(hwnd_,nullptr,FALSE);
}
void StatisticsPage::populate_table() {
    ListView_DeleteAllItems(table_);
    if(image_list_) ImageList_RemoveAll(image_list_);
    std::map<std::wstring,int> images;
    for(std::size_t i=0;i<history_.uptime.size();++i) {
        const auto& app=history_.uptime[i];
        LVITEMW item{};item.mask=LVIF_TEXT|LVIF_IMAGE;item.iItem=static_cast<int>(i);
        item.pszText=const_cast<wchar_t*>(app.name.c_str());
        item.iImage=-1;
        if(image_list_ && !app.path.empty()) {
            if(const auto it=images.find(app.path);it!=images.end()) item.iImage=it->second;
            else if(HICON icon=application_icon(app.path)) {
                item.iImage=ImageList_AddIcon(image_list_,icon);
                images.emplace(app.path,item.iImage);
            }
        }
        const auto index=ListView_InsertItem(table_,&item);
        if(index<0) continue;
        auto uptime=span_label(app.observed_seconds);
        auto last=local_label(app.last_seen_ms);
        ListView_SetItemText(table_,index,1,uptime.data());
        ListView_SetItemText(table_,index,2,last.data());
    }
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
void StatisticsPage::on_snapshot(std::int64_t timestamp_ms,const std::vector<Process>& grouped) {
    if(!active_) return;
    std::vector<const Process*> sorted;
    sorted.reserve(grouped.size());
    for(const auto& p:grouped) {
        std::wstring name;
        if(!p.name.empty()) {
            const auto bytes=static_cast<int>(p.name.size());
            const int count=MultiByteToWideChar(CP_UTF8,0,p.name.data(),bytes,nullptr,0);
            if(count>0) {name.resize(static_cast<std::size_t>(count));MultiByteToWideChar(CP_UTF8,0,p.name.data(),bytes,name.data(),count);}
        }
        std::wstring path;
        if(p.exe_path) {
            const int count=MultiByteToWideChar(CP_UTF8,0,p.exe_path->data(),static_cast<int>(p.exe_path->size()),nullptr,0);
            if(count>0) {path.resize(static_cast<std::size_t>(count));MultiByteToWideChar(CP_UTF8,0,p.exe_path->data(),static_cast<int>(p.exe_path->size()),path.data(),count);}
        }
        if(!system_ && statistics_is_system(name,path)) continue;
        sorted.push_back(&p);
    }
    std::stable_sort(sorted.begin(),sorted.end(),[](const Process* a,const Process* b){
        return auto_rank(*a,31U).score>auto_rank(*b,31U).score;
    });
    for(std::size_t i=0;i<std::min<std::size_t>(sorted.size(),limit_);++i) {
        const auto* p=sorted[i];
        auto convert=[](std::string_view s)->std::wstring {
            const int n=MultiByteToWideChar(CP_UTF8,0,s.data(),static_cast<int>(s.size()),nullptr,0);
            if(n<=0) return L"?";
            std::wstring out(static_cast<std::size_t>(n),L'\0');
            MultiByteToWideChar(CP_UTF8,0,s.data(),static_cast<int>(s.size()),out.data(),n);
            return out;
        };
        auto name=convert(p->name);
        auto path=p->exe_path?convert(*p->exe_path):L"";
        live_.push_back({timestamp_ms,std::move(name),std::move(path),auto_rank(*p,31U).score});
    }
    while(!live_.empty() && live_.front().time<timestamp_ms-60000) live_.pop_front();
    // Bounds a burst of very short sampling intervals or an unexpected clock rollback.
    while(live_.size()>10000) live_.pop_front();
    if(now_ms()-last_history_refresh_>30000) request();
    InvalidateRect(hwnd_,nullptr,FALSE);
}
void StatisticsPage::draw_graph(HDC dc,const RECT& area,const std::vector<StatisticsApplication>& apps,
    std::int64_t from,std::int64_t to,const wchar_t* caption,bool is_live) {
    const HBRUSH paper=CreateSolidBrush(RGB(250,248,239));
    FillRect(dc,&area,paper);DeleteObject(paper);
    const auto original=SelectObject(dc,GetStockObject(NULL_BRUSH));
    HPEN grid=CreatePen(PS_DOT,1,RGB(204,200,187));auto old=SelectObject(dc,grid);
    for(int row=0;row<=4;++row) {
        const auto y=area.top+(area.bottom-area.top)*row/4;
        MoveToEx(dc,area.left,y,nullptr);LineTo(dc,area.right,y);
    }
    for(int col=0;col<=6;++col) {
        const auto x=area.left+(area.right-area.left)*col/6;
        MoveToEx(dc,x,area.top,nullptr);LineTo(dc,x,area.bottom);
    }
    SelectObject(dc,old);DeleteObject(grid);
    HPEN border=CreatePen(PS_SOLID,1,RGB(119,111,98));old=SelectObject(dc,border);
    Rectangle(dc,area.left,area.top,area.right,area.bottom);
    SelectObject(dc,old);DeleteObject(border);
    label(dc,caption,area.left,area.top-28,RGB(64,63,61));
    label(dc,L"100",area.left-33,area.top-3);
    label(dc,L"0",area.left-15,area.bottom-13);
    if(to<=from) to=from+1;
    for(std::size_t i=0;i<apps.size();++i) {
        const auto& points=apps[i].points;
        if(points.empty()) continue;
        std::vector<POINT> line;
        line.reserve(points.size());
        for(const auto& [at,score]:points) {
            const auto fraction=std::clamp(static_cast<double>(at-from)/static_cast<double>(to-from),0.0,1.0);
            const auto jitter=.9*std::sin(static_cast<double>(at%5003)*.019+i*2.3);
            const auto xx=static_cast<double>(area.left)+fraction*(area.right-area.left);
            const auto yy=static_cast<double>(area.bottom)-std::clamp(score,0.0,100.0)/100.0*(area.bottom-area.top);
            line.push_back(POINT{static_cast<LONG>(std::lround(xx)),static_cast<LONG>(std::lround(yy+jitter))});
        }
        HPEN stroke=CreatePen(PS_SOLID,2,ink(i));old=SelectObject(dc,stroke);
        if(line.size()>1) Polyline(dc,line.data(),static_cast<int>(line.size()));
        else Ellipse(dc,line[0].x-2,line[0].y-2,line[0].x+2,line[0].y+2);
        SelectObject(dc,old);DeleteObject(stroke);
        // Small uneven ink marks evoke hand-drawn notebook charts without a graphics engine.
        if(line.size()>1) {
            HPEN detail=CreatePen(PS_DOT,1,ink(i));old=SelectObject(dc,detail);
            Polyline(dc,line.data(),static_cast<int>(line.size()));
            SelectObject(dc,old);DeleteObject(detail);
        }
    }
    if(apps.empty() || std::none_of(apps.begin(),apps.end(),[](const auto& a){return !a.points.empty();}))
        label(dc,is_live?L"Ожидание измерений AutoScore…":L"Баллы появятся после начала записи статистики",
            area.left+20,area.top+55,RGB(119,111,98));
    const auto start_label=is_live?std::wstring(L"−60 секунд"):local_label(from);
    const auto end_label=is_live?std::wstring(L"сейчас"):local_label(to-1);
    label(dc,start_label.c_str(),area.left,area.bottom+3,RGB(97,91,82));
    label(dc,end_label.c_str(),area.right-130,area.bottom+3,RGB(97,91,82));
    SelectObject(dc,original);
}
void StatisticsPage::draw(HDC dc) {
    RECT physical{};GetClientRect(hwnd_,&physical);
    SetMapMode(dc,MM_ANISOTROPIC);SetWindowExtEx(dc,560,760,nullptr);
    SetViewportExtEx(dc,std::max<LONG>(1,physical.right),std::max<LONG>(1,physical.bottom),nullptr);
    const RECT background{0,0,560,760};
    HBRUSH paper=CreateSolidBrush(RGB(245,243,236));FillRect(dc,&background,paper);DeleteObject(paper);
    HFONT print=CreateFontW(-16,0,0,0,FW_SEMIBOLD,0,0,0,DEFAULT_CHARSET,0,0,
        CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe Print");
    const auto old_font=SelectObject(dc,print);
    label(dc,L"БАЛЛЫ АВТОРЕЖИМА  ·  0–100",18,102);
    const auto now=now_ms();
    std::vector<StatisticsApplication> minute=history_.recent;
    // Merge the last minute read from SQLite with fresh in-memory samples.
    for(const auto& event:live_) {
        if(event.time<now-60000) continue;
        auto found=std::find_if(minute.begin(),minute.end(),[&](const auto& app){
            return !event.path.empty()?app.path==event.path:app.path.empty()&&app.name==event.name;
        });
        if(found==minute.end()) {
            StatisticsApplication app;app.name=event.name;app.path=event.path;
            minute.push_back(std::move(app));found=std::prev(minute.end());
        }
        // Writer may have already recorded the same timestamp; replace instead of double drawing.
        auto& points=found->points;
        auto prev=std::find_if(points.begin(),points.end(),[&](const auto& p){return p.first==event.time;});
        if(prev!=points.end()) prev->second=event.score;
        else points.emplace_back(event.time,event.score);
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
    draw_graph(dc,one_minute,live_apps_,now-60000,now,L"ПОСЛЕДНЯЯ МИНУТА  ·  LIVE",true);
    draw_graph(dc,history_chart,history_.history,window_from_,window_to_,L"ИСТОРИЯ  ·  СРЕДНИЙ БАЛЛ",false);
    if(!history_.error.empty()) label(dc,history_.error.c_str(),18,467,RGB(162,50,40));
    label(dc,L"НАБЛЮДАЕМЫЙ АПТАЙМ ПРИЛОЖЕНИЙ",18,515);
    if(hover_section_>=0) paint_tooltip(dc);
    SelectObject(dc,old_font);DeleteObject(print);
}
void StatisticsPage::mouse(int x,int y) {
    RECT physical{};GetClientRect(hwnd_,&physical);
    if(physical.right<=0 || physical.bottom<=0) return;
    x=MulDiv(x,560,physical.right);y=MulDiv(y,760,physical.bottom);
    const bool in_live=y>=one_minute.top && y<=one_minute.bottom;
    const bool in_history=y>=history_chart.top && y<=history_chart.bottom;
    if(!in_live && !in_history) {
        if(hover_section_!=-1) {hover_section_=-1;InvalidateRect(hwnd_,nullptr,FALSE);}
        return;
    }
    const auto& chart=in_live?live_apps_:history_.history;
    const auto& area=in_live?one_minute:history_chart;
    const auto now=now_ms();
    const auto from=in_live?now-60000:window_from_;
    const auto to=in_live?now:window_to_;
    double best=9.0*9.0;
    int section=-1;std::size_t selected=0;double score=0;
    if(to>from) for(std::size_t i=0;i<chart.size();++i) {
        const auto& pts=chart[i].points;
        for(std::size_t j=0;j<pts.size();++j) {
            const double xpos=area.left+std::clamp(static_cast<double>(pts[j].first-from)/static_cast<double>(to-from),0.0,1.0)*(area.right-area.left);
            const double ypos=area.bottom-std::clamp(pts[j].second,0.0,100.0)/100*(area.bottom-area.top);
            const double dist=(xpos-x)*(xpos-x)+(ypos-y)*(ypos-y);
            if(dist<best) {best=dist;section=in_live?0:1;selected=i;score=pts[j].second;}
            if(j==0) continue;
            const auto& prev=pts[j-1];
            const double old_x=area.left+std::clamp(static_cast<double>(prev.first-from)/static_cast<double>(to-from),0.0,1.0)*(area.right-area.left);
            if(xpos<=old_x || x<old_x || x>xpos) continue;
            const double t=(x-old_x)/(xpos-old_x);
            const double line_score=prev.second+(pts[j].second-prev.second)*t;
            const double line_y=area.bottom-std::clamp(line_score,0.0,100.0)/100*(area.bottom-area.top);
            const double d=(line_y-y)*(line_y-y);
            if(d<best){best=d;section=in_live?0:1;selected=i;score=line_score;}
        }
    }
    if(hover_section_!=section || hover_index_!=selected || hover_at_.x!=x || hover_at_.y!=y) {
        hover_section_=section;hover_index_=selected;hover_value_=score;hover_at_={x,y};
        InvalidateRect(hwnd_,nullptr,FALSE);
    }
}
void StatisticsPage::paint_tooltip(HDC dc) {
    const auto& source=hover_section_==0?live_apps_:history_.history;
    if(hover_index_>=source.size()) return;
    const auto& app=source[hover_index_];
    const int left=std::clamp(static_cast<int>(hover_at_.x+14),10,310);
    const int top=std::clamp(static_cast<int>(hover_at_.y+13),105,430);
    RECT r{left,top,left+225,top+54};
    HBRUSH bg=CreateSolidBrush(RGB(255,255,244));FillRect(dc,&r,bg);DeleteObject(bg);
    FrameRect(dc,&r,reinterpret_cast<HBRUSH>(GetStockObject(GRAY_BRUSH)));
    if(auto icon=application_icon(app.path)) DrawIconEx(dc,left+7,top+15,icon,20,20,0,nullptr,DI_NORMAL);
    RECT label_rect{left+33,top+5,left+216,top+28};
    SetBkMode(dc,TRANSPARENT);SetTextColor(dc,RGB(44,40,35));
    DrawTextW(dc,app.name.c_str(),-1,&label_rect,DT_LEFT|DT_SINGLELINE|DT_END_ELLIPSIS|DT_NOPREFIX);
    wchar_t score[48]{};swprintf_s(score,L"AutoScore: %.1f / 100",hover_value_);
    label(dc,score,left+33,top+29,ink(hover_index_));
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
            HDC buffer=CreateCompatibleDC(dc);
            HBITMAP bitmap=CreateCompatibleBitmap(dc,r.right,r.bottom);
            if(buffer && bitmap) {
                auto old=SelectObject(buffer,bitmap);
                draw(buffer);BitBlt(dc,0,0,r.right,r.bottom,buffer,0,0,SRCCOPY);
                SelectObject(buffer,old);
            } else draw(dc);
            if(bitmap) DeleteObject(bitmap);
            if(buffer) DeleteDC(buffer);
        }
        EndPaint(hwnd_,&ps);return 0;
    }
    case WM_MOUSEMOVE: mouse(GET_X_LPARAM(lp),GET_Y_LPARAM(lp));return 0;
    case WM_MOUSELEAVE: hover_section_=-1;InvalidateRect(hwnd_,nullptr,FALSE);return 0;
    case WM_SIZE: if(hwnd_) {layout();InvalidateRect(hwnd_,nullptr,FALSE);} return 0;
    case ready_message: accept();return 0;
    case WM_COMMAND:
        if(LOWORD(wp)==period_id && HIWORD(wp)==CBN_SELCHANGE) {
            period_index_=static_cast<int>(SendMessageW(period_,CB_GETCURSEL,0,0));
            update_dates();request();return 0;
        }
        if(LOWORD(wp)==system_id && HIWORD(wp)==BN_CLICKED) {
            system_=SendMessageW(show_system_,BM_GETCHECK,0,0)==BST_CHECKED;
            WritePrivateProfileStringW(L"overlay",L"stats_show_system",system_?L"1":L"0",settings_.c_str());
            live_.clear();request();return 0;
        }
        break;
    case WM_NOTIFY: {
        const auto* hdr=reinterpret_cast<NMHDR*>(lp);
        if((hdr->idFrom==from_id || hdr->idFrom==to_id) && hdr->code==DTN_DATETIMECHANGE
           && period_index_==2) {request();return 0;}
        break;
    }
    }
    return DefWindowProcW(hwnd_,msg,wp,lp);
}
} // namespace fluxtop