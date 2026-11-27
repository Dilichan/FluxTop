#include "fluxtop/disk_io_collector.hpp"
#include "fluxtop/disk_attribution.hpp"
#include "fluxtop/cpu_etw_collector.hpp"
#include "fluxtop/etw_session.hpp"
#include "fluxtop/diagnostics.hpp"
#include "fluxtop/windows.hpp"
#include <windows.h>
#include <winioctl.h>
#include <evntrace.h>
#include <evntcons.h>
#include <tdh.h>
#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace fluxtop {
namespace {
constexpr GUID disk_guid{0x3d6fa8d4,0xfe05,0x11d0,{0x9d,0xda,0x00,0xc0,0x4f,0xd7,0xba,0x7c}};
constexpr GUID file_guid{0x90cbdc39,0x4a3e,0x11d1,{0x84,0xf4,0x00,0x00,0xf8,0x04,0x64,0xe3}};
constexpr GUID thread_guid{0x3d6fa8d1,0xfe05,0x11d0,{0x9d,0xda,0x00,0xc0,0x4f,0xd7,0xba,0x7c}};
constexpr std::size_t max_irps=65536;

// TDH handles MOF versions and 32/64-bit ETW pointer widths.
std::optional<std::uint64_t> property(EVENT_RECORD* event,const wchar_t* name) {
    PROPERTY_DATA_DESCRIPTOR desc{};
    desc.PropertyName=reinterpret_cast<ULONGLONG>(name);
    desc.ArrayIndex=ULONG_MAX;
    ULONG bytes{};
    if(TdhGetPropertySize(event,0,nullptr,1,&desc,&bytes)!=ERROR_SUCCESS ||
        (bytes!=4 && bytes!=8))return std::nullopt;
    std::uint64_t value{};
    if(TdhGetProperty(event,0,nullptr,1,&desc,bytes,reinterpret_cast<BYTE*>(&value))!=ERROR_SUCCESS)
        return std::nullopt;
    return value;
}
std::vector<PhysicalVolumeExtent> discover_volumes(DWORD mask) {
    std::vector<PhysicalVolumeExtent> results;
    for(unsigned i=0;i<26;++i) {
        if(!(mask&(1U<<i)))continue;
        const wchar_t letter=static_cast<wchar_t>(L'A'+i);
        wchar_t root[]{L"C:\\"};root[0]=letter;
        if(GetDriveTypeW(root)!=DRIVE_FIXED)continue;
        wchar_t volume[]{L"\\\\.\\C:"};volume[4]=letter;
        Handle handle(CreateFileW(volume,0,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
            nullptr,OPEN_EXISTING,0,nullptr));
        if(!handle.valid())continue;
        std::array<BYTE,4096> output{};
        DWORD returned{};
        if(!DeviceIoControl(handle.get(),IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS,nullptr,0,
            output.data(),static_cast<DWORD>(output.size()),&returned,nullptr))continue;
        if(returned<offsetof(VOLUME_DISK_EXTENTS,Extents))continue;
        const auto* extents=reinterpret_cast<const VOLUME_DISK_EXTENTS*>(output.data());
        const auto capacity=(output.size()-offsetof(VOLUME_DISK_EXTENTS,Extents))/sizeof(DISK_EXTENT);
        if(extents->NumberOfDiskExtents>capacity)continue;
        for(DWORD j=0;j<extents->NumberOfDiskExtents;++j) {
            const auto& e=extents->Extents[j];
            if(e.StartingOffset.QuadPart<0 || e.ExtentLength.QuadPart<=0)continue;
            results.push_back({e.DiskNumber,
                static_cast<std::uint64_t>(e.StartingOffset.QuadPart),
                static_cast<std::uint64_t>(e.ExtentLength.QuadPart),letter});
        }
    }
    return results;
}
}
struct DiskIoCollector::Impl {
    EtwSession session;
    struct Inflight { DWORD pid{};std::uint64_t file{};std::uint64_t at{};bool write{}; };
    struct Bytes { std::uint64_t read{},write{};std::array<std::uint64_t,26> volumes{};
        std::uint64_t first_event{}; };
    TRACEHANDLE consumer=INVALID_PROCESSTRACE_HANDLE;
    std::thread thread;
    std::unique_ptr<CpuEtwCollector> cpu;
    std::mutex mutex;
    std::unordered_map<std::uint64_t,Inflight> irps; // FileIo, optional correlation.
    std::unordered_map<std::uint64_t,Inflight> disk_inits; // DiskIo Init; same layer as completion.
    DiskThreadOwners thread_owners; // ETW Start/DCStart/End, bounded by thread lifetime.
    std::unordered_map<DWORD,Bytes> per_pid;
    std::array<std::uint64_t,26> volume_total{};
    std::vector<PhysicalVolumeExtent> extents;
    std::uint64_t physical_total{},matched_total{};
    DiskDeliveryAccounting delivery{}; // Sampling-side accounting, reset every 15s.
    // Cumulative ETW diagnostics reset only every 15 seconds, not each sample.
    // Diagnostics report coverage without capturing paths, names or raw events.
    struct Telemetry {
        std::uint64_t file_events{},disk_events{},file_missing{},disk_invalid{};
        std::uint64_t no_disk_irp{},unmatched_irp{},mismatch_file{},
            mismatch_direction{},mismatch_time{},matched_events{},unmapped_volume{};
        std::uint64_t physical_bytes{},matched_bytes{};
        std::uint64_t disk_init_events{},disk_init_usable{},disk_init_matches{},
            disk_init_unknown_owner{},disk_init_missing_irp{},disk_completion_no_fileobj{};
        std::uint64_t thread_events{},thread_cached{},thread_retired{},thread_untracked{};
        std::uint64_t init_pid_header{},init_pid_open_thread{},init_pid_etw{};
        std::uint64_t init_missing_tid{},init_system_thread{},init_no_thread_owner{};
        std::uint64_t init_open_denied{},init_open_gone{},init_open_other{};
    } telemetry{};
    std::chrono::steady_clock::time_point last_diagnostics=std::chrono::steady_clock::now();
    std::atomic<bool> failed{false};
    bool baseline{};
    std::string status{"unavailable"};
    DWORD drive_mask{};
    std::chrono::steady_clock::time_point previous=std::chrono::steady_clock::now();

