#include "fluxtop/history.hpp"
#include "fluxtop/statistics_history.hpp"
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool ok,const char* why) {if(!ok) throw std::runtime_error(why);}
}
int main(int argc,char** argv) {
    try {
        require(argc==2,"fixture directory is required");
        const auto directory=std::filesystem::path(argv[1])/
            ("stats-test-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(directory);
        const auto db=directory/L"statistics.db";
        fluxtop::Process app,child,system;
        app.pid=3301;app.creation_time=10;app.name="UserApp.exe";app.exe_path="C:\\Apps\\UserApp.exe";
        app.cpu_percent=20;app.working_set=1'048'576;
        child=app;child.pid=3302;child.creation_time=11;child.parent_pid=3301;
        child.cpu_percent=10;
        system.pid=3801;system.creation_time=20;system.name="svchost.exe";
        system.exe_path="C:\\Windows\\System32\\svchost.exe";system.cpu_percent=70;
        fluxtop::Snapshot sample;
        sample.timestamp_ms=1'900'000'000'000LL;sample.interval_ms=1000;
        sample.total_ram_capacity=16ULL*1024*1024*1024;
        sample.total_gpu_memory_capacity=8ULL*1024*1024*1024;
        sample.processes={app,child,system};
        {
            fluxtop::HistoryWriter writer(db);
            for(int n=0;n<3;++n) {
                sample.timestamp_ms+=1000;
                writer.append(sample);
                require(sample.history_error.empty(),"history writer rejected scored sample");
            }
        }
        const auto range=fluxtop::read_statistics(db,1'900'000'000'000LL,
            1'900'000'010'000LL,10,false,{});
        require(range.first_record_ms==1'900'000'001'000LL,"incorrect first historical sample");
        require(range.scores_available,"AutoScore table missing");
        require(range.uptime.size()==1,"system filter or app deduplication failed");
        require(range.uptime[0].observed_seconds>=2.9 && range.uptime[0].observed_seconds<=3.1,
            "multi-process uptime counted more than once per sample");
        require(range.history.size()==1 && range.history[0].points.size()==3,
            "historical AutoScore chart lacks persistent samples");
        for(const auto& [time,score]:range.history[0].points)
            require(time>0 && score>=0 && score<=100,"AutoScore is out of bounds");
        const auto all=fluxtop::read_statistics(db,1'900'000'000'000LL,
            1'900'000'010'000LL,10,true,{});
        require(all.uptime.size()==2,"including system applications failed");
        std::cout<<"Persistent AutoScore, uptime dedup, history filtering and optional system rows passed\n";
        return 0;
    } catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}