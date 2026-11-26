#include "fluxtop/etw_session.hpp"
#include "fluxtop/windows.hpp"
#include <objbase.h>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cwchar>
#include <limits>
#include <vector>

namespace fluxtop {
namespace {
constexpr std::wstring_view prefix(EtwSessionKind kind) noexcept {
    if(kind==EtwSessionKind::presentation)return L"FluxTop.Presentation.";
    return kind==EtwSessionKind::network?L"FluxTop.Network.":L"FluxTop.Disk.";
}
template<class Integer>
std::optional<Integer> parse_number(std::wstring_view text,unsigned base) noexcept {
    if(text.empty())return {};
    Integer value=0;
    for(const auto ch:text) {
        unsigned digit=base;
        if(ch>=L'0' && ch<=L'9')digit=static_cast<unsigned>(ch-L'0');
        else if(ch>=L'a' && ch<=L'f')digit=static_cast<unsigned>(ch-L'a')+10;
        else if(ch>=L'A' && ch<=L'F')digit=static_cast<unsigned>(ch-L'A')+10;
        if(digit>=base || value>(std::numeric_limits<Integer>::max()-digit)/base)return {};
        value=value*base+digit;
    }
    return value;
}
bool owner_gone(const EtwSessionOwner& identity) noexcept {
    Handle owner(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,identity.pid));
    if(!owner.valid())return GetLastError()==ERROR_INVALID_PARAMETER;
    DWORD exit_code{};
    if(GetExitCodeProcess(owner.get(),&exit_code) && exit_code!=STILL_ACTIVE)return true;
    FILETIME created{},exited{},kernel{},user{};
    return GetProcessTimes(owner.get(),&created,&exited,&kernel,&user) &&
        filetime_ticks(created)!=identity.creation_time;
}
void reclaim_sessions(EtwSessionKind kind) {
    struct Entry { EVENT_TRACE_PROPERTIES data{}; wchar_t name[1024]{}; };
    std::array<EVENT_TRACE_PROPERTIES*,64> pointers{};
    std::vector<Entry> entries(pointers.size());
    for(std::size_t i=0;i<pointers.size();++i) {
        auto& data=entries[i].data;
        data.Wnode.BufferSize=sizeof(Entry);
        data.LoggerNameOffset=offsetof(Entry,name);
        pointers[i]=&data;
    }
    ULONG count=0;
    if(QueryAllTracesW(pointers.data(),static_cast<ULONG>(pointers.size()),&count)!=ERROR_SUCCESS)return;
    for(std::size_t i=0;i<std::min<std::size_t>(count,entries.size());++i) {
        auto& entry=entries[i];
        const auto offset=entry.data.LoggerNameOffset;
        if(offset<offsetof(Entry,name) || offset>=sizeof(Entry) || offset%alignof(wchar_t))continue;
        const auto* name=reinterpret_cast<const wchar_t*>(reinterpret_cast<const std::byte*>(&entry)+offset);
        const auto* end=std::wmemchr(name,L'\0',(sizeof(Entry)-offset)/sizeof(wchar_t));
        if(!end)continue;
        const auto owner=parse_etw_session_owner(kind,{name,static_cast<std::size_t>(end-name)});
        if(!owner || !owner_gone(*owner))continue;
        entry.data.Wnode.Flags=WNODE_FLAG_TRACED_GUID;
        entry.data.LogFileNameOffset=0;
        ControlTraceW(0,name,&entry.data,EVENT_TRACE_CONTROL_STOP);
    }
}
} // namespace

std::optional<EtwSessionOwner> parse_etw_session_owner(EtwSessionKind kind,std::wstring_view name) noexcept {
    const auto start=prefix(kind);
    if(!name.starts_with(start))return {};
    name.remove_prefix(start.size());
    const auto first=name.find(L'.');
    if(first==name.npos)return {};
    const auto second=name.find(L'.',first+1);
    if(second==name.npos)return {};
    const auto pid=parse_number<DWORD>(name.substr(0,first),10);
    const auto creation=parse_number<std::uint64_t>(name.substr(first+1,second-first-1),16);
    const auto suffix=name.substr(second+1);
    if(!pid || !*pid || !creation || !*creation || suffix.size()!=8 || !parse_number<DWORD>(suffix,16))return {};
    return EtwSessionOwner{*pid,*creation};
}

EtwSession::~EtwSession(){stop();}
ULONG EtwSession::start(EtwSessionKind kind,const EtwSessionConfig& config) {
    stop();
    GUID guid{};
    FILETIME created{},exited{},kernel{},user{};
    if(FAILED(CoCreateGuid(&guid)))return ERROR_GEN_FAILURE;
    if(!GetProcessTimes(GetCurrentProcess(),&created,&exited,&kernel,&user))return GetLastError();
    reclaim_sessions(kind);
    storage_={};
    swprintf_s(storage_.name,L"%ls%lu.%llx.%08lx",prefix(kind).data(),GetCurrentProcessId(),
        static_cast<unsigned long long>(filetime_ticks(created)),guid.Data1);
    auto& p=storage_.data;
    p.Wnode.BufferSize=sizeof(storage_);
    p.Wnode.Flags=WNODE_FLAG_TRACED_GUID;
    p.Wnode.ClientContext=1; // Use QPC. Raw timestamps remain a consumer option.
    p.Wnode.Guid=guid;
    p.BufferSize=config.buffer_kib;
    p.MinimumBuffers=config.minimum_buffers;
    p.MaximumBuffers=config.maximum_buffers;
    p.FlushTimer=1;
    p.LogFileMode=EVENT_TRACE_REAL_TIME_MODE;
    if(kind!=EtwSessionKind::presentation)p.LogFileMode|=EVENT_TRACE_SYSTEM_LOGGER_MODE;
    if(!config.per_processor_buffers)p.LogFileMode|=EVENT_TRACE_NO_PER_PROCESSOR_BUFFERING;
    p.EnableFlags=config.events;
    p.LoggerNameOffset=offsetof(Properties,name);
    const auto status=StartTraceW(&handle_,storage_.name,&p);
    if(status!=ERROR_SUCCESS)handle_=0;
    return status;
}
ULONG EtwSession::enable_provider(const GUID& provider, UCHAR level, ULONGLONG keywords,
                                  ENABLE_TRACE_PARAMETERS parameters) const noexcept {
    if(!active())return ERROR_INVALID_HANDLE;
    return EnableTraceEx2(handle_,&provider,EVENT_CONTROL_CODE_ENABLE_PROVIDER,level,keywords,0,0,&parameters);
}
void EtwSession::stop() noexcept {
    if(!active())return;
    ControlTraceW(handle_,storage_.name,&storage_.data,EVENT_TRACE_CONTROL_STOP);
    handle_=0;
}
EtwSessionHealth EtwSession::health() const noexcept {
    if(!active())return {ERROR_INVALID_HANDLE,0,0,0};
    Properties query{};
    query.data.Wnode.BufferSize=sizeof(query);
    query.data.Wnode.Flags=WNODE_FLAG_TRACED_GUID;
    query.data.LoggerNameOffset=offsetof(Properties,name);
    const auto status=ControlTraceW(handle_,storage_.name,&query.data,EVENT_TRACE_CONTROL_QUERY);
    return {status,query.data.EventsLost,query.data.RealTimeBuffersLost,query.data.LogBuffersLost};
}
} // namespace fluxtop
