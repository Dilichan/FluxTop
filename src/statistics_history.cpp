#include "fluxtop/statistics_history.hpp"
#include "fluxtop/windows.hpp"
#include <winsqlite/winsqlite3.h>
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <chrono>
#include <cwctype>
#include <stdexcept>

namespace fluxtop {
namespace {
std::wstring utf16(const unsigned char* data) {
    if (!data) return {};
    const char* s=reinterpret_cast<const char*>(data);
    const int bytes=static_cast<int>(strlen(s));
    if (!bytes) return {};
    const int count=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,s,bytes,nullptr,0);
    if (count<=0) return L"?";
    std::wstring out(static_cast<std::size_t>(count),L'\0');
    MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,s,bytes,out.data(),count);
    return out;
}
std::wstring lowercase(std::wstring value) {
    std::transform(value.begin(),value.end(),value.begin(),[](wchar_t c){return static_cast<wchar_t>(towlower(c));});
    return value;
}
struct Database {
    sqlite3* db{};
    explicit Database(const std::filesystem::path& path,std::stop_token* stop) {
        const auto filename=utf8(path.c_str());
        if (sqlite3_open_v2(filename.c_str(),&db,SQLITE_OPEN_READONLY|SQLITE_OPEN_NOMUTEX,nullptr)!=SQLITE_OK)
            throw std::runtime_error("History database unavailable");
        sqlite3_busy_timeout(db,100);
        sqlite3_progress_handler(db,1000,[](void* ctx)->int{
            return static_cast<std::stop_token*>(ctx)->stop_requested()?1:0;
        },stop);
    }
    ~Database() { if(db) sqlite3_close(db); }
    Database(const Database&)=delete;
    Database& operator=(const Database&)=delete;
};
struct Statement {
    sqlite3_stmt* s{};
    explicit Statement(sqlite3* db,const char* query) {
        if (sqlite3_prepare_v2(db,query,-1,&s,nullptr)!=SQLITE_OK)
            throw std::runtime_error(sqlite3_errmsg(db));
    }
    ~Statement(){sqlite3_finalize(s);}
    void num(int index,std::int64_t value){sqlite3_bind_int64(s,index,value);}
    int next() {
        const int rc=sqlite3_step(s);
        if(rc!=SQLITE_ROW && rc!=SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(sqlite3_db_handle(s)));
        return rc;
    }
};
std::vector<StatisticsApplication> graph(sqlite3* db,const std::vector<StatisticsApplication>& selected,
    std::int64_t from,std::int64_t to,std::int64_t bucket,std::stop_token stop) {
    std::vector<StatisticsApplication> result=selected;
    // The score table stores a single application per sample, not one entry for
    // every child process, so each bucket is an unbiased average of snapshots.
    Statement query(db,
        "SELECT (s.timestamp_ms / ?1) * ?1, AVG(g.score) "
        "FROM application_scores g JOIN snapshots s ON s.id=g.sample_id "
        "WHERE g.application_id=?2 AND s.timestamp_ms>=?3 AND s.timestamp_ms<?4 "
        "GROUP BY 1 ORDER BY 1");
    for (auto& app: result) {
        if (stop.stop_requested()) return {};
        sqlite3_reset(query.s);sqlite3_clear_bindings(query.s);
        query.num(1,bucket);query.num(2,app.id);query.num(3,from);query.num(4,to);
        while(query.next()==SQLITE_ROW) {
            const double score=sqlite3_column_double(query.s,1);
            if (std::isfinite(score)) app.points.emplace_back(sqlite3_column_int64(query.s,0),std::clamp(score,0.0,100.0));
        }
    }
    return result;
}
std::vector<StatisticsApplication> select_top(sqlite3* db,std::int64_t from,std::int64_t to,
    unsigned limit,bool include_system,bool by_score,std::stop_token stop) {
    // DISTINCT app/sample prevents 6 Chrome child processes from counting as
    // 6 seconds of uptime for one second of actual observation.
    constexpr char uptime_sql[] =
        "SELECT a.id,a.name,a.exe_path,SUM(t.ms)/1000.0,MAX(t.last_seen) "
        "FROM (SELECT p.application_id, s.id, MIN(MAX(s.interval_ms,0.0),10000.0) AS ms, "
        "MAX(s.timestamp_ms) AS last_seen FROM snapshots s "
        "JOIN measurements m ON m.sample_id=s.id "
        "JOIN process_instances p ON p.id=m.process_instance_id "
        "WHERE s.timestamp_ms>=?1 AND s.timestamp_ms<?2 "
        "GROUP BY p.application_id,s.id) t "
        "JOIN applications a ON a.id=t.application_id "
        "GROUP BY a.id ORDER BY SUM(t.ms) DESC LIMIT ?3";
    constexpr char score_sql[] =
        "SELECT a.id,a.name,a.exe_path,AVG(g.score),MAX(s.timestamp_ms) "
        "FROM snapshots s JOIN application_scores g ON g.sample_id=s.id "
        "JOIN applications a ON a.id=g.application_id "
        "WHERE s.timestamp_ms>=?1 AND s.timestamp_ms<?2 "
        "GROUP BY a.id ORDER BY AVG(g.score) DESC LIMIT ?3";
    Statement q(db,by_score?score_sql:uptime_sql);
    q.num(1,from);q.num(2,to);q.num(3,std::min<unsigned>(2000,std::max<unsigned>(200,limit*8)));
    std::vector<StatisticsApplication> out;
    while(q.next()==SQLITE_ROW && !stop.stop_requested()) {
        StatisticsApplication app;
        app.id=sqlite3_column_int64(q.s,0);
        app.name=utf16(sqlite3_column_text(q.s,1));
        app.path=utf16(sqlite3_column_text(q.s,2));
        app.observed_seconds=sqlite3_column_double(q.s,3);
        app.last_seen_ms=sqlite3_column_int64(q.s,4);
        if (!include_system && statistics_is_system(app.name,app.path)) continue;
        out.push_back(std::move(app));
        if (out.size()>=limit) break;
    }
    return out;
}
}
bool statistics_is_system(std::wstring_view name,std::wstring_view path) {
    const auto lower_name=lowercase(std::wstring(name));
    for (const auto* known: {L"system",L"registry",L"idle",L"system idle process",L"[system process]",
         L"svchost.exe",L"csrss.exe",L"smss.exe",L"wininit.exe",L"winlogon.exe",
         L"services.exe",L"lsass.exe",L"dwm.exe",L"fontdrvhost.exe"})
        if (lower_name==known) return true;
    // A Windows directory query per running application per sample is wasteful.
    static const std::wstring directory=[] {
        wchar_t windows[MAX_PATH]{};
        const UINT length=GetWindowsDirectoryW(windows,MAX_PATH);
        return length && length<MAX_PATH ? lowercase(std::wstring(windows,length)) : std::wstring{};
    }();
    if (!directory.empty()) {
        const auto application=lowercase(std::wstring(path));
        return application.size()>directory.size() && application.starts_with(directory)
            && (application[directory.size()]==L'\\' || application[directory.size()]==L'/');
    }
    return false;
}
StatisticsHistory read_statistics(const std::filesystem::path& file,
    std::int64_t from_ms,std::int64_t to_ms,unsigned limit,bool include_system,std::stop_token stop) {
    StatisticsHistory out;
    if (stop.stop_requested()) return out;
    if (file.empty() || !std::filesystem::exists(file)) return out;
    try {
        Database connection(file,&stop);
        Statement bounds(connection.db,"SELECT MIN(timestamp_ms),MAX(timestamp_ms) FROM snapshots");
        if (bounds.next()!=SQLITE_ROW || sqlite3_column_type(bounds.s,0)==SQLITE_NULL) return out;
        out.first_record_ms=sqlite3_column_int64(bounds.s,0);
        out.last_record_ms=sqlite3_column_int64(bounds.s,1);
        from_ms=std::max(from_ms,out.first_record_ms);
        if (to_ms<=from_ms) return out;
        Statement available(connection.db,"SELECT count(*) FROM sqlite_master WHERE type='table' AND name='application_scores'");
        out.scores_available=available.next()==SQLITE_ROW && sqlite3_column_int(available.s,0)>0;
        limit=std::clamp(limit,1U,100U);
        out.uptime=select_top(connection.db,from_ms,to_ms,limit,include_system,false,stop);
        if (out.scores_available && !stop.stop_requested()) {
            // A maximum of ~480 time buckets per line; no large raw history buffers.
            const auto bucket=std::max<std::int64_t>(1000,(to_ms-from_ms+479)/480);
            auto ranked=select_top(connection.db,from_ms,to_ms,limit,include_system,true,stop);
            out.history=graph(connection.db,ranked,from_ms,to_ms,bucket,stop);
            const auto now=std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            const auto minute_from=std::max(out.first_record_ms,now-60000);
            const auto minute_to=now+1000;
            auto recent=select_top(connection.db,minute_from,minute_to,limit,include_system,true,stop);
            out.recent=graph(connection.db,recent,minute_from,minute_to,1000,stop);
        }
    } catch (const std::exception& error) {
        if (!stop.stop_requested()) out.error=utf16(reinterpret_cast<const unsigned char*>(error.what()));
    }
    return out;
}
}