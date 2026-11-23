#include "fluxtop/cpu_etw_collector.hpp"
#include "fluxtop/cpu_execution.hpp"
#include "fluxtop/diagnostics.hpp"
#include "fluxtop/windows.hpp"
#include <evntrace.h>
#include <evntcons.h>
#include <tdh.h>
#include <tlhelp32.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <thread>
#include <cmath>

namespace fluxtop {
namespace {
constexpr GUID thread_guid{0x3d6fa8d1,0xfe05,0x11d0,{0x9d,0xda,0x00,0xc0,0x4f,0xd7,0xba,0x7c}};
constexpr GUID process_guid{0x3d6fa8d0,0xfe05,0x11d0,{0x9d,0xda,0x00,0xc0,0x4f,0xd7,0xba,0x7c}};
std::optional<DWORD> property(EVENT_RECORD* event,const wchar_t* name) {
    PROPERTY_DATA_DESCRIPTOR desc{};desc.PropertyName=reinterpret_cast<ULONGLONG>(name);desc.ArrayIndex=ULONG_MAX;
    ULONG size{};DWORD value{};
    if(TdhGetPropertySize(event,0,nullptr,1,&desc,&size)!=ERROR_SUCCESS || size!=sizeof(value) ||
        TdhGetProperty(event,0,nullptr,1,&desc,sizeof(value),reinterpret_cast<BYTE*>(&value))!=ERROR_SUCCESS)return std::nullopt;
    return value;
}
std::uint64_t qpc(){LARGE_INTEGER value{};return QueryPerformanceCounter(&value)?static_cast<std::uint64_t>(value.QuadPart):0;}
}
struct CpuEtwCollector::Impl {
    TRACEHANDLE consumer=INVALID_PROCESSTRACE_HANDLE;
    std::thread thread;
    std::mutex mutex; // Only exchanges immutable completed windows; never held by UI.
    std::atomic<bool> request{true},lost{false};
    std::optional<CpuExecutionWindow> completed;
    std::unique_ptr<CpuExecutionTracker> tracker;
    std::vector<std::pair<unsigned,unsigned>> topology;
    std::uint64_t frequency{},last_event{},last_sealed{};
    ULONG lost_buffers{};
    std::string status{"unavailable"};
    unsigned group_count{};
    std::uint64_t wall_origin{},qpc_origin{};
    bool clock_valid=true;
    bool logged_schema=false;
    bool logged_failure=false;
    bool logged_cswitch=false;
    bool seeded=false;
    std::uint64_t published_end{};
    void observe_process(DWORD pid,std::uint64_t at) {
        if(!pid || !at)return;
        Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid));
        FILETIME birth{},exited{},kernel{},user{},now{};
        if(!process.valid() || !GetProcessTimes(process.get(),&birth,&exited,&kernel,&user))return;
        GetSystemTimePreciseAsFileTime(&now);const auto tick=qpc();
        // UTC is used ONLY to reject recycled live PIDs for delayed lifecycle events.
        // Runtime and all window boundaries use raw QPC, never wall-clock differences.
        const auto expected=static_cast<long double>(wall_origin)+
            (static_cast<long double>(tick)-qpc_origin)*10000000.0L/frequency;
        if(clock_valid && std::abs(static_cast<long double>(filetime_ticks(now))-expected)>1000000.0L) {
            clock_valid=false;
            diagnostics::write(diagnostics::Level::warning,"cpu_etw.identity","wall/QPC correlation changed; new PID identities remain unknown until restart");
        }
        if(!clock_valid || !tick || tick<at)return;
        const auto event_wall=static_cast<long double>(filetime_ticks(now))-
            static_cast<long double>(tick-at)*10000000.0L/frequency;
        if(static_cast<long double>(filetime_ticks(birth))>event_wall)return;
        tracker->process_start(pid,filetime_ticks(birth),at);
    }
    void seed_threads() {
        Handle list(CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD,0));
        if(!list.valid())return;
        THREADENTRY32 item{};item.dwSize=sizeof(item);
        if(!Thread32First(list.get(),&item))return;
        do {
            Handle handle(OpenThread(THREAD_QUERY_LIMITED_INFORMATION,FALSE,item.th32ThreadID));
            if(!handle.valid())continue;
            const DWORD pid=GetProcessIdOfThread(handle.get());
            FILETIME birth{},exited{},kernel{},user{};
            if(!pid || pid!=item.th32OwnerProcessID || !GetThreadTimes(handle.get(),&birth,&exited,&kernel,&user))continue;
            const auto at=qpc();if(!tracker->has_process(pid))observe_process(pid,at);
            tracker->thread_start(item.th32ThreadID,pid,at,true);
        }while(Thread32Next(list.get(),&item));
    }
    static void WINAPI event(EVENT_RECORD* record) noexcept {
        auto& self=*static_cast<Impl*>(record->UserContext);
        try {
            const auto at=static_cast<std::uint64_t>(record->EventHeader.TimeStamp.QuadPart);
            if(!at)return;
            const auto opcode=record->EventHeader.EventDescriptor.Opcode;
            const bool is_thread=IsEqualGUID(record->EventHeader.ProviderId,thread_guid);
            const bool is_process=IsEqualGUID(record->EventHeader.ProviderId,process_guid);
            if(!is_thread && !is_process)return; // Synthetic trace headers do not carry scheduler QPC.
            // OpenTrace alone does not establish the real-time subscription.
            // Seed on the consumer after its first real event, otherwise threads
            // born between seeding and ProcessTrace attachment can be lost forever.
            // No CPU state exists yet, so future-dated seeds cannot charge a tail
            // ahead of this delayed event; ownership waits until QPC catches up.
            if(!self.seeded){self.seeded=true;self.seed_threads();}
            self.last_event=std::max(self.last_event,at);
            if(is_thread && opcode==36) {
                DWORD next{},previous{};
                // Documented Thread_V2 CSwitch prefix, independent of 32/64-bit pointer widths.
                const auto decoded=cpu_switch_threads(record->EventHeader.EventDescriptor.Version,
                    {static_cast<const std::uint8_t*>(record->UserData),record->UserDataLength});
                if(decoded) {
                    next=decoded->first;previous=decoded->second;
                    if(!self.logged_cswitch) {
                        self.logged_cswitch=true;
                        diagnostics::write(diagnostics::Level::info,"cpu_etw.layout","version="+
                            std::to_string(record->EventHeader.EventDescriptor.Version)+" bytes="+std::to_string(record->UserDataLength)+
                            " flags="+std::to_string(record->EventHeader.Flags)+" processor_index="+
                            std::to_string(record->BufferContext.ProcessorIndex)+" new_tid="+std::to_string(next)+" old_tid="+std::to_string(previous));
                    }
                }else {
                    const auto n=property(record,L"NewThreadId"),p=property(record,L"OldThreadId");
                    if(!n || !p){
                        if(!self.logged_schema) {
                            self.logged_schema=true;
                            std::string message="version="+std::to_string(record->EventHeader.EventDescriptor.Version)+
                                " length="+std::to_string(record->UserDataLength);
                            ULONG size{};const auto rc=TdhGetEventInformation(record,0,nullptr,nullptr,&size);
                            message+=" tdh="+std::to_string(rc);
                            if(rc==ERROR_INSUFFICIENT_BUFFER && size<=65536) {
                                std::vector<BYTE> buffer(size);auto* info=reinterpret_cast<TRACE_EVENT_INFO*>(buffer.data());
                                const auto parsed=TdhGetEventInformation(record,0,nullptr,info,&size);
                                message+=" parsed="+std::to_string(parsed);
                                if(parsed==ERROR_SUCCESS)for(ULONG i=0;i<info->TopLevelPropertyCount;++i) {
                                    const auto offset=info->EventPropertyInfoArray[i].NameOffset;
                                    if(offset<size)message+=" field="+utf8(reinterpret_cast<const wchar_t*>(buffer.data()+offset));
                                }
                            }
                            diagnostics::write(diagnostics::Level::warning,"cpu_etw.schema",message);
                        }
                        self.tracker->invalidate("unsupported-cswitch");return;
                    }next=*n;previous=*p;
                }
                std::size_t index{};
                if(record->EventHeader.Flags&EVENT_HEADER_FLAG_PROCESSOR_INDEX)index=record->BufferContext.ProcessorIndex;
                else if(self.group_count==1)index=record->BufferContext.ProcessorNumber;
                else {self.tracker->invalidate("processor-group-unavailable");return;}
                self.tracker->context_switch(index,previous,next,at);return;
            }
            const bool lifecycle=opcode==1 || opcode==2 || opcode==3 || opcode==4;
            if(!lifecycle)return;
            if(is_thread) {
                const auto tid=property(record,L"TThreadId"),pid=property(record,L"ProcessId");
                if(!tid || !pid){self.tracker->invalidate("unsupported-thread-lifecycle");return;}
                if(opcode==2 || opcode==4)self.tracker->thread_end(*tid,*pid,at);
                else {
                    if(!self.tracker->has_process(*pid))self.observe_process(*pid,at);
                    self.tracker->thread_start(*tid,*pid,at,opcode==3);
                }
            }else if(is_process) {
                const auto pid=property(record,L"ProcessId");
                if(!pid){self.tracker->invalidate("unsupported-process-lifecycle");return;}
                if(opcode==2 || opcode==4)self.tracker->process_end(*pid,at);
                else self.observe_process(*pid,at);
            }
        }catch(const std::exception& error){
            if(!self.lost.exchange(true))diagnostics::write(diagnostics::Level::error,"cpu_etw.callback",error.what());
        }catch(...){self.lost=true;}
    }
    static ULONG WINAPI buffer(EVENT_TRACE_LOGFILEW* log) noexcept {
        auto& self=*static_cast<Impl*>(log->Context);
        try {
            if(log->EventsLost>self.lost_buffers){
                self.lost=true;self.lost_buffers=log->EventsLost;
                diagnostics::write(diagnostics::Level::warning,"cpu_etw.consumer","events_lost="+std::to_string(log->EventsLost));
            }
            if(self.lost)self.tracker->invalidate("incomplete");
            if(self.request.load() && self.last_event>self.last_sealed) {
                std::lock_guard lock(self.mutex);
                if(!self.completed && self.request.exchange(false)) {
                    self.completed=self.tracker->finish(self.last_event);self.last_sealed=self.last_event;
                    if(!self.logged_failure && self.completed->status!="ready" && self.completed->status!="partial" && self.completed->status!="baseline") {
                        self.logged_failure=true;
                        diagnostics::write(diagnostics::Level::warning,"cpu_etw.quality",self.completed->status+" "+self.tracker->failure_detail());
                    }
                }
            }
        }catch(...){self.lost=true;}
        return TRUE;
    }
    explicit Impl(const wchar_t* name) {
        HANDLE raw{};TOKEN_ELEVATION elevation{};DWORD returned{};
        if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&raw)){status="administrator-required";return;}
        Handle token(raw);
        if(!GetTokenInformation(raw,TokenElevation,&elevation,sizeof(elevation),&returned) || !elevation.TokenIsElevated) {
            status="administrator-required";return;
        }
        LARGE_INTEGER frequency_value{};
        if(!QueryPerformanceFrequency(&frequency_value) || frequency_value.QuadPart<=0)return;
        frequency=static_cast<std::uint64_t>(frequency_value.QuadPart);
        FILETIME now{};GetSystemTimePreciseAsFileTime(&now);wall_origin=filetime_ticks(now);qpc_origin=qpc();
        group_count=GetActiveProcessorGroupCount();
        const auto count=GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
        if(!count || count>4096 || !group_count){status="topology-unavailable";return;}
        DWORD bytes{};
        GetLogicalProcessorInformationEx(RelationGroup,nullptr,&bytes);
        if(!bytes || bytes>65536){status="topology-unavailable";return;}
        std::vector<BYTE> groups(bytes);
        auto* relation=reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(groups.data());
        if(!GetLogicalProcessorInformationEx(RelationGroup,relation,&bytes) ||
            relation->Relationship!=RelationGroup || relation->Group.ActiveGroupCount!=group_count) {
            status="topology-unavailable";return;
        }
        // Systemwide indexes enumerate active processors in group order. Never
        // infer group=0 or use index/64 on partially populated groups.
        for(WORD group=0;group<relation->Group.ActiveGroupCount;++group) {
            const auto& info=relation->Group.GroupInfo[group];
            unsigned active=0;
            for(unsigned number=0;number<64;++number)if(info.ActiveProcessorMask&(KAFFINITY{1}<<number)) {
                topology.emplace_back(group,number);++active;
            }
            if(active!=info.ActiveProcessorCount){status="topology-unavailable";return;}
        }
        if(topology.size()!=count){status="topology-unavailable";return;}
        tracker=std::make_unique<CpuExecutionTracker>(topology,frequency);
        EVENT_TRACE_LOGFILEW trace{};
        trace.LoggerName=const_cast<LPWSTR>(name);
        trace.ProcessTraceMode=PROCESS_TRACE_MODE_REAL_TIME|PROCESS_TRACE_MODE_EVENT_RECORD|PROCESS_TRACE_MODE_RAW_TIMESTAMP;
        trace.EventRecordCallback=event;trace.BufferCallback=buffer;trace.Context=this;
        consumer=OpenTraceW(&trace);
        if(consumer==INVALID_PROCESSTRACE_HANDLE)return;
        try {
            thread=std::thread([this]{
                const auto result=ProcessTrace(&consumer,1,nullptr,nullptr);
                if(result!=ERROR_SUCCESS && result!=ERROR_CANCELLED){
                    lost=true;diagnostics::write(diagnostics::Level::warning,"cpu_etw.consumer","ProcessTrace win32="+std::to_string(result));
                }
            });
            status="initializing";
        }catch(...){CloseTrace(consumer);consumer=INVALID_PROCESSTRACE_HANDLE;throw;}
    }
    ~Impl(){
        if(consumer!=INVALID_PROCESSTRACE_HANDLE)CloseTrace(consumer);
        if(thread.joinable())thread.join();
    }
};
CpuEtwCollector::CpuEtwCollector(const wchar_t* name):impl_(std::make_unique<Impl>(name)){}
CpuEtwCollector::~CpuEtwCollector()=default;
void CpuEtwCollector::events_lost(){impl_->lost=true;}
void CpuEtwCollector::collect(Snapshot& snapshot) {
    auto& s=*impl_;std::optional<CpuExecutionWindow> result;
    const auto unavailable=[&](const std::string& status) {
        snapshot.cpu_execution_status=status;
        for(auto& row:snapshot.processes){row.cpu_execution={};row.cpu_execution.status=status;}
    };
    unavailable(s.status=="initializing" && s.published_end?"awaiting-window":s.status);
    if(s.status!="initializing")return;
    {
        std::lock_guard lock(s.mutex);
        result=std::move(s.completed);s.completed.reset();
        s.request=true;
    }
    if(s.lost){unavailable("incomplete");return;}
    if(GetActiveProcessorCount(ALL_PROCESSOR_GROUPS)!=s.topology.size() || GetActiveProcessorGroupCount()!=s.group_count) {
        s.lost=true;unavailable("topology-changed");return;
    }
    if(!result) {
        const auto now=qpc();
        if(s.published_end && now>s.published_end && now-s.published_end>s.frequency*5)unavailable("stale");
        return;
    }
    const auto now=qpc();
    if(now<result->end_qpc || now-result->end_qpc>s.frequency*5){unavailable("stale");return;}
    snapshot.cpu_execution_status=result->status;
    if(result->status!="ready" && result->status!="partial" && result->status!="baseline" && result->status!="stale")s.status=result->status;
    s.published_end=result->end_qpc;
    snapshot.cpu_execution_begin_qpc=result->begin_qpc;snapshot.cpu_execution_end_qpc=result->end_qpc;
    snapshot.cpu_execution_frequency=result->frequency;
    snapshot.cpu_execution_unknown_ticks=result->unknown_ticks;
    snapshot.cpu_execution_lag_ms=1000.0*static_cast<double>(now-result->end_qpc)/static_cast<double>(s.frequency);
    for(auto& row:snapshot.processes) {
        if(!row.creation_time){row.cpu_execution.status="identity-unavailable";continue;}
        row.cpu_execution=result->for_process({row.pid,*row.creation_time});
    }
}
}
