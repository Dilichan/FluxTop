#include "fluxtop/cpu_execution.hpp"
#include "fluxtop/model.hpp"
#include "fluxtop/output.hpp"
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <array>
#include <cstring>
#include <sstream>
using namespace fluxtop;
namespace {
void need(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void near(std::optional<double> actual,double expected,const char* message){need(actual && std::abs(*actual-expected)<0.000001,message);}
void idle(CpuExecutionTracker& tracker,unsigned count=1) {
    for(unsigned i=0;i<count;++i)tracker.context_switch(i,0,0,100);
    need(tracker.finish(100).status=="baseline","initial window must be baseline");
}
void owner(CpuExecutionTracker& t,unsigned pid=10,std::uint64_t creation=1000,unsigned tid=100,std::uint64_t at=100) {
    t.process_start(pid,creation,at);t.thread_start(tid,pid,at);
}
void boundaries() {
    std::array<std::uint8_t,32> payload{};std::uint32_t next=123,old=456;
    std::memcpy(payload.data(),&next,4);std::memcpy(payload.data()+4,&old,4);
    for(unsigned version=2;version<=5;++version) {
        const auto decoded=cpu_switch_threads(version,payload);
        need(decoded && decoded->first==123 && decoded->second==456,"CSwitch V2/V3/V4/V5 prefix");
    }
    need(!cpu_switch_threads(4,std::span(payload).first(23)) && !cpu_switch_threads(5,std::span(payload).first(27))
        && !cpu_switch_threads(6,payload),"truncated or unknown CSwitch never guessed");
    CpuExecutionTracker t({{0,0}},1000);idle(t);owner(t);
    t.context_switch(0,0,100,120);t.context_switch(0,100,0,180);
    auto window=t.finish(200);need(window.status=="ready","complete trace");
    auto execution=window.for_process({10,1000});near(execution.logical[0].percent,60,"closed interval percent");
    need(execution.logical[0].runtime_ns==60000000,"QPC runtime to ns");
    t.context_switch(0,0,100,220);
    execution=t.finish(250).for_process({10,1000});near(execution.logical[0].percent,60,"boundary clips active tail");
    need(execution.logical[0].runtime_ns==30000000,"first clipped tail");
    execution=t.finish(300).for_process({10,1000});near(execution.logical[0].percent,100,"continuation across boundary");
    need(execution.logical[0].runtime_ns==50000000,"tail must not count twice");
    need(!t.finish(300).for_process({10,1000}).logical[0].percent,"repeated boundary is stale");
}
void groups_and_migration() {
    CpuExecutionTracker t({{0,63},{1,0}},1000);idle(t,2);owner(t);
    t.context_switch(0,0,100,110);t.context_switch(0,100,0,150);
    t.context_switch(1,0,100,150);t.context_switch(1,100,0,190);
    const auto e=t.finish(200).for_process({10,1000});
    need(e.logical[0].group==0 && e.logical[0].number==63 && e.logical[1].group==1 && e.logical[1].number==0,"processor groups are distinct");
    near(e.logical[0].percent,40,"migration source");near(e.logical[1].percent,40,"migration destination");
    need(e.logical[0].runtime_ns+e.logical[1].runtime_ns==80000000,"migration time counted once");
}
void lifetime() {
    CpuExecutionTracker t({{0,0}},1000);idle(t);owner(t,10,1);
    t.context_switch(0,0,100,110);t.context_switch(0,100,0,150);
    t.thread_end(100,10,151);t.process_end(10,155);
    owner(t,10,2,100,160);t.context_switch(0,0,100,190);
    const auto w=t.finish(200);
    near(w.for_process({10,1}).logical[0].percent,40,"retired PID owns only its interval");
    near(w.for_process({10,2}).logical[0].percent,10,"reused PID/TID cannot inherit old CPU time");
    need(w.for_process({10,999}).status=="identity-unavailable" && !w.for_process({10,999}).logical[0].percent,"unverified generation never gets zero");
    t.context_switch(0,100,0,210);t.thread_end(100,10,211);
    for(unsigned i=0;i<1000;++i){t.thread_start(200+i,10,220+i);t.thread_end(200+i,10,221+i);}
    need(t.thread_count()==0,"terminated threads must not accumulate");
    // Delayed lifecycle records cannot retire the reused identity.
    t.thread_start(100,10,1300);t.thread_end(100,10,1250);
    need(t.thread_count()==1,"late End must not retire a newer TID");
}
void uncertainty() {
    CpuExecutionTracker t({{0,0},{0,1}},1000);idle(t,2);owner(t);
    t.context_switch(0,0,999,120);
    const auto w=t.finish(200);need(w.status=="partial" && w.unknown_ticks==80,"unknown thread interval explicit");
    const auto e=w.for_process({10,1000});need(!e.logical[0].percent,"partial CPU is not reliable zero");
    near(e.logical[1].percent,0,"independently fully covered idle CPU");
    CpuExecutionTracker missing({{0,0},{1,0}},1000);missing.context_switch(0,0,0,100);missing.finish(100);owner(missing);
    const auto partial=missing.finish(200);need(partial.status=="partial" && !partial.for_process({10,1000}).logical[1].percent,"silent logical processor remains unknown");
    t.invalidate("incomplete");const auto lost=t.finish(300);
    need(lost.status=="incomplete" && lost.processes.empty() && !lost.for_process({10,1000}).logical[1].percent,"loss invalidates even observed subset");
    need(t.finish(400).status=="incomplete","partial trace cannot silently recover as exact");
    CpuExecutionTracker restart({{0,0}},1000);idle(restart);owner(restart);
    near(restart.finish(200).for_process({10,1000}).logical[0].percent,0,"new session starts clean after loss");
}
void bad_order_and_limits() {
    CpuExecutionTracker order({{0,0}},1000);idle(order);owner(order);
    order.context_switch(0,0,100,150);order.context_switch(0,100,0,140);
    need(order.finish(200).status=="out-of-order","negative interval rejected");
    CpuExecutionTracker overlap({{0,0},{0,1}},1000);idle(overlap,2);owner(overlap);
    overlap.context_switch(0,0,100,110);overlap.context_switch(1,0,100,120);
    const auto bad=overlap.finish(200);need(bad.status=="overlapping-thread-execution" && bad.processes.empty(),"one TID cannot execute on two CPUs at once");
    CpuExecutionTracker gap({{0,0}},1000);idle(gap);owner(gap);
    gap.context_switch(0,100,0,150);need(gap.finish(200).status=="switch-discontinuity","missing switch invalidates exact trace");
    CpuExecutionTracker limited({{0,0}},1000,{1,1,1});idle(limited);owner(limited);
    limited.thread_start(101,10,110);need(limited.thread_count()==1 && limited.finish(200).status=="state-capacity-exceeded","thread state bounded");
    CpuExecutionTracker cells({{0,0},{0,1}},1000,{10,10,1});idle(cells,2);owner(cells);
    cells.context_switch(0,0,100,110);need(cells.finish(200).status=="state-capacity-exceeded","account cells bounded");
    CpuExecutionTracker stale({{0,0}},1000);idle(stale);need(stale.finish(32000).status=="stale","stale window never becomes a reliable percentage");
}
void applications_and_invariants() {
    CpuExecutionTracker t({{0,0}},1000);idle(t);owner(t,10,1,100);owner(t,11,2,101);owner(t,12,3,102);
    t.context_switch(0,0,100,110);t.context_switch(0,100,101,140);t.context_switch(0,101,102,180);t.context_switch(0,102,0,190);
    const auto w=t.finish(200);
    std::vector<Process> rows(3);
    for(unsigned i=0;i<3;++i) {
        auto& p=rows[i];p.pid=10+i;p.creation_time=1+i;p.name="same.exe";
        p.exe_path=i<2?"c:/one/same.exe":"c:/two/same.exe";p.cpu_percent=5;
        p.cpu_execution=w.for_process({p.pid,*p.creation_time});
    }
    const auto grouped=aggregate_application_processes(rows);
    need(grouped.size()==2,"same filename in different EXE paths never merges");
    near(grouped[0].cpu_execution.logical[0].percent,70,"same-path workers aggregate once");
    near(grouped[1].cpu_execution.logical[0].percent,10,"other path remains separate");
    near(grouped[0].cpu_percent,10,"existing total CPU aggregation unchanged");
    auto missing=rows;missing[0].cpu_execution={};
    const auto subset=aggregate_application_processes(missing);
    need(subset[0].cpu_execution.status=="partial" && subset[0].cpu_execution.logical[0].runtime_ns==40000000
        && !subset[0].cpu_execution.logical[0].percent,"unknown first member preserves verified sibling as a lower bound");
    Snapshot snapshot;snapshot.cpu_execution_status="partial";snapshot.processes={rows[0]};
    auto& execution=snapshot.processes[0].cpu_execution;
    execution.status="partial";execution.logical[0].percent.reset();
    std::ostringstream json;write_jsonl(json,snapshot,0);
    need(json.str().find("\"observed_runtime_ns\":30000000,\"percent\":null,\"observed_percent\":30.000")!=std::string::npos,
        "JSON exposes partial time as lower bound, never reliable zero");
    SpikeTracker a,b;auto no_etw=rows;
    for(auto& p:no_etw)p.cpu_execution={};
    a.update(rows);b.update(no_etw);
    rows[0].cpu_percent=20;no_etw[0].cpu_percent=20;
    a.update(rows);b.update(no_etw);
    need(rows[0].auto_scores==no_etw[0].auto_scores && rows[0].burst_scores==no_etw[0].burst_scores,"ETW CPU never changes AutoScore");
}
}
int main(){try{boundaries();groups_and_migration();lifetime();uncertainty();bad_order_and_limits();applications_and_invariants();
    std::cout<<"CPU ETW timing, identity, groups, migration, quality, bounds and aggregation passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
