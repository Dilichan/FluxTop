#include "fluxtop/history.hpp"
#include "fluxtop/windows.hpp"
#include <winsqlite/winsqlite3.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
using namespace std::chrono_literals;
namespace {
void need(bool value,const char* text){if(!value)throw std::runtime_error(text);}
struct Database {
    sqlite3* db{};
    explicit Database(const std::filesystem::path& path){auto u=path.u8string();need(sqlite3_open(reinterpret_cast<const char*>(u.c_str()),&db)==SQLITE_OK,"open history");sqlite3_busy_timeout(db,5000);}
    ~Database(){sqlite3_close(db);}
    std::string scalar(const char* sql){sqlite3_stmt* s{};need(sqlite3_prepare_v2(db,sql,-1,&s,nullptr)==SQLITE_OK,"prepare history query");
        const auto code=sqlite3_step(s);std::string result;
        if(code==SQLITE_ROW && sqlite3_column_text(s,0))result=reinterpret_cast<const char*>(sqlite3_column_text(s,0));
        sqlite3_finalize(s);need(code==SQLITE_ROW,"query history");return result;}
};
void append(fluxtop::HistoryWriter& writer,int minute){
    fluxtop::Snapshot s;s.timestamp_ms=1'900'000'020'000LL+minute*60'000;s.interval_ms=1000;
    fluxtop::Process p;p.pid=54;p.creation_time=54;p.exe_path="C:\\fixture\\history.exe";p.name="history.exe";p.cpu_percent=25;s.processes.push_back(p);
    writer.append(s);need(s.history_error.empty(),"writer reports failure");
}
}
int wmain(int argc,wchar_t** argv){try{
    need(argc>=2,"fixture directory required");
    if(argc==4 && std::wstring_view(argv[1])==L"--child"){
        fluxtop::HistoryWriter writer(argv[2]);
        for(int i=0;i<5;++i)append(writer,i);
        // Poll committed storage, never rely on a timing-only ready marker.
        for(int i=0;i<200;++i){
            if(std::filesystem::exists(argv[2])){Database db(argv[2]);
                if(db.scalar("SELECT COUNT(*) FROM sqlite_master WHERE name='history_rollups'")=="1" &&
                    db.scalar("SELECT SUM(uptime_ms) FROM history_rollups")=="4000"){
                    std::ofstream(argv[3])<<"committed";Sleep(INFINITE);
                }}
            std::this_thread::sleep_for(25ms);
        }return 2;
    }
    const auto root=std::filesystem::path(argv[1])/(L"history-crash-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
    std::filesystem::create_directories(root);const auto path=root/L"history.db",marker=root/L"ready";
    wchar_t exe[32768]{};need(GetModuleFileNameW(nullptr,exe,32768)>0,"fixture executable");
    auto command=L"\""+std::wstring(exe)+L"\" --child \""+path.wstring()+L"\" \""+marker.wstring()+L"\"";
    STARTUPINFOW start{};start.cb=sizeof(start);PROCESS_INFORMATION child{};
    need(CreateProcessW(exe,command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&start,&child)!=FALSE,"start owned history writer");
    struct Cleanup{PROCESS_INFORMATION& p;~Cleanup(){if(WaitForSingleObject(p.hProcess,0)==WAIT_TIMEOUT){TerminateProcess(p.hProcess,54);WaitForSingleObject(p.hProcess,5000);}CloseHandle(p.hProcess);CloseHandle(p.hThread);}}cleanup{child};
    for(int i=0;i<300 && !std::filesystem::exists(marker);++i){need(WaitForSingleObject(child.hProcess,0)==WAIT_TIMEOUT,"writer exited before commit");std::this_thread::sleep_for(25ms);}
    need(std::filesystem::exists(marker),"wait for committed history");
    const auto wal=std::filesystem::path(path.wstring()+L"-wal");need(std::filesystem::exists(wal) && std::filesystem::file_size(wal)>0,"writer uses WAL");
    need(TerminateProcess(child.hProcess,54)!=FALSE && WaitForSingleObject(child.hProcess,5000)==WAIT_OBJECT_0,"crash owned writer");
    {Database db(path);need(db.scalar("PRAGMA integrity_check")=="ok","WAL recovery integrity");
        need(db.scalar("SELECT SUM(uptime_ms) FROM history_rollups")=="4000","all committed samples survive, pending minute is not fabricated");}
    {Database pinned(path);need(sqlite3_exec(pinned.db,"BEGIN",nullptr,nullptr,nullptr)==SQLITE_OK,"hold reader transaction");
    need(pinned.scalar("SELECT SUM(uptime_ms) FROM history_rollups")=="4000","pin the pre-restart read snapshot");
    {fluxtop::HistoryWriter writer(path);for(int i=5;i<65;++i){append(writer,i);
        if(i>5){bool committed=false;for(int attempt=0;attempt<200 && !committed;++attempt){
            Database db(path);committed=db.scalar("SELECT SUM(uptime_ms) FROM history_rollups")==std::to_string(4000+(i-5)*1000);
            if(!committed)std::this_thread::sleep_for(5ms);
        }need(committed,"continuous writer commits each closed minute");}
    }}
    {Database db(path);need(db.scalar("PRAGMA integrity_check")=="ok","integrity after restart");
        need(db.scalar("SELECT SUM(uptime_ms) FROM history_rollups")=="64000","history resumes without duplicated or missing committed intervals");
        int log{},done{};need(sqlite3_wal_checkpoint_v2(db.db,nullptr,SQLITE_CHECKPOINT_PASSIVE,&log,&done)==SQLITE_OK && log>done,
            "active reader prevents complete checkpoint without blocking writer");
        need(sqlite3_exec(pinned.db,"COMMIT",nullptr,nullptr,nullptr)==SQLITE_OK,"release pinned reader");
        need(sqlite3_wal_checkpoint_v2(db.db,nullptr,SQLITE_CHECKPOINT_TRUNCATE,&log,&done)==SQLITE_OK,"WAL truncates after readers close");
        need(!std::filesystem::exists(wal) || std::filesystem::file_size(wal)==0,"no leftover WAL after truncate");}}
    std::filesystem::remove_all(root);std::cout<<"History crash, WAL recovery, resume, checkpoint passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
