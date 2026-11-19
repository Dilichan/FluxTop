#include "fluxtop/history.hpp"
#include "fluxtop/history_math.hpp"
#include "fluxtop/statistics_history.hpp"
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
}
int main(int argc,char** argv){
    try {
        require(argc==2,"fixture directory required");
        const auto root=std::filesystem::path(argv[1])/
            ("stats-v10-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(root);
        const auto path=root/L"statistics.db";
        const auto base=fluxtop::history_floor(1'900'000'000'000LL,fluxtop::history_day_ms);
        fluxtop::Process game,child,system;
        game.pid=1001;game.creation_time=101;game.name="UserGame.exe";game.exe_path="C:\\Games\\UserGame.exe";
        game.cpu_percent=12.0;game.working_set=12'000'000;
        child=game;child.pid=1002;child.creation_time=102;child.parent_pid=1001;
        system.pid=2001;system.creation_time=201;system.name="svchost.exe";
        system.exe_path="C:\\Windows\\System32\\svchost.exe";system.cpu_percent=65.0;
        {
            fluxtop::HistoryWriter writer(path);
            for(int i=0;i<4;++i){
                fluxtop::Snapshot sample;
                sample.timestamp_ms=base+10*fluxtop::history_hour_ms+i*1000;
                sample.interval_ms=1000;
                sample.total_ram_capacity=16ULL*1024*1024*1024;
                sample.total_gpu_memory_capacity=8ULL*1024*1024*1024;
                sample.processes={game,child,system};
                writer.append(sample);
                require(sample.history_error.empty(),"writer error");
            }
            fluxtop::Snapshot future;
            future.timestamp_ms=base+3*fluxtop::history_day_ms+1'000;
            future.interval_ms=1000;
            future.processes={game,child,system};
            writer.append(future);
        }
        auto history=fluxtop::read_statistics(path,base,base+4*fluxtop::history_day_ms,10,false,{});
        require(history.error.empty(),"reader returned error");
        require(history.scores_available,"historical scores available");
        require(history.uptime.size()==1,"system filter or app dedup failed");
        require(std::abs(history.uptime[0].observed_seconds-5.0)<0.0001,"one uptime per application, not per PID");
        require(history.history.size()==1 && !history.history[0].points.empty(),"historical graph survives compaction");
        for(const auto& [at,score]:history.history[0].points)
            require(at>=base && score>=0 && score<=100,"historical point range");
        auto all=fluxtop::read_statistics(path,base,base+4*fluxtop::history_day_ms,10,true,{});
        require(all.error.empty() && all.uptime.size()==2,"system filter toggle");
        auto separated=fluxtop::read_statistics(path,base,base+4*fluxtop::history_day_ms,
            1,2,1,true,{});
        require(separated.error.empty() && separated.uptime.size()==2 &&
            separated.history.size()==1,"Independent history/uptime SQL limits");
        std::filesystem::remove_all(root);
        std::cout<<"Compacted AutoScore graph, uptime, multi-process dedup, system toggle passed\n";
        return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