    // Do not infer the application from a System-context disk completion.
    // Resolve its initiating thread by a live handle first; when that thread
    // has already exited, use a time-bounded ETW lifetime observation.
    enum class OwnerSource { unknown, header, open_thread, etw_thread, missing_tid, system_thread };
    struct OwnerResolution {
        DWORD pid{};
        OwnerSource source{OwnerSource::unknown};
        DWORD open_error{};
    };
    static OwnerResolution initiating_pid(Impl& self,EVENT_RECORD* record,
        const wchar_t* thread_property,std::uint64_t at) {
        const DWORD header_pid=record->EventHeader.ProcessId;
        if(header_pid>4)return {header_pid,OwnerSource::header,0};
        const auto value=property(record,thread_property);
        if(!value || !*value || *value>UINT32_MAX)
            return {0,OwnerSource::missing_tid,0};
        const DWORD tid=static_cast<DWORD>(*value);
        Handle thread(OpenThread(THREAD_QUERY_LIMITED_INFORMATION,FALSE,tid));
        DWORD error{};
        if(thread.valid()) {
            const DWORD owner=GetProcessIdOfThread(thread.get());
            if(owner>4)return {owner,OwnerSource::open_thread,0};
            if(owner!=0)return {0,OwnerSource::system_thread,0};
            error=GetLastError();
        } else error=GetLastError();
        // The ETW callback can run after a short-lived thread has exited.
        // Lookup is guarded by the same mutex as lifecycle event updates.
        std::lock_guard lock(self.mutex);
        const auto recorded=self.thread_owners.find(tid,at);
        if(recorded) {
            if(*recorded>4)return {*recorded,OwnerSource::etw_thread,error};
            return {0,OwnerSource::system_thread,error};
        }
        return {0,OwnerSource::unknown,error};
    }

