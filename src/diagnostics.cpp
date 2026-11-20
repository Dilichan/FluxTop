#include "fluxtop/diagnostics.hpp"
#include <windows.h>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cwchar>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace fluxtop::diagnostics {
namespace {
constexpr std::uint64_t mib=1024ULL*1024;
constexpr std::uint64_t max_segment_bytes=100ULL*mib;
constexpr unsigned max_segments=5;
constexpr std::size_t max_pending_bytes=256*1024;
constexpr std::size_t max_message_bytes=4096;

std::filesystem::path journal_path() {
    wchar_t buffer[32768]{};
    auto n=GetEnvironmentVariableW(L"FLUXTOP_LOG_PATH",buffer,32768);
    if(n && n<32768) return std::filesystem::absolute(buffer);
    n=GetEnvironmentVariableW(L"LOCALAPPDATA",buffer,32768);
    if(!n || n>=32768) return {};
    return std::filesystem::path(buffer)/L"FluxTop"/L"activity.log";
}
std::string cleaned(std::string_view input,std::size_t limit) {
    std::string result;
    result.reserve(std::min(input.size(),limit));
    for(char ch:input) {
        if(result.size()>=limit) break;
        if(ch=='\n'||ch=='\r'||ch=='\t') result.push_back(' ');
        else if(static_cast<unsigned char>(ch)>=32) result.push_back(ch);
    }
    return result;
}
std::string event_line(Level level,std::string_view category,std::string_view message) {
    // Timestamp uses the Windows local clock; include the UTC offset so daylight
    // saving changes remain unambiguous and older UTC ('Z') records stay readable.
    SYSTEMTIME utc{},now{};
    GetSystemTime(&utc);
    if(!SystemTimeToTzSpecificLocalTime(nullptr,&utc,&now))GetLocalTime(&now);
    FILETIME utc_file{},local_file{};
    int offset_minutes=0;
    if(SystemTimeToFileTime(&utc,&utc_file) && SystemTimeToFileTime(&now,&local_file)){
        ULARGE_INTEGER utc_ticks{},local_ticks{};
        utc_ticks.LowPart=utc_file.dwLowDateTime;utc_ticks.HighPart=utc_file.dwHighDateTime;
        local_ticks.LowPart=local_file.dwLowDateTime;local_ticks.HighPart=local_file.dwHighDateTime;
        constexpr std::int64_t ticks_per_minute=600000000LL;
        const auto delta=static_cast<std::int64_t>(local_ticks.QuadPart)-static_cast<std::int64_t>(utc_ticks.QuadPart);
        offset_minutes=static_cast<int>(delta/ticks_per_minute);
    }
    const int absolute_offset=offset_minutes<0?-offset_minutes:offset_minutes;
    const char offset_sign=offset_minutes<0?'-':'+';
    char prefix[116]{};
    const char* severity=level==Level::error?"ERROR":level==Level::warning?"WARN":level==Level::trace?"TRACE":"INFO";
    std::snprintf(prefix,sizeof(prefix),"%04u-%02u-%02uT%02u:%02u:%02u.%03u%c%02d:%02d pid=%lu tid=%lu [%s] ",
        static_cast<unsigned>(now.wYear),static_cast<unsigned>(now.wMonth),
        static_cast<unsigned>(now.wDay),static_cast<unsigned>(now.wHour),
        static_cast<unsigned>(now.wMinute),static_cast<unsigned>(now.wSecond),
        static_cast<unsigned>(now.wMilliseconds),offset_sign,absolute_offset/60,absolute_offset%60,
        static_cast<unsigned long>(GetCurrentProcessId()),static_cast<unsigned long>(GetCurrentThreadId()),severity);
    std::string result(prefix);
    result+=cleaned(category,72);
    result+=" | ";
    result+=cleaned(message,max_message_bytes);
    result+='\n';
    return result;
}
std::filesystem::path segment_name(const std::filesystem::path& path,unsigned index) {
    if(!index) return path;
    auto name=path;name+=L"."+std::to_wstring(index);return name;
}
std::uint64_t existing_bytes(const std::filesystem::path& path) {
    std::error_code ec;
    const auto size=std::filesystem::file_size(path,ec);
    return ec?0:size;
}
// A small segment size may be requested only by regression tests, while
// production runs always retain at most five 100 MiB files.
std::uint64_t segment_budget() {
    wchar_t buffer[32]{};
    const auto n=GetEnvironmentVariableW(L"FLUXTOP_LOG_TEST_SEGMENT_KIB",buffer,32);
    if(n && n<32) {
        wchar_t* end=nullptr;
        const unsigned long amount=std::wcstoul(buffer,&end,10);
        if(end && *end==L'\0' && amount>=64 && amount<=1024)
            return static_cast<std::uint64_t>(amount)*1024ULL;
    }
    return max_segment_bytes;
}
void rotate(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::remove(segment_name(path,max_segments-1),ec);
    for(int i=static_cast<int>(max_segments)-2;i>=1;--i) {
        const auto source=segment_name(path,static_cast<unsigned>(i));
        if(std::filesystem::exists(source,ec))
            std::filesystem::rename(source,segment_name(path,static_cast<unsigned>(i+1)),ec);
        if(ec) return;
    }
    if(std::filesystem::exists(path,ec))
        std::filesystem::rename(path,segment_name(path,1),ec);
}

class Journal {
public:
    Journal():path_(journal_path()),worker_([this]{run();}){}
    Journal(const Journal&)=delete;
    Journal& operator=(const Journal&)=delete;
    ~Journal() {
        {std::lock_guard lock(mutex_);closing_=true;}
        ready_.notify_one();
        if(worker_.joinable())worker_.join();
    }
    void enqueue(Level level,std::string_view category,std::string_view message) noexcept {
        try {
            auto line=event_line(level,category,message);
            {
                std::lock_guard lock(mutex_);
                if(closing_ || line.size()+pending_bytes_>max_pending_bytes) {++dropped_;return;}
                pending_bytes_+=line.size();pending_.push_back(std::move(line));
            }
            ready_.notify_one();
        }catch(...){}
    }
    void flush_now() noexcept {
        std::unique_lock lock(mutex_);
        drained_.wait_for(lock,std::chrono::seconds(5),[this]{return pending_.empty()&&!writing_;});
    }
private:
    void write_batch(const std::deque<std::string>& lines,std::uint64_t dropped) noexcept {
        if(path_.empty())return;
        try {
            std::error_code ec;
            std::filesystem::create_directories(path_.parent_path(),ec);
            if(ec)return;
            HANDLE mutex=CreateMutexW(nullptr,FALSE,L"Local\\FluxTop.ActivityLog.v1");
            if(!mutex)return;
            const DWORD result=WaitForSingleObject(mutex,5000);
            if(result!=WAIT_OBJECT_0 && result!=WAIT_ABANDONED){CloseHandle(mutex);return;}
            const auto unlock=[&]{ReleaseMutex(mutex);CloseHandle(mutex);};
            try {
                std::string overflow;
                if(dropped)overflow=event_line(Level::warning,"log.backpressure","dropped="+std::to_string(dropped));
                auto budget=segment_budget();
                auto current=existing_bytes(path_);
                std::ofstream stream;
                auto put=[&](const std::string& line){
                    if(line.size()>budget)return;
                    if(current+line.size()>budget){
                        if(stream.is_open())stream.close();
                        rotate(path_);current=existing_bytes(path_);
                    }
                    if(!stream.is_open())stream.open(path_,std::ios::binary|std::ios::app);
                    if(stream){stream.write(line.data(),static_cast<std::streamsize>(line.size()));current+=line.size();}
                };
                if(!overflow.empty())put(overflow);
                for(const auto& line:lines)put(line);
            }catch(...){}
            unlock();
        }catch(...){}
    }
    void run() noexcept {
        for(;;) {
            std::deque<std::string> batch;
            std::uint64_t dropped=0;
            {
                std::unique_lock lock(mutex_);
                ready_.wait_for(lock,std::chrono::seconds(2),[this]{return closing_||!pending_.empty();});
                if(closing_&&pending_.empty())break;
                batch.swap(pending_);
                pending_bytes_=0;
                writing_=true;
                dropped=std::exchange(dropped_,0);
            }
            write_batch(batch,dropped);
            {std::lock_guard lock(mutex_);writing_=false;}
            drained_.notify_all();
        }
    }
    std::filesystem::path path_;
    std::mutex mutex_;
    std::condition_variable ready_;
    std::condition_variable drained_;
    std::deque<std::string> pending_;
    std::size_t pending_bytes_{};
    std::uint64_t dropped_{};
    bool closing_{},writing_{};
    std::thread worker_;
};
Journal& instance(){static Journal journal;return journal;}
}
void write(Level level,std::string_view category,std::string_view message) noexcept {
    try {instance().enqueue(level,category,message);}catch(...){}
}
void flush() noexcept {try{instance().flush_now();}catch(...){}}
} // namespace fluxtop::diagnostics
