#include "fluxtop/process_collector.hpp"
#include <psapi.h>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <thread>

int main() {
    try {
        fluxtop::ProcessCollector collector;
        const auto first=collector.collect();
        std::this_thread::sleep_for(std::chrono::milliseconds(140));
        const auto second=collector.collect();
        if(first.processes.empty() || second.processes.empty()) return 1;
        const auto pid=GetCurrentProcessId();
        const fluxtop::RawProcess* self=nullptr;
        for(const auto& p:second.processes) if(p.pid==pid) {self=&p;break;}
        if(!self || !self->creation_time || !self->working_set || !self->private_bytes) return 2;
        PROCESS_MEMORY_COUNTERS_EX memory{};memory.cb=sizeof(memory);
        if(!GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),sizeof(memory))) return 3;
        auto close=[](std::uint64_t a,std::uint64_t b) {
            const auto diff=a>b?a-b:b-a;
            return diff<=std::max<std::uint64_t>(16ULL*1024*1024,b/4);
        };
        if(!close(*self->working_set,memory.WorkingSetSize) || !close(*self->private_bytes,memory.PrivateUsage)) return 4;
        std::cout << "Process snapshot self-PID/creation/RAM baseline passed\n";
        return 0;
    } catch(...) {return 5;}
}
