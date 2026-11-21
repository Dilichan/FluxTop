#include "fluxtop/disk_io_collector.hpp"
#include "fluxtop/disk_attribution.hpp"
#include "fluxtop/windows.hpp"
#include <windows.h>
#include <winioctl.h>
#include <evntrace.h>
#include <evntcons.h>
#include <tdh.h>
#include <objbase.h>
#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <cstring>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <vector>

namespace fluxtop {
namespace {
constexpr GUID disk_guid{0x3d6fa8d4,0xfe05,0x11d0,{0x9d,0xda,0x00,0xc0,0x4f,0xd7,0xba,0x7c}};
constexpr GUID file_guid{0x90cbdc39,0x4a3e,0x11d1,{0x84,0xf4,0x00,0x00,0xf8,0x04,0x64,0xe3}};
constexpr std::size_t max_irps=65536;
constexpr std::uint64_t filetime_second=10000000ULL;
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
void reclaim_old_sessions() {
    struct Properties { EVENT_TRACE_PROPERTIES info{}; wchar_t name[256]{}; };
    std::vector<Properties> info(64);
    std::array<EVENT_TRACE_PROPERTIES*,64> pointers{};
    for(std::size_t i=0;i<info.size();++i) {
        info[i].info.Wnode.BufferSize=sizeof(Properties);
        info[i].info.LoggerNameOffset=offsetof(Properties,name);
        pointers[i]=&info[i].info;
    }
    ULONG found{};
    if(QueryAllTracesW(pointers.data(),static_cast<ULONG>(pointers.size()),&found)!=ERROR_SUCCESS)return;
    for(ULONG i=0;i<std::min<ULONG>(found,static_cast<ULONG>(info.size()));++i) {
        const auto& p=info[i].info;
        if(p.LoggerNameOffset>=sizeof(Properties))continue;
        const auto* name=reinterpret_cast<const wchar_t*>(
            reinterpret_cast<const BYTE*>(&info[i])+p.LoggerNameOffset);
        unsigned long pid{};unsigned long long birth{};unsigned long suffix{};
        if(swscanf_s(name,L"FluxTop.Disk.%lu.%llx.%lx",&pid,&birth,&suffix)!=3)continue;
        Handle owner(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid));
        bool orphan=!owner.valid() && GetLastError()==ERROR_INVALID_PARAMETER;
        if(owner.valid()) {
            FILETIME created{},exited{},kernel{},user{};
            if(GetProcessTimes(owner.get(),&created,&exited,&kernel,&user))
                orphan=filetime_ticks(created)!=birth;
        }
        if(orphan) {
            auto props=info[i].info;
            props.Wnode.Flags=WNODE_FLAG_TRACED_GUID;
            props.LogFileNameOffset=0;
            ControlTraceW(0,name,&props,EVENT_TRACE_CONTROL_STOP);
        }
    }
}
std::vector<PhysicalVolumeExtent> discover_volumes() {
    std::vector<PhysicalVolumeExtent> results;
    const DWORD mask=GetLogicalDrives();
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
    struct Properties { EVENT_TRACE_PROPERTIES trace{}; wchar_t name[96]{}; } properties{};
    struct Inflight { DWORD pid{};std::uint64_t file{};std::uint64_t at{};bool write{}; };
    struct Bytes { std::uint64_t read{},write{};std::array<std::uint64_t,26> volumes{};
        std::uint64_t first_event{}; };
    TRACEHANDLE session{},consumer=INVALID_PROCESSTRACE_HANDLE;
    std::thread thread;
    std::mutex mutex;
    std::unordered_map<std::uint64_t,Inflight> irps;
    std::unordered_map<DWORD,Bytes> per_pid;
    std::array<std::uint64_t,26> volume_total{};
    std::vector<PhysicalVolumeExtent> extents;
    std::uint64_t physical_total{},matched_total{};
    std::atomic<bool> failed{false};
    bool baseline{};
    std::string status{"unavailable"};
    DWORD drive_mask{};
    std::chrono::steady_clock::time_point previous=std::chrono::steady_clock::now();

    static void WINAPI event(EVENT_RECORD* record) noexcept {
        auto& self=*static_cast<Impl*>(record->UserContext);
        const auto opcode=record->EventHeader.EventDescriptor.Opcode;
        const bool file=IsEqualGUID(record->EventHeader.ProviderId,file_guid);
        const bool disk=IsEqualGUID(record->EventHeader.ProviderId,disk_guid);
        if((file && opcode!=67 && opcode!=68) || (disk && opcode!=10 && opcode!=11) || (!file && !disk))return;
        try {
            const auto irp=property(record,file?L"IrpPtr":L"Irp");
            const auto fileobj=property(record,L"FileObject");
            const auto when=static_cast<std::uint64_t>(record->EventHeader.TimeStamp.QuadPart);
            std::lock_guard lock(self.mutex);
            if(file) {
                if(!irp || !*irp || !fileobj || !*fileobj)return;
                const DWORD pid=record->EventHeader.ProcessId;
                if(pid<=4)return; // Cache manager / System writes cannot be assigned to a user application.
                if(self.irps.size()>=max_irps && !self.irps.contains(*irp)) {
                    self.failed=true;return;
                }
                self.irps[*irp]={pid,*fileobj,when,opcode==68};
                return;
            }
            const auto number=property(record,L"DiskNumber");
            const auto offset=property(record,L"ByteOffset");
            const auto size=property(record,L"TransferSize");
            if(!number || !offset || !size || !*size || *size>64ULL*1024*1024 ||
                *number>UINT32_MAX) {self.failed=true;return;}
            const auto volume=disk_io_volume_for(self.extents,static_cast<std::uint32_t>(*number),*offset,*size);
            self.physical_total+=*size;
            if(volume)self.volume_total[static_cast<unsigned>(*volume-L'A')]+=*size;
            if(!irp || !*irp || !fileobj || !*fileobj)return; // Count unattributed physical work.
            const auto match=self.irps.find(*irp);
            if(match==self.irps.end())return;
            const auto request=match->second;
            self.irps.erase(match); // An IRP completion is consumed once.
            // Different IRPs, reused pointers or mismatched operation types cannot grant attribution.
            if(request.file!=*fileobj || request.write!=(opcode==11) ||
                when<request.at || when-request.at>10ULL*filetime_second)return;
            auto& bytes=self.per_pid[request.pid];
            if(bytes.first_event==0 || request.at<bytes.first_event)bytes.first_event=request.at;
            if(opcode==11)bytes.write+=*size;else bytes.read+=*size;
            if(volume)bytes.volumes[static_cast<unsigned>(*volume-L'A')]+=*size;
            self.matched_total+=*size;
        } catch (...) { self.failed=true; }
    }
    static ULONG WINAPI buffer(EVENT_TRACE_LOGFILEW* log) noexcept {
        if(log->EventsLost)static_cast<Impl*>(log->Context)->failed=true;
        return TRUE;
    }
    Impl() {
        extents=discover_volumes();drive_mask=GetLogicalDrives();
        GUID suffix{};
        FILETIME created{},exited{},kernel{},user{};
        if(FAILED(CoCreateGuid(&suffix)) ||
            !GetProcessTimes(GetCurrentProcess(),&created,&exited,&kernel,&user))return;
        reclaim_old_sessions();
        swprintf_s(properties.name,L"FluxTop.Disk.%lu.%llx.%08lx",GetCurrentProcessId(),
            static_cast<unsigned long long>(filetime_ticks(created)),suffix.Data1);
        auto& p=properties.trace;
        p.Wnode.BufferSize=sizeof(properties);p.Wnode.Flags=WNODE_FLAG_TRACED_GUID;
        p.Wnode.ClientContext=1;p.Wnode.Guid=suffix;
        p.BufferSize=64;p.MinimumBuffers=2;p.MaximumBuffers=8;p.FlushTimer=1;
        p.LogFileMode=EVENT_TRACE_REAL_TIME_MODE|EVENT_TRACE_SYSTEM_LOGGER_MODE|
            EVENT_TRACE_NO_PER_PROCESSOR_BUFFERING;
        p.EnableFlags=EVENT_TRACE_FLAG_DISK_IO|EVENT_TRACE_FLAG_DISK_FILE_IO|
            EVENT_TRACE_FLAG_FILE_IO|EVENT_TRACE_FLAG_FILE_IO_INIT;
        p.LoggerNameOffset=offsetof(Properties,name);
        const ULONG rc=StartTraceW(&session,properties.name,&p);
        if(rc!=ERROR_SUCCESS) {
            session=0;status=rc==ERROR_ACCESS_DENIED?"administrator-required":"unavailable";
            return;
        }
        EVENT_TRACE_LOGFILEW trace{};
        trace.LoggerName=properties.name;
        trace.ProcessTraceMode=PROCESS_TRACE_MODE_REAL_TIME|PROCESS_TRACE_MODE_EVENT_RECORD;
        trace.EventRecordCallback=event;trace.BufferCallback=buffer;trace.Context=this;
        consumer=OpenTraceW(&trace);
        if(consumer==INVALID_PROCESSTRACE_HANDLE){stop();return;}
        try {
            thread=std::thread([this]{
                const ULONG result=ProcessTrace(&consumer,1,nullptr,nullptr);
                if(result!=ERROR_SUCCESS && result!=ERROR_CANCELLED) {
                    std::lock_guard lock(mutex);failed=true;
                }
            });
            status="ready";
        }catch(...) {stop();throw;}
    }
    void stop() noexcept {
        if(session) {
            ControlTraceW(session,properties.name,&properties.trace,EVENT_TRACE_CONTROL_STOP);
            session=0;
        }
        if(consumer!=INVALID_PROCESSTRACE_HANDLE) {
            CloseTrace(consumer);
            if(thread.joinable())thread.join();
            consumer=INVALID_PROCESSTRACE_HANDLE;
        }
    }
    ~Impl(){stop();}
};
DiskIoCollector::DiskIoCollector():impl_(std::make_unique<Impl>()){}
DiskIoCollector::~DiskIoCollector()=default;
void DiskIoCollector::collect(Snapshot& snapshot) {
    auto& self=*impl_;
    const auto now=std::chrono::steady_clock::now();
    const double seconds=std::chrono::duration<double>(now-self.previous).count();
    self.previous=now;
    if(self.drive_mask!=GetLogicalDrives()) {
        auto next=discover_volumes();
        std::lock_guard lock(self.mutex);
        self.extents=std::move(next);self.drive_mask=GetLogicalDrives();
    }
    std::unordered_map<DWORD,Impl::Bytes> counts;
    std::array<std::uint64_t,26> physical_by_volume{};
    std::uint64_t total{},matched{};
    bool lost{};
    {
        std::lock_guard lock(self.mutex);
        counts.swap(self.per_pid);
        physical_by_volume=self.volume_total;self.volume_total.fill(0);
        total=self.physical_total;matched=self.matched_total;
        self.physical_total=0;self.matched_total=0;
        lost=self.failed.load();
        // IRPs survive sampling boundaries; old ones are evicted to limit reuse.
        constexpr std::uint64_t retention=10ULL*filetime_second;
        const auto latest=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count()/100)+116444736000000000ULL;
        for(auto it=self.irps.begin();it!=self.irps.end();) {
            if(latest>it->second.at && latest-it->second.at>retention)it=self.irps.erase(it);
            else ++it;
        }
    }
    snapshot.disk_io_status=lost?"incomplete":self.status;
    if(self.status!="ready" || lost || !self.baseline || seconds<0.1) {
        self.baseline=true;return;
    }
    if(total==0) {
        // An idle interval is valid only when all available PDH volumes are idle.
        for(const auto& drive:snapshot.disks)if(drive.active_percent && *drive.active_percent>1.0)return;
    }else if(!disk_io_trustworthy(matched,total,false)) {
        snapshot.disk_io_status="partial-attribution";return;
    }
    for(auto& row:snapshot.processes) {
        if(!row.has_baseline || !row.creation_time)continue;
        const auto found=counts.find(row.pid);
        if(found!=counts.end() && found->second.first_event &&
            found->second.first_event<*row.creation_time)continue; // PID reuse.
        const Impl::Bytes* bytes=found==counts.end()?nullptr:&found->second;
        const auto read=bytes?bytes->read:0ULL;
        const auto write=bytes?bytes->write:0ULL;
        row.disk_read_bytes_per_second=static_cast<double>(read)/seconds;
        row.disk_write_bytes_per_second=static_cast<double>(write)/seconds;
        double peak=0.0;
        bool has_pressure=false;
        for(const auto& drive:snapshot.disks) {
            if(drive.letter<L'A' || drive.letter>L'Z')continue;
            const auto i=static_cast<unsigned>(drive.letter-L'A');
            const auto amount=bytes?bytes->volumes[i]:0ULL;
            if(physical_by_volume[i]) {
                row.disk_volume_bytes_per_second[i]=static_cast<double>(amount)/seconds;
                const auto pressure=disk_io_pressure(drive.active_percent,amount,physical_by_volume[i]);
                if(pressure){
                    row.disk_volume_pressure_percent[i]=*pressure;
                    peak=std::max(peak,*pressure);has_pressure=true;
                }
            } else if(drive.active_percent && *drive.active_percent<=1.0 && total==0) {
                row.disk_volume_bytes_per_second[i]=0.0;
                row.disk_volume_pressure_percent[i]=0.0;
                has_pressure=true;
            }
        }
        if(has_pressure)row.disk_pressure_percent=peak;
    }
}
} // namespace fluxtop