    static void WINAPI event(EVENT_RECORD* record) noexcept {
        auto& self=*static_cast<Impl*>(record->UserContext);
        const auto opcode=record->EventHeader.EventDescriptor.Opcode;
        const bool file=IsEqualGUID(record->EventHeader.ProviderId,file_guid);
        const bool disk=IsEqualGUID(record->EventHeader.ProviderId,disk_guid);
        const bool disk_init=disk && (opcode==12 || opcode==13);
        const bool disk_end=disk && (opcode==10 || opcode==11);
        const bool thread_event=IsEqualGUID(record->EventHeader.ProviderId,thread_guid) &&
            (opcode==1 || opcode==2 || opcode==3 || opcode==4);
        if((!file || (opcode!=67 && opcode!=68)) && !disk_init && !disk_end && !thread_event)return;
        try {
            if(thread_event) {
                const auto tid=property(record,L"TThreadId");
                const auto pid=property(record,L"ProcessId");
                const auto at=static_cast<std::uint64_t>(record->EventHeader.TimeStamp.QuadPart);
                std::lock_guard lock(self.mutex);
                ++self.telemetry.thread_events;
                if(!tid || !*tid || *tid>UINT32_MAX || !at) {
                    ++self.telemetry.thread_untracked;return;
                }
                if(opcode==2 || opcode==4) {
                    self.thread_owners.finish(static_cast<DWORD>(*tid),at);
                    ++self.telemetry.thread_retired;
                }else if(pid && *pid>0 && *pid<=UINT32_MAX &&
                    self.thread_owners.observe(static_cast<DWORD>(*tid),
                        static_cast<DWORD>(*pid),at)) {
                    ++self.telemetry.thread_cached;
                }else ++self.telemetry.thread_untracked;
                return;
            }
            const auto irp=property(record,file?L"IrpPtr":L"Irp");
            const auto fileobj=disk_init?std::optional<std::uint64_t>{}:
                property(record,L"FileObject");
            const auto when=static_cast<std::uint64_t>(record->EventHeader.TimeStamp.QuadPart);
            // ReadInit/WriteInit records carry IssuingThreadId and the physical
            // IRP. Their initiating PID is a stronger correlation than FileIo
            // requests which can split into different IRPs below the filesystem.
            const auto ownership=(disk_init || file)?initiating_pid(self,record,
                disk_init?L"IssuingThreadId":L"TTID",when):OwnerResolution{};
            const DWORD owner=ownership.pid;
            std::lock_guard lock(self.mutex);
            if(disk_init) {
                ++self.telemetry.disk_init_events;
                switch(ownership.source) {
                case OwnerSource::header: ++self.telemetry.init_pid_header;break;
                case OwnerSource::open_thread: ++self.telemetry.init_pid_open_thread;break;
                case OwnerSource::etw_thread: ++self.telemetry.init_pid_etw;break;
                case OwnerSource::missing_tid: ++self.telemetry.init_missing_tid;break;
                case OwnerSource::system_thread: ++self.telemetry.init_system_thread;break;
                default: ++self.telemetry.init_no_thread_owner;break;
                }
                if(ownership.open_error==ERROR_ACCESS_DENIED)
                    ++self.telemetry.init_open_denied;
                else if(ownership.open_error==ERROR_INVALID_PARAMETER)
                    ++self.telemetry.init_open_gone;
                else if(ownership.open_error)
                    ++self.telemetry.init_open_other;
                if(!irp || !*irp){++self.telemetry.disk_init_missing_irp;return;}
                if(owner<=4){++self.telemetry.disk_init_unknown_owner;return;}
                if(self.disk_inits.size()>=max_irps && !self.disk_inits.contains(*irp)){
                    self.failed=true;return;
                }
                self.disk_inits[*irp]={owner,0,when,opcode==13};
                ++self.telemetry.disk_init_usable;
                return;
            }
            if(file) {
                ++self.telemetry.file_events;
                if(!irp || !*irp || !fileobj || !*fileobj) {
                    ++self.telemetry.file_missing;return;
                }
                const DWORD pid=owner;
                if(pid<=4)return; // Cache manager / System writes remain unattributed.
                if(self.irps.size()>=max_irps && !self.irps.contains(*irp)) {
                    self.failed=true;return;
                }
                self.irps[*irp]={pid,*fileobj,when,opcode==68};
                return;
            }
            ++self.telemetry.disk_events;
            const auto number=property(record,L"DiskNumber");
            const auto offset=property(record,L"ByteOffset");
            const auto size=property(record,L"TransferSize");
            if(!number || !offset || !size || !*size || *size>64ULL*1024*1024 ||
                *number>UINT32_MAX) {
                ++self.telemetry.disk_invalid;self.failed=true;return;
            }
            const auto volume=disk_io_volume_for(self.extents,static_cast<std::uint32_t>(*number),*offset,*size);
            self.physical_total+=*size;
            self.telemetry.physical_bytes+=*size;
            if(volume)self.volume_total[static_cast<unsigned>(*volume-L'A')]+=*size;
            else ++self.telemetry.unmapped_volume;
            if(!irp || !*irp) {
                ++self.telemetry.no_disk_irp;return; // Cannot correlate physical work.
            }
            DWORD attributed_pid{};
            std::uint64_t request_at{};
            bool matched_init=false;
            const auto init=self.disk_inits.find(*irp);
            if(init!=self.disk_inits.end()) {
                const auto request=init->second;
                self.disk_inits.erase(init); // Prevent IRP reuse/duplicate credits.
                if(disk_io_valid_completion(request.at,when,request.write,opcode==11)) {
                    attributed_pid=request.pid;
                    request_at=request.at;
                    matched_init=true;
                } else ++self.telemetry.mismatch_time;
            }
            // For drivers without DiskIo Init, preserve the stricter FileIo
            // correlation only when FileObject and operation direction agree.
            if(!matched_init) {
                if(!fileobj || !*fileobj) {
                    ++self.telemetry.disk_completion_no_fileobj;return;
                }
                const auto match=self.irps.find(*irp);
                if(match==self.irps.end()) {
                    ++self.telemetry.unmatched_irp;return;
                }
                const auto request=match->second;
                self.irps.erase(match);
                if(request.file!=*fileobj) {++self.telemetry.mismatch_file;return;}
                if(request.write!=(opcode==11)) {++self.telemetry.mismatch_direction;return;}
                if(!disk_io_valid_completion(request.at,when,request.write,opcode==11)) {
                    ++self.telemetry.mismatch_time;return;
                }
                attributed_pid=request.pid;
                request_at=request.at;
            } else {
                self.irps.erase(*irp); // Same physical IRP must be credited once.
                ++self.telemetry.disk_init_matches;
            }
            if(attributed_pid<=4)return;
            auto& bytes=self.per_pid[attributed_pid];
            if(bytes.first_event==0 || request_at<bytes.first_event)bytes.first_event=request_at;
            if(opcode==11)bytes.write+=*size;else bytes.read+=*size;
            if(volume)bytes.volumes[static_cast<unsigned>(*volume-L'A')]+=*size;
            self.matched_total+=*size;
            self.telemetry.matched_bytes+=*size;
            ++self.telemetry.matched_events;
        } catch (...) { self.failed=true; }
    }
    static ULONG WINAPI buffer(EVENT_TRACE_LOGFILEW* log) noexcept {
        if(log->EventsLost)static_cast<Impl*>(log->Context)->failed=true;
        return TRUE;
    }
    explicit Impl(DWORD mask) {
        HANDLE raw_token{};
        TOKEN_ELEVATION elevation{};DWORD token_bytes{};
        if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&raw_token)) {status="administrator-required";return;}
        Handle elevation_token(raw_token);
        if(!GetTokenInformation(raw_token,TokenElevation,&elevation,sizeof(elevation),&token_bytes) || !elevation.TokenIsElevated) {
            status="administrator-required";return;
        }
        drive_mask=mask;
        extents=discover_volumes(drive_mask);
        const auto processors=GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
        // CSwitch needs real originating-CPU context, not the legacy shared
        // low-frequency buffer stream. Keep an explicit <=32 MiB pool budget.
        const bool cpu_supported=processors>0 && processors<=512;
        // Reserve burst headroom up front: high-IRQL scheduler events cannot
        // always obtain on-demand buffers. The requested pool remains <=32 MiB.
        ULONG events=EVENT_TRACE_FLAG_DISK_IO|EVENT_TRACE_FLAG_DISK_IO_INIT|
            EVENT_TRACE_FLAG_DISK_FILE_IO|EVENT_TRACE_FLAG_FILE_IO|EVENT_TRACE_FLAG_FILE_IO_INIT|
            EVENT_TRACE_FLAG_THREAD;
        if(cpu_supported)events|=EVENT_TRACE_FLAG_PROCESS|EVENT_TRACE_FLAG_CSWITCH;
        const ULONG rc=session.start(EtwSessionKind::disk_cpu,{events,cpu_supported?16UL:64UL,
            cpu_supported?std::max<ULONG>(processors*2,256):2UL,cpu_supported?2048UL:8UL,cpu_supported});
        if(rc!=ERROR_SUCCESS) {
            status=rc==ERROR_ACCESS_DENIED?"administrator-required":"unavailable";
            diagnostics::write(diagnostics::Level::warning,"disk_io.session",
                "StartTrace failed win32="+std::to_string(rc)+" status="+status);
            return;
        }
        EVENT_TRACE_LOGFILEW trace{};
        trace.LoggerName=session.name();
        trace.ProcessTraceMode=PROCESS_TRACE_MODE_REAL_TIME|PROCESS_TRACE_MODE_EVENT_RECORD;
        trace.EventRecordCallback=event;trace.BufferCallback=buffer;trace.Context=this;
        consumer=OpenTraceW(&trace);
        if(consumer==INVALID_PROCESSTRACE_HANDLE) {
            const DWORD code=GetLastError();
            status=code==ERROR_ACCESS_DENIED?"administrator-required":"unavailable";
            diagnostics::write(diagnostics::Level::warning,"disk_io.session",
                "OpenTrace failed win32="+std::to_string(code)+" status="+status);
            stop();return;
        }
        // Reuse this one kernel session. CPU gets a separate raw-QPC consumer;
        // the established disk consumer keeps its original FILETIME conversion/correlation.
        try {if(cpu_supported)cpu=std::make_unique<CpuEtwCollector>(session.name());}
        catch(const std::exception& error) {
            diagnostics::write(diagnostics::Level::warning,"cpu_etw.start",error.what());
        }
        try {
            thread=std::thread([this]{
                const ULONG result=ProcessTrace(&consumer,1,nullptr,nullptr);
                if(result!=ERROR_SUCCESS && result!=ERROR_CANCELLED) {
                    {std::lock_guard lock(mutex);failed=true;}
                    diagnostics::write(diagnostics::Level::warning,"disk_io.session",
                        "ProcessTrace failed win32="+std::to_string(result));
                }
            });
            status="ready";
            diagnostics::write(diagnostics::Level::info,"disk_io.session",
                "ETW system trace started");
        }catch(...) {stop();throw;}
    }
    void stop() noexcept {
        session.stop();
        if(consumer!=INVALID_PROCESSTRACE_HANDLE) {
            CloseTrace(consumer);
            if(thread.joinable())thread.join();
            consumer=INVALID_PROCESSTRACE_HANDLE;
        }
        cpu.reset();
    }
    ~Impl(){stop();}
};
DiskIoCollector::DiskIoCollector():impl_(std::make_unique<Impl>(GetLogicalDrives())){}
DiskIoCollector::~DiskIoCollector()=default;
void DiskIoCollector::collect(Snapshot& snapshot) {
    collect(snapshot,GetLogicalDrives());
}
void DiskIoCollector::collect(Snapshot& snapshot,std::uint32_t drive_mask) {
    const auto recovery_now=EtwRecovery::Clock::now();
    if(recovery_.due(recovery_now)){
        // Destroy/join both consumers and stop only our session before replacing it.
        impl_.reset();impl_=std::make_unique<Impl>(drive_mask);recovery_.restarted(recovery_now);
        diagnostics::write(diagnostics::Level::info,"disk_io.recovery","restarted shared CPU/disk session");
    }
    auto& self=*impl_;
    for(auto& row:snapshot.processes){
        row.disk_read_bytes_per_second.reset();row.disk_write_bytes_per_second.reset();row.disk_pressure_percent.reset();
        row.disk_volume_bytes_per_second.fill(std::nullopt);row.disk_volume_pressure_percent.fill(std::nullopt);
    }
    snapshot.cpu_execution_begin_qpc=0;snapshot.cpu_execution_end_qpc=0;
    snapshot.cpu_execution_frequency=0;snapshot.cpu_execution_unknown_ticks=0;snapshot.cpu_execution_lag_ms=0;
    bool trace_failed=false;
    if(self.session.active()) {
        const auto health=self.session.health();
        if(!health.complete()) {
            trace_failed=true;self.failed=true;
            diagnostics::write(diagnostics::Level::warning,"cpu_etw.loss",
                "query="+std::to_string(health.status)+" events="+std::to_string(health.events_lost)+
                " realtime_buffers="+std::to_string(health.realtime_buffers_lost)+" log_buffers="+std::to_string(health.log_buffers_lost));
            if(self.cpu)self.cpu->events_lost();
        }
    }
    if(self.cpu) {
        self.cpu->collect(snapshot);
    }else {
        snapshot.cpu_execution_status=self.status=="administrator-required"?self.status:"unavailable";
        for(auto& row:snapshot.processes){row.cpu_execution={};row.cpu_execution.status=snapshot.cpu_execution_status;}
    }
    const auto& quality=snapshot.cpu_execution_status;
    const bool cpu_failed=quality=="incomplete" || quality=="stale" || quality=="out-of-order" ||
        quality=="switch-discontinuity" || quality=="thread-lifetime-discontinuity" ||
        quality=="overlapping-thread-execution" || quality=="state-capacity-exceeded" || quality=="identity-clock-changed" ||
        quality=="topology-changed" || quality=="processor-unavailable" || quality=="runtime-overflow";
    if((trace_failed || self.failed || cpu_failed || self.status=="unavailable") && self.status!="administrator-required"){
        if(recovery_.schedule(recovery_now))diagnostics::write(diagnostics::Level::warning,"disk_io.recovery","scheduled fresh trace after failure; invalid windows remain unavailable");
    }else recovery_.healthy(recovery_now);
    const auto now=std::chrono::steady_clock::now();
    const double seconds=std::chrono::duration<double>(now-self.previous).count();
    self.previous=now;
    if(drive_mask && self.drive_mask!=drive_mask) {
        auto next=discover_volumes(drive_mask);
        std::lock_guard lock(self.mutex);
        self.extents=std::move(next);self.drive_mask=drive_mask;
    }
    std::unordered_map<DWORD,Impl::Bytes> counts;
    std::array<std::uint64_t,26> physical_by_volume{};
    std::uint64_t total{},matched{};
    Impl::Telemetry report_stats{};
    std::size_t tracked_threads{};
    bool report=false;
    bool lost{};
    {
        std::lock_guard lock(self.mutex);
        counts.swap(self.per_pid);
        physical_by_volume=self.volume_total;self.volume_total.fill(0);
        total=self.physical_total;matched=self.matched_total;
        self.physical_total=0;self.matched_total=0;
        lost=self.failed.load();
        if(now-self.last_diagnostics>=std::chrono::seconds(15)) {
            report_stats=self.telemetry;
            tracked_threads=self.thread_owners.size();
            self.telemetry={};
            self.last_diagnostics=now;
            report=true;
        }
        // IRPs survive sampling boundaries. Keep FileIo and DiskIo-init caches
        // bounded to the SAME 10-second validity window used for correlation.
        // Invalid timestamp zero must also expire instead of occupying a slot.
        const auto latest=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count()/100)+116444736000000000ULL;
        disk_io_expire_requests(self.irps,latest);
        disk_io_expire_requests(self.disk_inits,latest);
    }
    DiskDeliveryAccounting delivered{};
    delivered.matched=matched;
    for(const auto& [pid,data]:counts) {
        (void)pid;
        delivered.queued+=data.read+data.write;
    }
    // This accounting is sampling-side only. ETW callback counters are captured
    // under the mutex in the same sample, so 15s windows can be reconciled.
    const auto flush_delivery=[&] {
        self.delivery.add(delivered);
        if(!report)return;
        const auto d=self.delivery;
        self.delivery={};
        diagnostics::write(diagnostics::Level::info,"disk_io.delivery",
            "matched_bytes="+std::to_string(d.matched)+
            " queued_bytes="+std::to_string(d.queued)+
            " snapshot_bytes="+std::to_string(d.in_snapshot)+
            " published_bytes="+std::to_string(d.published)+
            " missing_pid_bytes="+std::to_string(d.no_pid)+
            " no_baseline_bytes="+std::to_string(d.no_baseline)+
            " pid_reused_bytes="+std::to_string(d.pid_reused)+
            " unavailable_session_bytes="+std::to_string(d.no_session)+
            " lost_event_bytes="+std::to_string(d.lost_events)+
            " startup_bytes="+std::to_string(d.startup)+
            " short_interval_bytes="+std::to_string(d.short_interval)+
            " invalid_coverage_bytes="+std::to_string(d.invalid_coverage)+
            " unpublishable_bytes="+std::to_string(d.unpublishable)+
            " reconciled="+(d.reconciled()?"true":"false"));
    };
    snapshot.disk_io_status=lost?"incomplete":self.status;
    if(report) {
        const auto& d=report_stats;
        const auto percentage=d.physical_bytes ?
            std::to_string(100.0*static_cast<double>(d.matched_bytes)/
                static_cast<double>(d.physical_bytes)) : "n/a";
        diagnostics::write(diagnostics::Level::info,"disk_io.coverage",
            "physical_bytes="+std::to_string(d.physical_bytes)+
            " matched_bytes="+std::to_string(d.matched_bytes)+
            " coverage_percent="+percentage+
            " file_events="+std::to_string(d.file_events)+
            " disk_events="+std::to_string(d.disk_events)+
            " matches="+std::to_string(d.matched_events)+
            " file_missing="+std::to_string(d.file_missing)+
            " disk_invalid="+std::to_string(d.disk_invalid)+
            " no_disk_irp="+std::to_string(d.no_disk_irp)+
            " unmatched_irp="+std::to_string(d.unmatched_irp)+
            " wrong_file="+std::to_string(d.mismatch_file)+
            " wrong_direction="+std::to_string(d.mismatch_direction)+
            " wrong_time="+std::to_string(d.mismatch_time)+
            " unmapped_volume="+std::to_string(d.unmapped_volume)+
            " init_events="+std::to_string(d.disk_init_events)+
            " init_usable="+std::to_string(d.disk_init_usable)+
            " init_matches="+std::to_string(d.disk_init_matches)+
            " init_unknown_owner="+std::to_string(d.disk_init_unknown_owner)+
            " init_missing_irp="+std::to_string(d.disk_init_missing_irp)+
            " disk_no_fileobj="+std::to_string(d.disk_completion_no_fileobj)+
            " owner_header="+std::to_string(d.init_pid_header)+
            " owner_open_thread="+std::to_string(d.init_pid_open_thread)+
            " owner_etw="+std::to_string(d.init_pid_etw)+
            " owner_missing_tid="+std::to_string(d.init_missing_tid)+
            " owner_system="+std::to_string(d.init_system_thread)+
            " owner_unknown_tid="+std::to_string(d.init_no_thread_owner)+
            " owner_open_denied="+std::to_string(d.init_open_denied)+
            " owner_open_gone="+std::to_string(d.init_open_gone)+
            " owner_open_other="+std::to_string(d.init_open_other)+
            " thread_events="+std::to_string(d.thread_events)+
            " thread_cached="+std::to_string(d.thread_cached)+
            " thread_retired="+std::to_string(d.thread_retired)+
            " thread_untracked="+std::to_string(d.thread_untracked)+
            " tracked_threads="+std::to_string(tracked_threads)+
            " quality="+(self.status!="ready"?"unavailable":d.physical_bytes==0?"unobserved":
                d.matched_bytes==d.physical_bytes?"complete":"partial")+
            " session="+self.status);
    }
    if(self.status!="ready" || lost || !self.baseline || seconds<0.1) {
        // Preserve the original priority of rejection causes.
        if(self.status!="ready")delivered.no_session=matched;
        else if(lost)delivered.lost_events=matched;
        else if(!self.baseline)delivered.startup=matched;
        else delivered.short_interval=matched;
        self.baseline=true;
        flush_delivery();return;
    }
    if(total>0 && matched>total) {
        delivered.invalid_coverage=matched;
        snapshot.disk_io_status="incomplete";
        flush_delivery();return;
    }
    // FileIO and DiskIO do not correspond one-to-one because of caching and
    // kernel activity. Low global coverage is not evidence against a single
    // positively correlated read/write: publish only that verified lower bound.
    if(total>0 && !disk_io_fully_attributed(matched,total,false))
        snapshot.disk_io_status="partial-attribution";
    // Zero observed DiskIo events do not prove an idle physical device:
    // providers, drivers and delivery latency vary. Never infer zero from PDH.
    if(!disk_io_publishable_bytes(matched,total,false)) {
        delivered.unpublishable=matched;
        flush_delivery();return;
    }
    const bool complete=disk_io_fully_attributed(matched,total,false);
    std::array<std::uint64_t,26> verified_by_volume{};
    for(const auto& [pid,data]:counts) {
        (void)pid;
        for(std::size_t i=0;i<verified_by_volume.size();++i)
            verified_by_volume[i]+=data.volumes[i];
    }
    for(auto& row:snapshot.processes) {
        const auto found=counts.find(row.pid);
        const std::uint64_t pid_bytes=found==counts.end()?0ULL:
            found->second.read+found->second.write;
        delivered.in_snapshot+=pid_bytes;
        if(!row.has_baseline || !row.creation_time) {
            delivered.no_baseline+=pid_bytes;
            continue;
        }
        if(!disk_io_pid_lifetime_valid(true,row.creation_time,
            found==counts.end()?0ULL:found->second.first_event)) {
            delivered.pid_reused+=pid_bytes; // Discard old PID lifetime, never reassign its bytes.
            continue;
        }
        const Impl::Bytes* bytes=found==counts.end()?nullptr:&found->second;
        // A missing match in a partially observed system is UNKNOWN, not 0 B/s.
        if(!bytes && !complete)continue;
        const auto read=bytes?bytes->read:0ULL;
        const auto write=bytes?bytes->write:0ULL;
        row.disk_read_bytes_per_second=static_cast<double>(read)/seconds;
        row.disk_write_bytes_per_second=static_cast<double>(write)/seconds;
        delivered.published+=pid_bytes;
        double peak=0.0;
        bool has_pressure=false;
        for(const auto& drive:snapshot.disks) {
            if(drive.letter<L'A' || drive.letter>L'Z')continue;
            const auto i=static_cast<unsigned>(drive.letter-L'A');
            const auto amount=bytes?bytes->volumes[i]:0ULL;
            if(physical_by_volume[i]) {
                // No confirmed bytes for a volume means unknown unless its
                // physical interval was fully attributable to known processes.
                if(!amount && !disk_io_volume_zero_certified(
                    physical_by_volume[i],verified_by_volume[i]))continue;
                row.disk_volume_bytes_per_second[i]=static_cast<double>(amount)/seconds;
                const auto pressure=disk_io_pressure(drive.active_percent,amount,physical_by_volume[i]);
                if(pressure){
                    row.disk_volume_pressure_percent[i]=*pressure;
                    peak=std::max(peak,*pressure);has_pressure=true;
                }
            }
        }
        if(has_pressure)row.disk_pressure_percent=peak;
    }
    delivered.no_pid=delivered.queued>=delivered.in_snapshot ?
        delivered.queued-delivered.in_snapshot:0ULL;
    flush_delivery();
}
} // namespace fluxtop
