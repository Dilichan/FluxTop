#include "fluxtop/app_icons.hpp"
#include "fluxtop/history.hpp"
#include "fluxtop/windows.hpp"
#include <windows.h>
#include <winsqlite/winsqlite3.h>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <stdexcept>

namespace {
void require(bool valid,const char* reason){if(!valid)throw std::runtime_error(reason);}
int count(sqlite3* db,const char* sql){
    sqlite3_stmt* s{};
    require(sqlite3_prepare_v2(db,sql,-1,&s,nullptr)==SQLITE_OK,"prepare schema assertion");
    const int rc=sqlite3_step(s);
    const int result=rc==SQLITE_ROW?sqlite3_column_int(s,0):-1;
    sqlite3_finalize(s);
    require(rc==SQLITE_ROW,"read schema assertion");
    return result;
}
}
int wmain(int argc,wchar_t** argv){
    try{
        require(argc==2,"fixture root required");
        const auto root=std::filesystem::path(argv[1])/
            (L"unified-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
        std::filesystem::create_directories(root);
        const auto path=root/L"fluxtop.db";
        // Icon cache can be the first component to create the shared database.
        const auto executable=fluxtop::process_image_path(GetCurrentProcess());
        require(!executable.empty(),"fixture executable");
        std::mutex mutex;
        std::condition_variable cv;
        {
            fluxtop::AppIconCache cache(path,[&]{cv.notify_all();});
            cache.request(executable);
            std::unique_lock lock(mutex);
            require(cv.wait_for(lock,std::chrono::seconds(10),[&]{return cache.get(executable)!=nullptr;}),"icon in shared DB");
            require(cache.stats().persistent,"persistent cache open");
        }
        {
            fluxtop::HistoryWriter history(path);
            fluxtop::Snapshot s;
            s.interval_ms=1000;
            s.total_ram_capacity=16ULL*1024*1024*1024;
            s.total_gpu_memory_capacity=8ULL*1024*1024*1024;
            fluxtop::Process p;
            p.pid=10001;p.creation_time=1;p.name="fixture.exe";p.exe_path="C:\\Apps\\fixture.exe";
            p.cpu_percent=5;p.working_set=1048576;
            s.processes={p};
            s.timestamp_ms=1'900'000'000'000LL;history.append(s);
            s.timestamp_ms+=60'000;history.append(s);
        }
        sqlite3* db{};
        const auto name=fluxtop::utf8(path.c_str());
        require(sqlite3_open_v2(name.c_str(),&db,SQLITE_OPEN_READONLY,nullptr)==SQLITE_OK,"open shared database");
        require(count(db,"PRAGMA user_version")==2,"expected schema 2");
        require(count(db,"SELECT COUNT(*) FROM sqlite_master WHERE name IN ('icons','history_rollups','applications','history_state')")==4,
            "one database must contain all four tables");
        require(count(db,"SELECT COUNT(*) FROM icons")>=1,"persisted icons missing");
        require(count(db,"SELECT COUNT(*) FROM history_rollups")>=1,"persisted statistics missing");
        sqlite3_close(db);
        require(!std::filesystem::exists(root/L"icons.db"),"second database must not be created");
        std::cout<<"One unified SQLite database, icon-first initialization and history write passed\n";
        return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
