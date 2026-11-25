#include "fluxtop/temperature_selection.hpp"
#include <cmath>

namespace fluxtop {
namespace {
constexpr bool board_kind(CpuTemperatureKind kind) noexcept {
    return kind==CpuTemperatureKind::board || kind==CpuTemperatureKind::acpi;
}
constexpr int provider_rank(CpuTemperatureProvider provider) noexcept {
    switch(provider) {
    case CpuTemperatureProvider::intel_dts: return 50;
    case CpuTemperatureProvider::hwinfo: return 40;
    case CpuTemperatureProvider::libre_wmi: return 30;
    case CpuTemperatureProvider::open_wmi: return 20;
    case CpuTemperatureProvider::acpi: return 10;
    }
    return 0;
}
}
int cpu_temperature_rank(CpuTemperatureProvider provider,CpuTemperatureKind kind) noexcept {
    const int sensor=board_kind(kind)?0:1000;
    const int type=kind==CpuTemperatureKind::package?50:
        kind==CpuTemperatureKind::die?40:
        kind==CpuTemperatureKind::core_average?30:
        kind==CpuTemperatureKind::core?20:
        kind==CpuTemperatureKind::board?10:0;
    return sensor+provider_rank(provider)+type;
}
bool cpu_temperature_candidate_valid(const CpuTemperatureCandidate& sample,
                                     std::uint64_t now,std::uint64_t ttl_ms) noexcept {
    return sample.celsius && std::isfinite(*sample.celsius) &&
        *sample.celsius>=-10.0 && *sample.celsius<=125.0 &&
        sample.sampled_tick!=0 && now>=sample.sampled_tick &&
        now-sample.sampled_tick<=ttl_ms;
}
bool cpu_temperature_needs_wmi(std::span<const CpuTemperatureCandidate> primary,std::uint64_t now,
                              std::uint64_t preferred_started_at) noexcept {
    constexpr std::uint64_t startup_grace_ms=2000;
    if(preferred_started_at && now>=preferred_started_at && now-preferred_started_at<startup_grace_ms)return false;
    const int best_wmi=cpu_temperature_rank(CpuTemperatureProvider::libre_wmi,CpuTemperatureKind::die);
    for(const auto& candidate:primary)
        if(cpu_temperature_candidate_valid(candidate,now) && cpu_temperature_rank(candidate.provider,candidate.kind)>best_wmi)
            return false;
    return true;
}
CpuTemperatureSelection CpuTemperatureSelector::select(std::span<const CpuTemperatureCandidate> samples,
                                                        std::uint64_t now) noexcept {
    CpuTemperatureSelection result;
    const CpuTemperatureCandidate* best=nullptr;
    const CpuTemperatureCandidate* active=nullptr;
    for(const auto& sample:samples){
        if(!cpu_temperature_candidate_valid(sample,now))continue;
        if(!best || cpu_temperature_rank(sample.provider,sample.kind)>
            cpu_temperature_rank(best->provider,best->kind))best=&sample;
        if(active_provider_ && sample.provider==*active_provider_ && sample.kind==active_kind_)
            active=&sample;
    }
    if(!best){
        active_provider_.reset();pending_provider_.reset();pending_count_=0;
        return result;
    }
    if(!active || !active_provider_) {
        // Lost source: immediately fall back; never publish stale samples.
        active=best;active_provider_=best->provider;active_kind_=best->kind;
        pending_provider_.reset();pending_count_=0;
    } else if(best->provider==*active_provider_ && best->kind==active_kind_) {
        pending_provider_.reset();pending_count_=0;
    } else if(cpu_temperature_rank(best->provider,best->kind)>
              cpu_temperature_rank(active->provider,active->kind)) {
        // Two consecutive healthy observations suppress one-cycle provider flapping.
        if(pending_provider_ && *pending_provider_==best->provider && pending_kind_==best->kind)
            ++pending_count_;
        else {pending_provider_=best->provider;pending_kind_=best->kind;pending_count_=1;}
        if(pending_count_>=2){
            active=best;active_provider_=best->provider;active_kind_=best->kind;
            pending_provider_.reset();pending_count_=0;
        }
    } else {
        pending_provider_.reset();pending_count_=0;
    }
    result.celsius=active->celsius;
    result.provider=active->provider;
    result.kind=active->kind;
    result.sampled_tick=active->sampled_tick;
    return result;
}
std::string_view cpu_temperature_kind_name(CpuTemperatureKind kind) noexcept {
    switch(kind){
    case CpuTemperatureKind::package: return "cpu-package";
    case CpuTemperatureKind::die: return "cpu-die";
    case CpuTemperatureKind::core_average: return "core-average";
    case CpuTemperatureKind::core: return "core";
    case CpuTemperatureKind::board: return "cpu-socket-board";
    case CpuTemperatureKind::acpi: return "cpu-acpi-zone";
    }
    return "unknown";
}
} // namespace fluxtop
