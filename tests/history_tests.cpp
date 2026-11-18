#include "fluxtop/history.hpp"
#include <winsqlite/winsqlite3.h>
#include <chrono>
#include <iostream>
#include <stdexcept>

namespace {
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
struct Database {
    sqlite3* value{};
    explicit Database(const std::filesystem::path& path) {
        const auto utf = path.u8string();
        check(sqlite3_open(reinterpret_cast<const char*>(utf.c_str()), &value)==SQLITE_OK, "open fixture");
    }
    ~Database() { sqlite3_close(value); }
    std::int64_t scalar(const char* sql) {
        sqlite3_stmt* statement{};
        check(sqlite3_prepare_v2(value,sql,-1,&statement,nullptr)==SQLITE_OK,"prepare fixture query");
        const auto status=sqlite3_step(statement);
        const auto result=sqlite3_column_int64(statement,0); sqlite3_finalize(statement);
        check(status==SQLITE_ROW,"fixture scalar row"); return result;
    }
};
}
int main(int argc, char** argv) {
    try {
        check(argc==2,"fixture directory required");
        const auto directory=std::filesystem::path(argv[1])/ ("history-test-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(directory);
        const auto path=directory/L"история.db";
        fluxtop::Snapshot s; s.timestamp_ms=1700000000000; s.interval_ms=1000; s.network_status="ready"; s.gpu_status="ready";
        fluxtop::Process a,b;
        a.pid=1; a.creation_time=10; a.name="same.exe"; a.exe_path="c:\\one\\same.exe"; a.cpu_percent=2; a.working_set=1048576; a.rx_bytes_per_second=12.5;
        b.pid=2; b.creation_time=11; b.name="same.exe"; b.exe_path="c:\\two\\same.exe"; b.working_set=0;
        s.processes={a,b};
        {
            fluxtop::HistoryWriter writer(path); writer.append(s);
            check(s.history_error.empty() && s.history_records==2,"all processes committed");
            s.timestamp_ms+=1000; s.processes[0].creation_time=20; writer.append(s);
            check(s.history_error.empty() && s.history_records==4,"PID reuse stored separately");
        }
        s.timestamp_ms+=1000; s.processes.resize(1);
        {
            fluxtop::HistoryWriter writer(path); writer.append(s);
            check(s.history_error.empty() && s.history_records==5,"counter survives restart");
            Database db(path);
            check(db.scalar("SELECT COUNT(*) FROM applications")==2,"equal EXE names at different paths are distinct apps");
            check(db.scalar("SELECT COUNT(*) FROM process_instances")==3,"stable process identities survive reopen and PID reuse");
            check(db.scalar("SELECT COUNT(*) FROM measurements WHERE cpu_percent IS NULL AND working_set_bytes=0")==2,"unknown is NULL, zero remains zero");
            check(db.scalar("SELECT COUNT(*) FROM measurements WHERE rx_bytes_per_second=12.5")==3,"network values persisted");
            s.processes.push_back(s.processes[0]); writer.append(s);
            check(!s.history_error.empty(),"duplicate interval row fails visibly");
            check(db.scalar("SELECT COUNT(*) FROM snapshots")==3 && db.scalar("SELECT COUNT(*) FROM measurements")==5
                && db.scalar("SELECT measurement_count FROM history_state")==5,"failed transaction preserves data and counter atomically");
            s.processes.resize(1); s.timestamp_ms+=1000; writer.append(s);
            check(s.history_error.empty() && s.history_records==6,"writing recovers after rollback");
            s.processes.clear();
            for (unsigned i=0;i<1000;++i) { a.pid=100+i; a.creation_time=1000+i; s.processes.push_back(a); }
            s.timestamp_ms+=1000; writer.append(s);
            check(s.history_error.empty() && s.history_records==1006,"history has no TOP or application-count cap");
            check(db.scalar("SELECT COUNT(*) FROM measurements")==1006,"old samples retained");
            check(sqlite3_exec(db.value,"BEGIN IMMEDIATE",nullptr,nullptr,nullptr)==SQLITE_OK,"lock fixture");
            writer.append(s); check(!s.history_error.empty(),"database lock reported");
            sqlite3_exec(db.value,"ROLLBACK",nullptr,nullptr,nullptr);
            check(db.scalar("SELECT measurement_count FROM history_state")==1006,"lock cannot inflate committed count");
            writer.append(s); check(s.history_error.empty() && s.history_records==2006,"writer recovers after contention");
        }
        fluxtop::HistoryWriter invalid(path/L"not-a-directory.db"); invalid.append(s);
        check(!s.history_error.empty(),"invalid destination is visible without losing old database");
        std::cout<<"History persistence, identity, NULL, unlimited rows, rollback and contention checks passed\n";
        return 0;
    } catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
