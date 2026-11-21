#include "fluxtop/statistics_history.hpp"
#include "fluxtop/diagnostics.hpp"
#include "fluxtop/history_math.hpp"
#include <winsqlite/winsqlite3.h>
#include <windows.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cwctype>
#include <stdexcept>
#include <string>
#include <vector>

namespace fluxtop {
namespace {
std::wstring utf16(const unsigned char* data){
    if(!data)return {};
    const char* s=reinterpret_cast<const char*>(data);
    const int bytes=static_cast<int>(std::strlen(s));
    if(!bytes)return {};
    const int n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,s,bytes,nullptr,0);
    if(n<=0)return L"?";
    std::wstring out(static_cast<std::size_t>(n),L'\0');
    MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,s,bytes,out.data(),n);
    return out;
}
std::wstring lowercase(std::wstring s){
    std::transform(s.begin(),s.end(),s.begin(),[](wchar_t x){return static_cast<wchar_t>(towlower(x));});return s;
}
std::string utf8path(const std::filesystem::path& path){
    const auto u=path.u8string();return {reinterpret_cast<const char*>(u.data()),u.size()};
}
struct ReadContext {
    std::stop_token cancel;
    std::chrono::steady_clock::time_point deadline;
};
struct Db {
    sqlite3* db{};
    ReadContext context;
    Db(const std::filesystem::path& file,std::stop_token stop)
        :context{stop,std::chrono::steady_clock::now()+std::chrono::seconds(3)}{
        if(sqlite3_open_v2(utf8path(file).c_str(),&db,SQLITE_OPEN_READONLY|SQLITE_OPEN_NOMUTEX,nullptr)!=SQLITE_OK)
            throw std::runtime_error("History database unavailable");
        sqlite3_busy_timeout(db,250);
        sqlite3_progress_handler(db,2000,[](void* ptr)->int{
            const auto& c=*static_cast<ReadContext*>(ptr);
            return c.cancel.stop_requested() || std::chrono::steady_clock::now()>c.deadline;
        },&context);
    }
    ~Db(){if(db)sqlite3_close(db);}
    Db(const Db&)=delete;
};
struct Query {
    sqlite3_stmt* s{};
    Query(sqlite3* db,const char* sql){
        if(sqlite3_prepare_v2(db,sql,-1,&s,nullptr)!=SQLITE_OK)throw std::runtime_error(sqlite3_errmsg(db));
    }
    ~Query(){sqlite3_finalize(s);}
    void number(int position,std::int64_t value){sqlite3_bind_int64(s,position,value);}
    int next(){
        const int rc=sqlite3_step(s);
        if(rc!=SQLITE_ROW && rc!=SQLITE_DONE)throw std::runtime_error(sqlite3_errmsg(sqlite3_db_handle(s)));
        return rc;
    }
};
// Expand lower bound by at most one day: a daily bucket may overlap the
// requested window although its starting timestamp precedes the exact boundary.
std::int64_t search_from(std::int64_t from){return from-history_day_ms;}
std::vector<StatisticsApplication> top_apps(sqlite3* db,std::int64_t from,std::int64_t to,
    unsigned limit,bool include_system,bool by_score,std::stop_token stop){
    constexpr char score_sql[]=
        "SELECT a.id,a.name,a.exe_path,a.identity,"
        "SUM(r.score_integral)/NULLIF(SUM(r.score_duration_ms),0),MAX(r.last_seen_ms) "
        "FROM history_rollups r JOIN applications a ON a.id=r.application_id "
        "WHERE r.bucket_ms>=?1 AND r.bucket_ms<?2 "
        "AND r.bucket_ms+(CASE r.level WHEN 0 THEN 60000 WHEN 1 THEN 3600000 ELSE 86400000 END)>?3 "
        "GROUP BY a.id ORDER BY SUM(r.score_integral)/NULLIF(SUM(r.score_duration_ms),0) DESC LIMIT ?4";
    constexpr char uptime_sql[]=
        "SELECT a.id,a.name,a.exe_path,a.identity,SUM(r.uptime_ms)/1000.0,MAX(r.last_seen_ms) "
        "FROM history_rollups r JOIN applications a ON a.id=r.application_id "
        "WHERE r.bucket_ms>=?1 AND r.bucket_ms<?2 "
        "AND r.bucket_ms+(CASE r.level WHEN 0 THEN 60000 WHEN 1 THEN 3600000 ELSE 86400000 END)>?3 "
        "GROUP BY a.id ORDER BY SUM(r.uptime_ms) DESC LIMIT ?4";
    Query q(db,by_score?score_sql:uptime_sql);
    // Both SQL variants must expose id, name, path, identity, metric, last_seen.
    // Never interpret a timestamp as an uptime or score if projections diverge.
    if(sqlite3_column_count(q.s)!=6)
        throw std::runtime_error("Unexpected statistics application query layout");
    q.number(1,search_from(from));q.number(2,to);q.number(3,from);
    q.number(4,std::max<unsigned>(200U,std::min<unsigned>(2000U,limit*8U)));
    std::vector<StatisticsApplication> result;
    while(q.next()==SQLITE_ROW && !stop.stop_requested()){
        StatisticsApplication row;
        row.id=sqlite3_column_int64(q.s,0);
        row.name=utf16(sqlite3_column_text(q.s,1));
        row.path=utf16(sqlite3_column_text(q.s,2));
        row.identity=utf16(sqlite3_column_text(q.s,3));
        row.observed_seconds=sqlite3_column_double(q.s,4);
        row.last_seen_ms=sqlite3_column_int64(q.s,5);
        if(!include_system && statistics_is_system(row.name,row.path))continue;
        result.push_back(std::move(row));
        if(result.size()>=limit)break;
    }
    return result;
}
std::vector<StatisticsApplication> graph(sqlite3* db,const std::vector<StatisticsApplication>& ranked,
   std::int64_t from,std::int64_t to,std::int64_t bucket,std::stop_token stop){
    std::vector<StatisticsApplication> result=ranked;
    Query q(db,
        "SELECT (r.bucket_ms / ?1)*?1,"
        "SUM(r.score_integral)/NULLIF(SUM(r.score_duration_ms),0) "
        "FROM history_rollups r "
        "WHERE r.application_id=?2 AND r.bucket_ms>=?3 AND r.bucket_ms<?4 "
        "AND r.bucket_ms+(CASE r.level WHEN 0 THEN 60000 WHEN 1 THEN 3600000 ELSE 86400000 END)>?5 "
        "GROUP BY 1 ORDER BY 1");
    for(auto& app:result){
        if(stop.stop_requested())return {};
        sqlite3_reset(q.s);sqlite3_clear_bindings(q.s);
        q.number(1,bucket);q.number(2,app.id);q.number(3,search_from(from));
        q.number(4,to);q.number(5,from);
        while(q.next()==SQLITE_ROW){
            if(sqlite3_column_type(q.s,1)==SQLITE_NULL)continue;
            const double score=sqlite3_column_double(q.s,1);
            if(!std::isfinite(score))continue;
            const std::int64_t raw_stamp=sqlite3_column_int64(q.s,0);
            auto stamp=raw_stamp;
            stamp=std::clamp(stamp,from,to);
            app.points.emplace_back(stamp,std::clamp(score,0.0,100.0));
        }
    }
    return result;
}
}
bool statistics_is_system(std::wstring_view name,std::wstring_view path){
    const auto n=lowercase(std::wstring(name));
    for(const auto* known:{L"system",L"registry",L"idle",L"system idle process",L"[system process]",
        L"svchost.exe",L"csrss.exe",L"smss.exe",L"wininit.exe",L"winlogon.exe",
        L"services.exe",L"lsass.exe",L"dwm.exe",L"fontdrvhost.exe"})
        if(n==known)return true;
    static const std::wstring win=[] {
        wchar_t path_buffer[MAX_PATH]{};
        const UINT length=GetWindowsDirectoryW(path_buffer,MAX_PATH);
        return length && length<MAX_PATH ? lowercase(std::wstring(path_buffer,length)):std::wstring{};
    }();
    if(!win.empty()){
        const auto p=lowercase(std::wstring(path));
        return p.size()>win.size() && p.starts_with(win) && (p[win.size()]==L'\\'||p[win.size()]==L'/');
    }
    return false;
}
StatisticsHistory read_statistics(const std::filesystem::path& file,
    std::int64_t from_ms,std::int64_t to_ms,unsigned history_limit,unsigned uptime_limit,
    unsigned live_limit,std::int64_t uptime_from_ms,std::int64_t uptime_to_ms,
    bool include_system,std::stop_token stop,bool uptime_only){
    StatisticsHistory out;
    diagnostics::write(diagnostics::Level::trace,"statistics.read","query started");
    if(stop.stop_requested() || file.empty() || !std::filesystem::exists(file))return out;
    try {
        Db connection(file,stop);
        Query version(connection.db,"PRAGMA user_version");
        if(version.next()!=SQLITE_ROW)
            throw std::runtime_error("Cannot read database schema");
        const int schema=sqlite3_column_int(version.s,0);
        if(schema==0)return out; // Icon cache created the file before the first completed minute.
        if(schema!=2)throw std::runtime_error("Unsupported history schema (expected compact rollups v2)");
        Query bounds(connection.db,"SELECT first_ms,last_ms FROM history_state WHERE id=1");
        if(bounds.next()!=SQLITE_ROW || sqlite3_column_type(bounds.s,0)==SQLITE_NULL)return out;
        out.first_record_ms=sqlite3_column_int64(bounds.s,0);
        out.last_record_ms=sqlite3_column_int64(bounds.s,1);
        from_ms=std::max(from_ms,out.first_record_ms);
        uptime_from_ms=std::max(uptime_from_ms,out.first_record_ms);
        out.scores_available=true;
        history_limit=std::clamp(history_limit,1U,100U);
        uptime_limit=std::clamp(uptime_limit,1U,100U);
        live_limit=std::clamp(live_limit,1U,100U);
        if(uptime_to_ms>uptime_from_ms)
            out.uptime=top_apps(connection.db,uptime_from_ms,uptime_to_ms,uptime_limit,include_system,false,stop);
        if(uptime_only || stop.stop_requested())return out;
        if(to_ms>from_ms) {
            const auto bucket=std::max<std::int64_t>(history_minute_ms,(to_ms-from_ms+479)/480);
            auto ranked=top_apps(connection.db,from_ms,to_ms,history_limit,false,true,stop);
            out.history=graph(connection.db,ranked,from_ms,to_ms,bucket,stop);
        }
        const auto now=std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        const auto start=std::max(out.first_record_ms,now-history_minute_ms);
        auto live_ranked=top_apps(connection.db,start,now+1000,live_limit,false,true,stop);
        out.recent=graph(connection.db,live_ranked,start,now+1000,1000,stop);
        diagnostics::write(diagnostics::Level::trace,"statistics.result",
            "uptime="+std::to_string(out.uptime.size())+" history="+std::to_string(out.history.size()));
    }catch(const std::exception& err){
        diagnostics::write(diagnostics::Level::warning,"statistics.error",err.what());
        if(!stop.stop_requested())out.error=utf16(reinterpret_cast<const unsigned char*>(err.what()));
    }
    return out;
}
StatisticsHistory read_statistics(const std::filesystem::path& file,
    std::int64_t from_ms,std::int64_t to_ms,unsigned history_limit,unsigned uptime_limit,
    unsigned live_limit,bool include_system,std::stop_token stop){
    return read_statistics(file,from_ms,to_ms,history_limit,uptime_limit,live_limit,
        from_ms,to_ms,include_system,stop);
}
StatisticsHistory read_statistics(const std::filesystem::path& file,
    std::int64_t from_ms,std::int64_t to_ms,unsigned limit,bool include_system,std::stop_token stop){
    return read_statistics(file,from_ms,to_ms,limit,limit,limit,include_system,stop);
}
}
