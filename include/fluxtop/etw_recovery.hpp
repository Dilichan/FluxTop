#pragma once
#include <algorithm>
#include <chrono>
namespace fluxtop {
class EtwRecovery {
public:
    using Clock=std::chrono::steady_clock;
    bool schedule(Clock::time_point now) noexcept {
        if(pending_)return false;
        deadline_=now+std::chrono::seconds(std::min(60U,3U<<std::min(attempts_,5U)));
        pending_=true;return true;
    }
    bool due(Clock::time_point now)const noexcept{return pending_ && now>=deadline_;}
    void restarted(Clock::time_point now)noexcept{pending_=false;last_restart_=now;attempts_=std::min(attempts_+1,6U);}
    void healthy(Clock::time_point now)noexcept{if(!pending_ && now-last_restart_>=std::chrono::seconds(60))attempts_=0;}
private:
    Clock::time_point deadline_{},last_restart_{};
    unsigned attempts_{};
    bool pending_{};
};
}
