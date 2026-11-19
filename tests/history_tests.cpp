#include "fluxtop/history.hpp"
#include "fluxtop/history_math.hpp"
#include <winsqlite/winsqlite3.h>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
void check(bool condition,const char* what){if(!condition)throw std::runtime_error(what);}
struct Database {
    sqlite3* db{};
    explicit Database(const std::filesystem::path& p){
        const auto u=p.u8string();const std::string filename(reinterpret_cast<const char*>(u.data()),u.size());
        check(sqlite3_open(filename.c_str(),&db)==SQLITE_OK,"open fixture");
    }
    ~Database(){if(db)sqlite3_close(db);}
    std::int64_t integer(const char* sql){
        sqlite3_stmt* s{};check(sqlite3_prepare_v2(db,sql,-1,&s,nullptr)==SQLITE_OK,"prepare scalar");
        const int status=sqlite3_step(s);
        const auto answer=sqlite3_column_int64(s,0);
        sqlite3_finalize(s);check(status==SQLITE_ROW,"step scalar");return answer;
    }
    double real(const char* sql){
        sqlite3_stmt* s{};check(sqlite3_prepare_v2(db,sql,-1,&s,nullptr)==SQLITE_OK,"prepare real");
        const int status=sqlite3_step(s);const double answer=sqlite3_column_double(s,0);
        sqlite3_finalize(s);check(status==SQLITE_ROW,"step real");return answer;
    }
};
}
int main(int argc,char** argv){
    try {
        check(argc==2,"fixture directory required");
        using namespace fluxtop;
        // Two non-uniform batches: mean of means is 25; correct weighted mean is 30.
        WeightedValue first,second;
        first.add(10.,1000);first.add(30.,3000);second.add(40.,2000);first.merge(second);
        check(first.duration_ms==6000 && first.samples==3,"exact weighted sample duration/count");
        check(std::abs(*first.average()-30.)<1.e-10,"merge weighted numerators, never averages");
        check(first.minimum==10. && first.maximum==40.,"min/max preserved");
        check(history_floor(-1,history_minute_ms)==-history_minute_ms,"negative UTC boundary");
        const auto root=std::filesystem::path(argv[1])/
            ("history-v10-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(root);
        const auto path=root/L"история.db";
        const std::int64_t base=history_floor(1'900'000'000'000LL,history_day_ms);
        auto write=[&](HistoryWriter& writer,std::int64_t timestamp,double cpu){
            Snapshot snap;snap.timestamp_ms=timestamp;snap.interval_ms=1000;
            Process a;a.pid=700;a.creation_time=777;a.name="Game.exe";
            a.exe_path="C:\\Games\\Game.exe";a.cpu_percent=cpu;a.working_set=16*1024*1024;
            snap.processes={a};writer.append(snap);
            check(snap.history_error.empty(),"no history writer error");
        };
        {
            HistoryWriter writer(path);
            write(writer,base+10*history_hour_ms+59*history_minute_ms+58'000,20);
            write(writer,base+10*history_hour_ms+59*history_minute_ms+59'000,40);
            write(writer,base+11*history_hour_ms+1'000,60);
            // Application is absent for multiple days. Must not create fictitious uptime.
            write(writer,base+3*history_day_ms+1'000,80);
            write(writer,base+4*history_day_ms+1'000,100);
        }
        {
            Database db(path);
            check(db.integer("PRAGMA user_version")==2,"conveyor schema v2");
            check(db.integer("SELECT COUNT(*) FROM history_rollups")>0,"aggregates saved");
            check(db.integer("SELECT SUM(uptime_ms) FROM history_rollups")==5000,"uptime matches observed intervals only");
            check(db.integer("SELECT SUM(score_duration_ms) FROM history_rollups")==5000,"no lost metric durations");
            check(db.integer("SELECT COUNT(*) FROM history_rollups WHERE level=2")>0,"closed days compacted");
            check(db.integer("SELECT COUNT(*) FROM history_rollups WHERE level=0")<=1,"seconds are never persisted");
            check(db.integer("SELECT COUNT(*) FROM applications")==1,"one stable application identity");
        }
        {
            HistoryWriter writer(path);
            write(writer,base+7*history_day_ms+1'000,30);
        }
        {
            Database db(path);
            check(db.integer("SELECT SUM(uptime_ms) FROM history_rollups")==6000,"resume after downtime");
            check(db.integer("SELECT COUNT(*) FROM history_rollups WHERE level=2")>0,"daily data still available");
        }
        std::filesystem::remove_all(root);
        std::cout<<"Constant-space weighted aggregation, minute/hour/day conveyor, restart, gaps and WAL fixture passed\n";
        return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
