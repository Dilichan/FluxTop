#include "fluxtop/disk_attribution.hpp"
#include "fluxtop/model.hpp"
#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace {
void need(bool yes,const char* message) {
    if(!yes)throw std::runtime_error(message);
}
struct Request { std::uint64_t at{}; };
}
int main() {
    try {
        using namespace fluxtop;
        constexpr std::uint64_t tick=10000000ULL;
        std::unordered_map<std::uint64_t,Request> file_requests{
            {1,{10*tick}},{2,{20*tick}},{3,{0}}
        };
        need(disk_io_expire_requests(file_requests,21*tick)==2,
            "stale/uninitialized cached FileIo requests must expire");
        need(file_requests.size()==1 && file_requests.contains(2),
            "only recent FileIo request remains eligible");
        need(disk_io_expire_requests(file_requests,21*tick)==0,
            "expiry must be idempotent");
        need(disk_io_expire_requests(file_requests,31*tick)==1,
            "uncompleted IRP must not remain indefinitely");
        std::unordered_map<std::uint64_t,Request> init_requests{{11,{20*tick}}};
        need(disk_io_expire_requests(init_requests,19*tick)==0,
            "out-of-order callback must not expire newer requests");
        need(disk_io_expire_requests(init_requests,31*tick)==1,
            "DiskIo-init cache obeys the same lifetime limit");
        need(disk_io_valid_completion(20*tick,30*tick,false,false),
            "read completion exactly at TTL remains valid");
        need(!disk_io_valid_completion(20*tick,30*tick+1,false,false),
            "late completion is invalid, matching expiry threshold");
        need(!disk_io_pid_lifetime_valid(false,100ULL,110ULL),
            "PID with no baseline must not inherit ETW bytes");
        need(!disk_io_pid_lifetime_valid(true,std::nullopt,110ULL),
            "PID with unknown creation time must not inherit ETW bytes");
        need(!disk_io_pid_lifetime_valid(true,150ULL,140ULL),
            "reused PID must not inherit older physical writes");
        need(disk_io_pid_lifetime_valid(true,150ULL,150ULL),
            "physical request at creation time is valid");
        need(disk_io_pid_lifetime_valid(true,150ULL,0ULL),
            "confirmed idle interval may publish true zero");
        need(!disk_io_fully_attributed(0,0,false) &&
            !disk_io_publishable_bytes(0,0,false),
            "ETW silence must remain unknown, not 0 B/s");
        need(!disk_io_fully_attributed(0,4096,false) &&
            !disk_io_publishable_bytes(0,4096,false),
            "unattributed physical activity must remain unknown");
        need(disk_io_publishable_bytes(4096,8192,false) &&
            !disk_io_fully_attributed(4096,8192,false),
            "verified subset is a lower bound, not an exact measurement");
        need(!disk_io_publishable_bytes(4096,8192,true),
            "event loss invalidates an otherwise positive sample");
        need(disk_io_volume_zero_certified(4096,4096),
            "only completely observed volume may publish a zero");
        need(!disk_io_volume_zero_certified(0,0) &&
            !disk_io_volume_zero_certified(8192,4096),
            "silent or partially attributed volume must remain unknown");
        const std::vector<PhysicalVolumeExtent> volumes{
            {0,0,1024*1024,L'C'}, {0,2*1024*1024,1024*1024,L'D'}
        };
        need(disk_io_volume_for(volumes,0,128,4096)==L'C',
            "Disk C transfer must map exclusively to C");
        need(disk_io_volume_for(volumes,0,2*1024*1024+128,4096)==L'D',
            "Disk D transfer must map exclusively to D");
        need(!disk_io_volume_for(volumes,0,1024*1024-1024,4096),
            "transfer crossing volume boundary must remain unattributed");
        Process a,b;
        a.pid=101; a.creation_time=10; a.name="game.exe";
        a.exe_path="c:/games/game.exe";a.has_baseline=true;
        a.disk_read_bytes_per_second=1024.0;
        a.disk_write_bytes_per_second=2048.0;
        a.disk_volume_bytes_per_second[L'C'-L'A']=3072.0;
        b=a;b.pid=102;b.creation_time=11;
        b.disk_read_bytes_per_second=512.0;
        b.disk_write_bytes_per_second=0.0; // explicitly measured zero
        b.disk_volume_bytes_per_second[L'C'-L'A']=512.0;
        const auto grouped=aggregate_application_processes({a,b});
        need(grouped.size()==1 && grouped[0].member_pids.size()==2,
            "two child PIDs group into one application");
        need(grouped[0].disk_read_bytes_per_second==1536.0 &&
            grouped[0].disk_write_bytes_per_second==2048.0,
            "physical read/write are summed once across grouped PIDs");
        need(grouped[0].disk_volume_bytes_per_second[L'C'-L'A']==3584.0 &&
            !grouped[0].disk_volume_bytes_per_second[L'D'-L'A'],
            "Disk C stays independent; unknown Disk D must not become zero");
        b.disk_read_bytes_per_second.reset();
        b.disk_write_bytes_per_second.reset();
        b.disk_volume_bytes_per_second[L'C'-L'A'].reset();
        const auto partial=aggregate_application_processes({a,b});
        need(partial.size()==1 && partial[0].disk_read_bytes_per_second==1024.0 &&
            partial[0].disk_write_bytes_per_second==2048.0,
            "one unmeasured PID does not erase verified sibling readings");
        a.disk_read_bytes_per_second.reset();
        a.disk_write_bytes_per_second.reset();
        a.disk_volume_bytes_per_second[L'C'-L'A'].reset();
        const auto unknown=aggregate_application_processes({a,b});
        need(unknown.size()==1 && !unknown[0].disk_read_bytes_per_second &&
            !unknown[0].disk_write_bytes_per_second &&
            !unknown[0].disk_volume_bytes_per_second[L'C'-L'A'],
            "unknown data remains nullopt throughout application aggregation");
        std::cout << "v38 disk ETW lifetime, per-volume and grouping pipeline passed\n";
    } catch(const std::exception& error) {
        std::cerr << "v38 disk pipeline: " << error.what() << '\n';
        return 1;
    }
    return 0;
}