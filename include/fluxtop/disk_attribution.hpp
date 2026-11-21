#pragma once
// Conservative, transport-agnostic ETW disk attribution primitives.
// SATA, NVMe, SAS and mechanical HDDs all expose physical disk offsets through ETW.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <unordered_map>
#include <vector>

namespace fluxtop {
struct PhysicalVolumeExtent {
    std::uint32_t disk{};
    std::uint64_t offset{},length{};
    wchar_t letter{};
};
// Return a letter only if an operation belongs in precisely one entire volume extent.
// Multi-volume, cross-boundary and unknown operations remain unattributed to a letter.
inline std::optional<wchar_t> disk_io_volume_for(const std::vector<PhysicalVolumeExtent>& extents,
    std::uint32_t disk,std::uint64_t offset,std::uint64_t bytes) noexcept {
    if(!bytes || offset>UINT64_MAX-bytes)return std::nullopt;
    std::optional<wchar_t> found;
    for(const auto& extent:extents) {
        if(extent.disk!=disk || !extent.length || extent.offset>UINT64_MAX-extent.length)continue;
        if(offset>=extent.offset && offset+bytes<=extent.offset+extent.length) {
            if(found && *found!=extent.letter)return std::nullopt;
            found=extent.letter;
        }
    }
    return found;
}
// Full coverage certifies zeros for an observed interval. Any fewer matched
// bytes is partial coverage, regardless of the storage technology.
inline bool disk_io_fully_attributed(std::uint64_t matched,std::uint64_t total,
    bool lost_events) noexcept {
    return !lost_events && total>0 && matched==total;
}
// A positively matched subset can be shown as a lower bound even when
// not all observed physical operations can be attributed. Missing matches
// must never be presented as zero, and lost events invalidate the interval.
inline bool disk_io_publishable_bytes(std::uint64_t matched,std::uint64_t total,
    bool lost_events) noexcept {
    return !lost_events && matched>0 && matched<=total;
}
// Per-sampling-interval accounting for positively attributed physical bytes.
// Each matched byte must be published or assigned exactly one drop reason.
// This is diagnostic only: it never changes process measurements or ETW attribution.
struct DiskDeliveryAccounting {
    std::uint64_t matched{},queued{},in_snapshot{},published{};
    std::uint64_t no_pid{},no_baseline{},pid_reused{};
    std::uint64_t no_session{},lost_events{},startup{},short_interval{};
    std::uint64_t invalid_coverage{},unpublishable{};
    void add(const DiskDeliveryAccounting& other) noexcept {
        matched+=other.matched;queued+=other.queued;in_snapshot+=other.in_snapshot;
        published+=other.published;no_pid+=other.no_pid;
        no_baseline+=other.no_baseline;pid_reused+=other.pid_reused;
        no_session+=other.no_session;lost_events+=other.lost_events;
        startup+=other.startup;short_interval+=other.short_interval;
        invalid_coverage+=other.invalid_coverage;unpublishable+=other.unpublishable;
    }
    bool reconciled() const noexcept {
        if(matched!=queued || in_snapshot>queued)return false;
        std::uint64_t remaining=matched;
        for(const auto value:{published,no_pid,no_baseline,pid_reused,no_session,
            lost_events,startup,short_interval,invalid_coverage,unpublishable}) {
            if(value>remaining)return false;
            remaining-=value;
        }
        return remaining==0;
    }
};
// Bounded ETW thread-lifetime table. DCStart identifies pre-existing threads,
// Start updates reused TIDs, End removes them. A mapping is usable only for
// events at or after that lifetime observation; missing state never guesses PID.
class DiskThreadOwners {
public:
    bool observe(std::uint32_t tid,std::uint32_t pid,std::uint64_t at) {
        if(!tid || !pid || !at)return false;
        const auto found=owners_.find(tid);
        if(found!=owners_.end() && at<found->second.at)return false;
        if(found==owners_.end() && owners_.size()>=capacity)return false;
        owners_[tid]={pid,at};
        return true;
    }
    void finish(std::uint32_t tid,std::uint64_t at) {
        const auto found=owners_.find(tid);
        if(found!=owners_.end() && at>=found->second.at)owners_.erase(found);
    }
    std::optional<std::uint32_t> find(std::uint32_t tid,std::uint64_t at) const {
        const auto found=owners_.find(tid);
        if(found==owners_.end() || at<found->second.at)return std::nullopt;
        return found->second.pid;
    }
    std::size_t size() const noexcept {return owners_.size();}
private:
    struct Entry {std::uint32_t pid{};std::uint64_t at{};};
    static constexpr std::size_t capacity=65536;
    std::unordered_map<std::uint32_t,Entry> owners_;
};
// DiskIo ReadInit/WriteInit and completion must refer to the same IRP,
// operation direction and a bounded, monotonic FILETIME interval.
// A cached FileIo operation is not proof that any physical I/O occurred.
inline bool disk_io_valid_completion(std::uint64_t started,std::uint64_t finished,
    bool started_write,bool finished_write) noexcept {
    constexpr std::uint64_t max_request_age=100000000ULL; // 10 seconds, FILETIME ticks
    return started!=0 && finished>=started && finished-started<=max_request_age &&
        started_write==finished_write;
}
// Heuristic: apportion measured volume active time by its fraction of *physical* bytes.
// Not a promise of the process's precise service time or a benchmark of the device.
inline std::optional<double> disk_io_pressure(std::optional<double> busy,
    std::uint64_t attributed_bytes,std::uint64_t volume_bytes) noexcept {
    if(!busy || !std::isfinite(*busy) || *busy<0 || volume_bytes==0 ||
        attributed_bytes>volume_bytes)return std::nullopt;
    return std::clamp(*busy,0.0,100.0)*
        static_cast<double>(attributed_bytes)/static_cast<double>(volume_bytes);
}
}
