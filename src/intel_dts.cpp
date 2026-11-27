#include "fluxtop/intel_dts.hpp"
#include "fluxtop/cpu_topology_reader.hpp"
#include "pawnio_reader.hpp"
#include <windows.h>
#include <intrin.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <mutex>
#include <thread>
#include <vector>

namespace fluxtop {
void intel_dts_invalidate(CpuTopologySnapshot& sample) noexcept {
    sample.package_c.reset();
    for(auto& core:sample.cores) core.celsius.reset();
    sample.driver_ready=false;
    finalize_core_temperatures(sample);
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
constexpr std::uint64_t temperature_target=0x1A2;
constexpr std::uint64_t package_thermal_status=0x1B1;
constexpr std::uint64_t thermal_status=0x19C;
constexpr std::uint64_t max_age_ms=30000;
bool supports_current_cpu() noexcept {
    const auto vendor=read_cpu_vendor();
    return intel_dts_supports_vendor({vendor.data(),vendor.size()});
}
CpuTopologySnapshot poll(){
    CpuTopologySnapshot sample=read_cpu_topology();
    if(sample.physical_cores==0)return sample;
    PawnIoReader device;
    if(!device.open()){
        sample.status="driver-unavailable";
        return sample;
    }
    if(!device.load(L"IntelMSR.bin")){
        sample.status="module-unavailable";
        return sample;
    }
    sample.driver_ready=true;
    sample.status="partial";
    // Package reading uses one representative logical processor. Each physical
    // core reads its own register using the first logical processor in that core.
    GROUP_AFFINITY prior{};
    bool package_read=false;
    for(auto& current:sample.cores){
        GROUP_AFFINITY target{};
        target.Group=static_cast<WORD>(current.group);
        target.Mask=static_cast<KAFFINITY>(1)<<current.logical;
        if(!SetThreadGroupAffinity(GetCurrentThread(),&target,&prior))continue;
        int cpuid[4]{};
        __cpuidex(cpuid,0x1A,0);
        const unsigned core_type=static_cast<unsigned>(cpuid[0])>>24;
        const bool performance=core_type==0x40;
        std::uint64_t tj{},core_raw{},package_raw{};
        bool target_ok=device.read("ioctl_read_msr",temperature_target,tj);
        bool core_ok=device.read("ioctl_read_msr",thermal_status,core_raw);
        if(!package_read){
            package_read=true;
            if(target_ok && device.read("ioctl_read_msr",package_thermal_status,package_raw))
                sample.package_c=intel_dts_decode(tj,package_raw);
        }
        SetThreadGroupAffinity(GetCurrentThread(),&prior,nullptr);
        // Core type 0x20 means Atom/E-core, 0x40 Core/P-core; Windows
        // EfficiencyClass is a fallback only on CPUs not exposing CPUID 0x1A.
        const bool hybrid_known=core_type==0x20 || core_type==0x40;
        if(hybrid_known)current.performance=performance;
        current.celsius=target_ok && core_ok?intel_dts_decode(tj,core_raw):std::nullopt;
        current.sensor_independent_unverified=core_type==0x20;
    }
    sample.sampled_tick=GetTickCount64();
    finalize_core_temperatures(sample);
    sample.status=sample.partial?"partial":"ready";
    return sample;
}
class CachedIntelDts {
public:
    CachedIntelDts():last_(read_cpu_topology()),supported_(supports_current_cpu()) {
        if(supported_) {
            if(last_.physical_cores)last_.status="initializing";
            worker_=std::jthread([this](std::stop_token stop){run(stop);});
        }
        else last_.status="unsupported-cpu";
    }
    CpuTopologySnapshot read() noexcept {
        std::lock_guard guard(mutex_);
        const auto now=GetTickCount64();
        if(!supported_) {
            // Keep the shared Windows topology current without a DTS polling thread.
            if(now-last_.sampled_tick>=10'000) {
                last_=read_cpu_topology();
                last_.status="unsupported-cpu";
            }
            return last_;
        }
        auto result=last_;
        if(!result.sampled_tick || now<result.sampled_tick || now-result.sampled_tick>max_age_ms){
            intel_dts_invalidate(result);
            result.status="stale";
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
    CpuTopologySnapshot last_;
    bool supported_;
    std::jthread worker_;
};
} // namespace
CpuTopologySnapshot read_intel_dts() noexcept {
    static CachedIntelDts cache;
    return cache.read();
}
} // namespace fluxtop
