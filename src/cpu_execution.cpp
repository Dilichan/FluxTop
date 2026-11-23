#include "fluxtop/cpu_execution.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <cstring>

namespace fluxtop {
std::optional<std::pair<std::uint32_t,std::uint32_t>> cpu_switch_threads(unsigned version,std::span<const std::uint8_t> payload) {
    // V2+ retain the prefix consumed by Microsoft's CSwitchTraceData.
    // Windows 11 V5 appends four bytes; unknown future layouts still fail closed.
    if(version<2 || version>5 || payload.size()<(version==5?28U:24U))return std::nullopt;
    std::uint32_t next{},previous{};
    std::memcpy(&next,payload.data(),4);std::memcpy(&previous,payload.data()+4,4);
    return std::pair{next,previous};
}
namespace {
struct IdentityHash {
    std::size_t operator()(const CpuExecutionIdentity& v) const noexcept {
        return std::hash<std::uint64_t>{}(v.creation)^(static_cast<std::size_t>(v.pid)*0x9e3779b1U);
    }
};
std::uint64_t nanoseconds(std::uint64_t ticks,std::uint64_t frequency) {
    return static_cast<std::uint64_t>(static_cast<long double>(ticks)*1000000000.0L/frequency);
}
}
struct CpuExecutionTracker::Impl {
    struct Process { CpuExecutionIdentity identity;std::uint64_t at; };
    struct Thread { CpuExecutionIdentity owner;std::uint64_t at,generation;bool seeded; };
    struct Cpu { bool initialized{};std::uint32_t tid{};std::uint64_t at{},generation{},unknown{};
        std::optional<CpuExecutionIdentity> owner; };
    struct Account { std::vector<std::uint64_t> ticks; };
    std::vector<std::pair<unsigned,unsigned>> topology;
    std::vector<Cpu> cpus;
    std::unordered_map<std::uint32_t,Process> processes;
    std::unordered_map<std::uint32_t,Thread> threads;
    std::unordered_map<CpuExecutionIdentity,Account,IdentityHash> accounts;
    using Running=std::unordered_map<std::uint32_t,std::size_t>;
    Running running;
    std::vector<Running::node_type> running_nodes;
    std::uint64_t frequency{},begin{},generation{},switches{};
    Limits limits;
    std::string failure;
    std::string detail;
    Impl(std::vector<std::pair<unsigned,unsigned>> values,std::uint64_t freq,Limits maximum)
        :topology(std::move(values)),cpus(topology.size()),frequency(freq),limits(maximum) {
        threads.reserve(std::min<std::size_t>(limits.threads,8192));
        processes.reserve(std::min<std::size_t>(limits.processes,1024));
        accounts.reserve(std::min<std::size_t>(limits.processes,1024));
        running.reserve(cpus.size()*2);
        running_nodes.reserve(cpus.size());
        // One reusable node per CPU. Idle/busy switches must not allocate and
        // free a hash-table node on every high-frequency scheduler event.
        for(std::size_t i=0;i<cpus.size();++i) {
            const auto entry=running.emplace(static_cast<std::uint32_t>(i+1),i).first;
            running_nodes.push_back(running.extract(entry));
        }
        if(!frequency || cpus.empty() || cpus.size()>4096)failure="topology-unavailable";
        auto sorted=topology;std::sort(sorted.begin(),sorted.end());
        if(std::adjacent_find(sorted.begin(),sorted.end())!=sorted.end())failure="topology-unavailable";
    }
    void fail(std::string reason) { if(failure.empty())failure=std::move(reason); }
    void charge(Cpu& cpu,std::uint64_t at) {
        if(!cpu.initialized || at<cpu.at) { fail("out-of-order");return; }
        const auto elapsed=at-cpu.at;
        if(cpu.tid && !cpu.owner)cpu.unknown+=elapsed;
        else if(cpu.tid && cpu.owner && elapsed) {
            auto found=accounts.find(*cpu.owner);
            if(found==accounts.end()) {
                if(accounts.size()>=limits.processes ||
                    (accounts.size()+1)>limits.cells/cpus.size()) { fail("state-capacity-exceeded");return; }
                found=accounts.emplace(*cpu.owner,Account{std::vector<std::uint64_t>(cpus.size())}).first;
            }
            const auto index=static_cast<std::size_t>(&cpu-cpus.data());
            found->second.ticks[index]+=elapsed;
        }
        cpu.at=at;
    }
};
CpuExecutionTracker::CpuExecutionTracker(std::vector<std::pair<unsigned,unsigned>> values,std::uint64_t frequency,Limits limits)
    :impl_(std::make_unique<Impl>(std::move(values),frequency,limits)){}
CpuExecutionTracker::CpuExecutionTracker(std::vector<std::pair<unsigned,unsigned>> values,std::uint64_t frequency)
    :CpuExecutionTracker(std::move(values),frequency,Limits{}){}
CpuExecutionTracker::~CpuExecutionTracker()=default;
void CpuExecutionTracker::invalidate(std::string reason){impl_->fail(std::move(reason));}
bool CpuExecutionTracker::has_process(std::uint32_t pid)const{return impl_->processes.contains(pid);}
std::size_t CpuExecutionTracker::thread_count()const{return impl_->threads.size();}
const std::string& CpuExecutionTracker::failure_detail()const{return impl_->detail;}
void CpuExecutionTracker::process_start(std::uint32_t pid,std::uint64_t creation,std::uint64_t at) {
    auto& s=*impl_;if(!pid || !creation || !at)return;
    if(const auto found=s.processes.find(pid);found!=s.processes.end()) {
        if(at<found->second.at)return; // Delayed rundown must not overwrite a newer observed PID.
        if(found->second.identity.creation==creation)return;
        process_end(pid,at);
    }
    if(s.processes.size()>=s.limits.processes){s.fail("state-capacity-exceeded");return;}
    s.processes.emplace(pid,Impl::Process{{pid,creation},at});
}
void CpuExecutionTracker::process_end(std::uint32_t pid,std::uint64_t at) {
    auto& s=*impl_;const auto found=s.processes.find(pid);
    if(found==s.processes.end() || at<found->second.at)return;
    const auto identity=found->second.identity;s.processes.erase(found);
    for(auto it=s.threads.begin();it!=s.threads.end();)
        if(it->second.owner==identity && it->second.at<=at)it=s.threads.erase(it);else ++it;
    // Running states retain the captured old generation until switch-out.
}
void CpuExecutionTracker::thread_start(std::uint32_t tid,std::uint32_t pid,std::uint64_t at,bool rundown) {
    auto& s=*impl_;if(!tid || !at)return;
    const auto process=s.processes.find(pid);
    if(process==s.processes.end() || at<process->second.at)return; // Unknown/older PID stays unknown.
    if(const auto found=s.threads.find(tid);found!=s.threads.end()) {
        if(at<found->second.at)return;
        if((rundown || found->second.seeded) && found->second.owner==process->second.identity) {
            found->second.seeded=rundown;return;
        }
        // An active TID cannot be reused without a corresponding lifetime end.
        s.fail("thread-lifetime-discontinuity");s.threads.erase(found);
    }
    if(s.threads.size()>=s.limits.threads){s.fail("state-capacity-exceeded");return;}
    const auto gen=++s.generation;
    s.threads.emplace(tid,Impl::Thread{process->second.identity,at,gen,rundown});
    if(const auto live=s.running.find(tid);live!=s.running.end()) {
        auto& cpu=s.cpus[live->second];
        if(at>=cpu.at && !cpu.owner) {
            s.charge(cpu,at);cpu.owner=process->second.identity;cpu.generation=gen;
        }
    }
}
void CpuExecutionTracker::thread_end(std::uint32_t tid,std::uint32_t pid,std::uint64_t at) {
    auto& s=*impl_;const auto found=s.threads.find(tid);
    if(found!=s.threads.end() && found->second.owner.pid==pid && at>=found->second.at)s.threads.erase(found);
}
void CpuExecutionTracker::context_switch(std::size_t index,std::uint32_t old_tid,std::uint32_t new_tid,std::uint64_t at) {
    auto& s=*impl_;if(!at || index>=s.cpus.size()){s.fail("processor-unavailable");return;}
    auto& cpu=s.cpus[index];++s.switches;
    if(cpu.initialized) {
        if(at<cpu.at){s.fail("out-of-order");return;}
        if(cpu.tid!=old_tid){
            if(s.failure.empty())s.detail="cpu="+std::to_string(index)+" expected_tid="+std::to_string(cpu.tid)+
                " old_tid="+std::to_string(old_tid)+" new_tid="+std::to_string(new_tid)+" at="+std::to_string(at);
            s.fail("switch-discontinuity");cpu.owner.reset();
        }
        s.charge(cpu,at);
        if(cpu.tid) {
            const auto previous=s.running.find(cpu.tid);
            if(previous!=s.running.end() && previous->second==index)s.running_nodes[index]=s.running.extract(previous);
        }
    } else {
        cpu.initialized=true;
        // Unknown initial tail is not assigned to OldThreadId.
        if(s.begin && at>s.begin)cpu.unknown+=at-s.begin;
    }
    cpu.at=at;cpu.tid=new_tid;cpu.owner.reset();cpu.generation=0;
    if(new_tid) {
        if(s.running.contains(new_tid)){s.fail("overlapping-thread-execution");return;}
        auto& node=s.running_nodes[index];
        if(!node){s.fail("switch-discontinuity");return;}
        node.key()=new_tid;node.mapped()=index;
        s.running.insert(std::move(node));
        const auto owner=s.threads.find(new_tid);
        if(owner!=s.threads.end() && owner->second.at<=at){cpu.owner=owner->second.owner;cpu.generation=owner->second.generation;}
    }
}
CpuExecutionWindow CpuExecutionTracker::finish(std::uint64_t at) {
    auto& s=*impl_;CpuExecutionWindow window;
    window.topology=s.topology;window.frequency=s.frequency;window.begin_qpc=s.begin;window.end_qpc=at;window.switches=s.switches;
    if(!at || (s.begin && at<=s.begin)){window.status="stale";return window;}
    for(auto& cpu:s.cpus)if(cpu.initialized)s.charge(cpu,at);
    const auto elapsed=s.begin ? at-s.begin : 0;
    window.complete.reserve(s.cpus.size());
    for(auto& cpu:s.cpus) {
        const bool complete=cpu.initialized && cpu.unknown==0;
        window.complete.push_back(complete);window.unknown_ticks+=cpu.initialized?cpu.unknown:elapsed;cpu.unknown=0;
    }
    if(!s.failure.empty())window.status=s.failure;
    else if(!s.begin)window.status="baseline";
    else if(!elapsed || elapsed/s.frequency>30)window.status="stale";
    else window.status=std::all_of(window.complete.begin(),window.complete.end(),[](bool value){return value;})?"ready":"partial";
    const bool usable=window.status=="ready" || window.status=="partial";
    for(const auto& [pid,process]:s.processes) {
        (void)pid;if(process.at<=at)window.known_processes.push_back(process.identity);
    }
    if(usable)for(const auto& [identity,account]:s.accounts) {
        CpuProcessExecution execution;execution.status=window.status;
        execution.begin_qpc=s.begin;execution.end_qpc=at;execution.frequency=s.frequency;
        execution.logical.reserve(s.cpus.size());
        for(std::size_t i=0;i<s.cpus.size();++i) {
            const auto ticks=account.ticks[i];
            if(ticks>elapsed){s.fail("runtime-overflow");break;}
            const auto percent=100.0*static_cast<double>(ticks)/static_cast<double>(elapsed);
            execution.logical.push_back({s.topology[i].first,s.topology[i].second,nanoseconds(ticks,s.frequency),
                window.complete[i]?std::optional<double>(percent):std::nullopt,percent});
        }
        window.processes.push_back({identity,std::move(execution)});
    }
    if(!s.failure.empty()){window.status=s.failure;window.processes.clear();std::fill(window.complete.begin(),window.complete.end(),false);}
    s.begin=at;s.switches=0;
    for(auto it=s.accounts.begin();it!=s.accounts.end();) {
        const auto process=s.processes.find(it->first.pid);
        if(process==s.processes.end() || process->second.identity!=it->first)it=s.accounts.erase(it);
        else {std::fill(it->second.ticks.begin(),it->second.ticks.end(),0);++it;}
    }
    return window;
}
CpuProcessExecution CpuExecutionWindow::for_process(CpuExecutionIdentity identity)const {
    for(const auto& process:processes)if(process.identity==identity)return process.execution;
    CpuProcessExecution result;result.status=status;result.begin_qpc=begin_qpc;result.end_qpc=end_qpc;result.frequency=frequency;
    const bool known=std::find(known_processes.begin(),known_processes.end(),identity)!=known_processes.end();
    if(!known)result.status="identity-unavailable";
    const bool usable=known && (status=="ready" || status=="partial");
    for(std::size_t i=0;i<topology.size();++i)result.logical.push_back({topology[i].first,topology[i].second,0,
        usable && i<complete.size() && complete[i]?std::optional<double>(0):std::nullopt});
    return result;
}
void merge_cpu_execution(CpuProcessExecution& target,const CpuProcessExecution& part,bool first) {
    if(first){target=part;return;}
    if(target.logical.empty() && !part.logical.empty()) {
        target=part;target.status="partial";for(auto& cpu:target.logical)cpu.percent.reset();return;
    }
    if(target.begin_qpc!=part.begin_qpc || target.end_qpc!=part.end_qpc || target.frequency!=part.frequency || target.logical.size()!=part.logical.size()) {
        target.status="partial";for(auto& cpu:target.logical)cpu.percent.reset();return;
    }
    if(part.status!="ready" || target.status!="ready")target.status="partial";
    for(std::size_t i=0;i<target.logical.size();++i) {
        auto& a=target.logical[i];const auto& b=part.logical[i];
        if(a.group!=b.group || a.number!=b.number){target.status="partial";a.percent.reset();continue;}
        a.runtime_ns+=b.runtime_ns;
        a.observed_percent+=b.observed_percent;
        if(a.percent && b.percent && *a.percent+*b.percent<=100.000001)a.percent=std::min(100.0,*a.percent+*b.percent);
        else a.percent.reset();
        if(a.observed_percent>100.000001){target.status="partial";a.runtime_ns=0;a.observed_percent=0;a.percent.reset();}
    }
}
}
