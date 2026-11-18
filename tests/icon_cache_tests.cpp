#include "fluxtop/app_icons.hpp"
#include "fluxtop/windows.hpp"
#include <winsqlite/winsqlite3.h>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <stdexcept>

namespace {
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
}
int wmain(int argc, wchar_t** argv) {
    try {
        check(argc == 2, "Expected build directory");
        const auto path = std::filesystem::path(argv[1]) / (L"icon-cache-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()) + L".db");
        FILETIME created{}, exited{}, kernel{}, user{};
        check(GetProcessTimes(GetCurrentProcess(),&created,&exited,&kernel,&user)!=FALSE,"Cannot read own identity");
        const auto identity = fluxtop::filetime_ticks(created);
        std::mutex mutex;
        std::condition_variable ready;
        const auto notify = [&] { ready.notify_all(); };
        {
            fluxtop::AppIconCache cache(path,notify);
            cache.request(GetCurrentProcessId(),identity);
            std::unique_lock lock(mutex);
            check(ready.wait_for(lock,std::chrono::seconds(10),[&] { return cache.get(GetCurrentProcessId(),identity)!=nullptr; }),"Icon extraction timed out");
            const auto icon = cache.get(GetCurrentProcessId(),identity);
            std::size_t alpha{};
            for (std::size_t i=3;i<icon->pixels.size();i+=4) if (icon->pixels[i]!=std::byte{}) ++alpha;
            check(alpha>0 && alpha<1024,"Icon must preserve transparent and visible pixels");
            check(cache.stats().persistent,"Persistent cache was not opened");
            for (unsigned batch=0;batch<4;++batch) {
                for (unsigned i=0;i<20;++i) cache.request(1000000+batch*20+i,0);
                check(ready.wait_for(lock,std::chrono::seconds(10),[&] { return cache.stats().pending==0; }),"Negative icon batch timed out");
                check(cache.stats().entries<=64,"Memory cache exceeded its bound");
            }
            const auto count = cache.stats().extractions;
            cache.request(1000000+3*20+19,0);
            check(cache.stats().extractions==count,"Negative result was not cached");
            cache.set_capacity(116); // 100 visible applications + 16 slots for churn.
            check(cache.stats().capacity==116,"Adaptive icon capacity is wrong");
            for (unsigned i=0;i<100;++i) cache.request(2000000+i,0);
            check(ready.wait_for(lock,std::chrono::seconds(20),[&] { return cache.stats().pending==0; }),
                "Adaptive icon queue timed out");
            check(cache.stats().entries>=100 && cache.stats().entries<=116,
                "Adaptive cache could not keep 100 applications");
            cache.set_capacity(32);
            check(cache.stats().capacity==32 && cache.stats().entries<=32,
                "Shrinking the icon cache did not release old entries");
        }
        {
            fluxtop::AppIconCache cache(path,notify);
            cache.request(GetCurrentProcessId(),identity);
            std::unique_lock lock(mutex);
            check(ready.wait_for(lock,std::chrono::seconds(10),[&] { return cache.get(GetCurrentProcessId(),identity)!=nullptr; }),"Persistent reload timed out");
            check(cache.stats().disk_hits==1 && cache.stats().extractions==0,"Second session did not reuse the stored PNG");
        }
        // Retain an archive larger than both former disk limits, then exercise
        // the real cache writer. Only the private test DB is modified here.
        {
            sqlite3* seed{};
            const auto name=fluxtop::utf8(path.c_str());
            check(sqlite3_open(name.c_str(),&seed)==SQLITE_OK,"Cannot seed archive fixture");
            check(sqlite3_exec(seed,"DELETE FROM icons; BEGIN",nullptr,nullptr,nullptr)==SQLITE_OK,"Cannot start archive fixture");
            sqlite3_stmt* insert{};
            check(sqlite3_prepare_v2(seed,"INSERT INTO icons(exe,stamp,bytes,png,used) VALUES(?1,0,0,zeroblob(4096),0)",-1,&insert,nullptr)==SQLITE_OK,"Cannot prepare archive fixture");
            for (int i=0;i<600;++i) {
                const auto archived="archived-app-"+std::to_string(i);
                sqlite3_bind_text(insert,1,archived.c_str(),-1,SQLITE_TRANSIENT);
                check(sqlite3_step(insert)==SQLITE_DONE,"Cannot seed archived icon");
                sqlite3_reset(insert);
            }
            sqlite3_finalize(insert);
            check(sqlite3_exec(seed,"COMMIT",nullptr,nullptr,nullptr)==SQLITE_OK,"Cannot commit archive fixture");
            sqlite3_close(seed);
            check(std::filesystem::file_size(path)>2*1024*1024,"Fixture did not exceed the old 2 MiB cap");
        }
        {
            fluxtop::AppIconCache cache(path,notify);
            cache.request(GetCurrentProcessId(),identity);
            std::unique_lock lock(mutex);
            check(ready.wait_for(lock,std::chrono::seconds(10),[&] { return cache.get(GetCurrentProcessId(),identity)!=nullptr; }),"New icon extraction after archive timed out");
            check(cache.stats().extractions==1,"Fixture must exercise a new DB write");
        }
        {
            fluxtop::AppIconCache cache(path,notify);
            cache.request(GetCurrentProcessId(),identity);
            std::unique_lock lock(mutex);
            check(ready.wait_for(lock,std::chrono::seconds(10),[&] { return cache.get(GetCurrentProcessId(),identity)!=nullptr; }),"Archive reload timed out");
            check(cache.stats().disk_hits==1 && cache.stats().extractions==0,"New icon was not persisted beyond the old disk cap");
        }
        sqlite3* db{};
        const auto name=fluxtop::utf8(path.c_str());
        check(sqlite3_open_v2(name.c_str(),&db,SQLITE_OPEN_READONLY,nullptr)==SQLITE_OK,"Cannot inspect cache DB");
        sqlite3_stmt* statement{};
        check(sqlite3_prepare_v2(db,"SELECT count(*),max(length(png)) FROM icons",-1,&statement,nullptr)==SQLITE_OK,"Cannot query icon records");
        check(sqlite3_step(statement)==SQLITE_ROW && sqlite3_column_int(statement,0)==601 && sqlite3_column_int(statement,1)<=16384,"Archived records were pruned while adding a new icon");
        sqlite3_finalize(statement); sqlite3_close(db);
        std::cout << "Icon extraction, bounded RAM, persistence and archive beyond former disk limits passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
