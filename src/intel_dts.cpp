#include "fluxtop/intel_dts.hpp"
#include <windows.h>
#include <intrin.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>

namespace fluxtop {
// A missing core is represented by nullopt. Never include it in aggregates.
void intel_dts_finalize(IntelDtsSnapshot& sample) noexcept {
    sample.physical_cores=static_cast<unsigned>(sample.cores.size());
    sample.available_cores=0;
    sample.core_max_c.reset();
    sample.core_average_c.reset();
    double sum=0.0;
    for(const auto& core:sample.cores){
        if(!core.celsius || !std::isfinite(*core.celsius) ||
            *core.celsius < -10.0 || *core.celsius>125.0)continue;
        ++sample.available_cores;
        sum+=*core.celsius;
        if(!sample.core_max_c || *sample.core_max_c<*core.celsius)
            sample.core_max_c=*core.celsius;
    }
    if(sample.available_cores)
        sample.core_average_c=sum/static_cast<double>(sample.available_cores);
    sample.partial=sample.physical_cores==0 ||
        sample.available_cores<sample.physical_cores || !sample.package_c;
}
std::optional<double> intel_dts_decode(std::uint64_t target,std::uint64_t status) noexcept {
    constexpr std::uint64_t valid=1ULL<<31;
    if(!(status&valid))return std::nullopt;
    const unsigned tjmax=static_cast<unsigned>((target>>16)&0xFF);
    const unsigned distance=static_cast<unsigned>((status>>16)&0x7F);
    if(tjmax<60 || tjmax>125 || distance>tjmax)return std::nullopt;
    const double celsius=static_cast<double>(tjmax-distance);
    return celsius>=-10.0 && celsius<=125.0 ? std::optional<double>{celsius}:std::nullopt;
}
namespace {
constexpr DWORD load_binary=(41394UL<<16)|(0x821UL<<2);
constexpr DWORD execute_function=(41394UL<<16)|(0x841UL<<2);
constexpr std::uint64_t temperature_target=0x1A2;
constexpr std::uint64_t package_thermal_status=0x1B1;
constexpr std::uint64_t thermal_status=0x19C;
constexpr std::uint64_t max_age_ms=30000;
struct LogicalCore { WORD group{}; KAFFINITY mask{}; unsigned logical{}; unsigned core{}; unsigned siblings{}; bool performance{}; };
struct WinHandle {
    HANDLE h{INVALID_HANDLE_VALUE};
    ~WinHandle(){if(h!=INVALID_HANDLE_VALUE)CloseHandle(h);}
    WinHandle()=default;
    WinHandle(const WinHandle&)=delete;
    WinHandle& operator=(const WinHandle&)=delete;
};
std::filesystem::path module_path(){
    std::wstring buffer(32768,L'\0');
    DWORD n=GetModuleFileNameW(nullptr,buffer.data(),static_cast<DWORD>(buffer.size()));
    if(!n || n>=buffer.size())return {};
    buffer.resize(n);
    return std::filesystem::path(buffer).parent_path()/L"IntelMSR.bin";
}
bool load_module(HANDLE handle){
    auto path=module_path();
    if(path.empty())return false;
    std::ifstream file(path,std::ios::binary|std::ios::ate);
    if(!file)return false;
    const auto size=file.tellg();
    if(size<=0 || size>65536)return false;
    std::vector<char> bytes(static_cast<std::size_t>(size));
    file.seekg(0,std::ios::beg);
    if(!file.read(bytes.data(),size))return false;
    DWORD returned{};
    return DeviceIoControl(handle,load_binary,bytes.data(),static_cast<DWORD>(bytes.size()),
        nullptr,0,&returned,nullptr)!=FALSE;
}
bool read_msr(HANDLE handle,std::uint64_t index,std::uint64_t& value){
    std::array<std::uint8_t,40> input{};
    constexpr char name[]="ioctl_read_msr";
    std::memcpy(input.data(),name,sizeof(name)-1);
    std::memcpy(input.data()+32,&index,sizeof(index));
    DWORD returned{};
    value=0;
    return DeviceIoControl(handle,execute_function,input.data(),static_cast<DWORD>(input.size()),
        &value,sizeof(value),&returned,nullptr)!=FALSE && returned==sizeof(value);
}
bool intel_processor(){
    int data[4]{};
    __cpuid(data,0);
    return static_cast<unsigned>(data[1])==0x756e6547U &&
        static_cast<unsigned>(data[3])==0x49656e69U &&
        static_cast<unsigned>(data[2])==0x6c65746eU;
}
std::vector<LogicalCore> cpu_topology(){
    DWORD bytes=0;
    GetLogicalProcessorInformationEx(RelationProcessorCore,nullptr,&bytes);
    if(!bytes || bytes>1024*1024)return {};
    std::vector<std::uint8_t> blob(bytes);
    if(!GetLogicalProcessorInformationEx(RelationProcessorCore,
        reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(blob.data()),&bytes))return {};
    std::vector<LogicalCore> result;
    unsigned core_id=0;
    for(DWORD pos=0;pos+sizeof(ULONG)*2<=bytes;){
        const auto* item=reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(blob.data()+pos);
        if(item->Size<sizeof(ULONG)*2 || item->Size>bytes-pos)break;
        if(item->Relationship==RelationProcessorCore){
            unsigned siblings=0;
            for(WORD gi=0;gi<item->Processor.GroupCount;++gi) {
                const KAFFINITY mask=item->Processor.GroupMask[gi].Mask;
                for(unsigned bit=0;bit<sizeof(KAFFINITY)*8;++bit)
                    if(mask&(static_cast<KAFFINITY>(1)<<bit))++siblings;
            }
            for(WORD gi=0;gi<item->Processor.GroupCount;++gi){
                const GROUP_AFFINITY ga=item->Processor.GroupMask[gi];
                for(unsigned bit=0;bit<sizeof(KAFFINITY)*8;++bit){
                    KAFFINITY mask=static_cast<KAFFINITY>(1)<<bit;
                    if(ga.Mask&mask)result.push_back({ga.Group,mask,bit,core_id,
                        siblings,item->Processor.EfficiencyClass==0});
                }
            }
            ++core_id;
        }
        pos+=item->Size;
    }
    return result;
}
IntelDtsSnapshot poll(){
    IntelDtsSnapshot sample;
    if(!intel_processor())return sample;
    WinHandle device;
    device.h=CreateFileW(L"\\\\?\\GLOBALROOT\\Device\\PawnIO",GENERIC_READ|GENERIC_WRITE,
        FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(device.h==INVALID_HANDLE_VALUE || !load_module(device.h))return sample;
    sample.driver_ready=true;
    auto topology=cpu_topology();
    if(topology.empty())return sample;
    sample.logical_processors=static_cast<unsigned>(topology.size());
    // Package reading uses one representative logical processor. Each physical
    // core reads its own register using the first logical processor in that core.
    GROUP_AFFINITY prior{};
    bool package_read=false;
    unsigned previous_core=~0U;
    for(const auto& logical:topology){
        if(logical.core==previous_core)continue;
        previous_core=logical.core;
        GROUP_AFFINITY target{};
        target.Group=logical.group;
        target.Mask=logical.mask;
        if(!SetThreadGroupAffinity(GetCurrentThread(),&target,&prior)){
            sample.cores.push_back({logical.group,logical.logical,logical.core,
                logical.performance,std::nullopt,logical.siblings,true});
            continue;
        }
        int cpuid[4]{};
        __cpuidex(cpuid,0x1A,0);
        const unsigned core_type=static_cast<unsigned>(cpuid[0])>>24;
        const bool performance=core_type==0x40;
        std::uint64_t tj{},core_raw{},package_raw{};
        bool target_ok=read_msr(device.h,temperature_target,tj);
        bool core_ok=read_msr(device.h,thermal_status,core_raw);
        if(!package_read){
            package_read=true;
            if(target_ok && read_msr(device.h,package_thermal_status,package_raw))
                sample.package_c=intel_dts_decode(tj,package_raw);
        }
        SetThreadGroupAffinity(GetCurrentThread(),&prior,nullptr);
        // Core type 0x20 means Atom/E-core, 0x40 Core/P-core; Windows
        // EfficiencyClass is a fallback only on CPUs not exposing CPUID 0x1A.
        const bool hybrid_known=core_type==0x20 || core_type==0x40;
        sample.cores.push_back({logical.group,logical.logical,logical.core,
            hybrid_known?performance:logical.performance,
            target_ok && core_ok?intel_dts_decode(tj,core_raw):std::nullopt,
            logical.siblings,core_type==0x20});
    }
    sample.sampled_tick=GetTickCount64();
    intel_dts_finalize(sample);
    return sample;
}
class CachedIntelDts {
public:
    CachedIntelDts():worker_([this](std::stop_token stop){run(stop);}){}
    IntelDtsSnapshot read() noexcept {
        std::lock_guard guard(mutex_);
        auto result=last_;
        const auto now=GetTickCount64();
        if(!result.sampled_tick || now<result.sampled_tick || now-result.sampled_tick>max_age_ms){
            result.package_c.reset();
            for(auto& core:result.cores)core.celsius.reset();
            intel_dts_finalize(result);
        }
        return result;
    }
private:
    void run(std::stop_token stop){
        while(!stop.stop_requested()){
            auto next=poll();
            {std::lock_guard guard(mutex_);last_=std::move(next);}
            for(int i=0;i<20 && !stop.stop_requested();++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    std::mutex mutex_;
    IntelDtsSnapshot last_;
    std::jthread worker_;
};
} // namespace
IntelDtsSnapshot read_intel_dts() noexcept {
    static CachedIntelDts cache;
    return cache.read();
}
} // namespace fluxtop