#include "fluxtop/intel_dts.hpp"
#include "fluxtop/windows.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
using namespace std::chrono_literals;
namespace {
void need(bool yes,const char* text){if(!yes)throw std::runtime_error(text);}
}
int wmain(int argc,wchar_t** argv){try{
    need(argc>=2,"fixture root required");
    if(std::wstring_view(argv[1])==L"--child"){
        need(argc==3,"child marker required");
        (void)fluxtop::read_intel_dts();
        std::this_thread::sleep_for(2500ms); // Allow the asynchronous first hardware poll.
        bool missing=false;
        for(int i=0;i<150;++i){
            const auto s=fluxtop::read_intel_dts();
            need(s.physical_cores>0 && !s.cores.empty(),"topology survives unavailable sensor");
            if(!missing && s.status!="initializing"){
                if(s.status!="module-unavailable"){
                    std::cout<<"Hardware prerequisite unavailable: "<<s.status<<'\n';return 77;
                }
                need(!s.package_c && !s.core_max_c && !s.core_average_c,"missing module must not produce temperatures");
                for(const auto& core:s.cores)need(!core.celsius,"missing sensor is unknown, not zero");
                std::ofstream(argv[2])<<"missing-module";missing=true;
            }
            if(missing && s.driver_ready && (s.status=="ready" || s.status=="partial") && s.available_cores>0){
                need(s.sampled_tick>0 && s.package_c.value_or(1)>0,"restored sensor is fresh and nonzero");
                std::cout<<"Private sensor module recovered without restarting collector\n";return 0;
            }
            std::this_thread::sleep_for(200ms);
        }throw std::runtime_error("sensor did not recover after restoring its private module");
    }
    wchar_t exe[32768]{};need(GetModuleFileNameW(nullptr,exe,32768)>0,"own executable");
    const auto source=std::filesystem::path(exe).parent_path()/L"IntelMSR.bin";
    if(!std::filesystem::exists(source)){std::cout<<"Bundled IntelMSR module unavailable\n";return 77;}
    const auto root=std::filesystem::path(argv[1])/(L"sensor-recovery-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
    std::filesystem::create_directories(root);const auto copy=root/L"sensor.exe",marker=root/L"ready";
    std::filesystem::copy_file(exe,copy);
    auto command=L"\""+copy.wstring()+L"\" --child \""+marker.wstring()+L"\"";
    STARTUPINFOW start{};start.cb=sizeof(start);PROCESS_INFORMATION child{};
    need(CreateProcessW(copy.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&start,&child)!=FALSE,"start isolated sensor process");
    struct Cleanup{PROCESS_INFORMATION& p;~Cleanup(){if(WaitForSingleObject(p.hProcess,0)==WAIT_TIMEOUT){TerminateProcess(p.hProcess,54);WaitForSingleObject(p.hProcess,5000);}CloseHandle(p.hProcess);CloseHandle(p.hThread);}}cleanup{child};
    for(int i=0;i<75 && !std::filesystem::exists(marker) && WaitForSingleObject(child.hProcess,0)==WAIT_TIMEOUT;++i)std::this_thread::sleep_for(200ms);
    if(std::filesystem::exists(marker))std::filesystem::copy_file(source,root/L"IntelMSR.bin");
    need(WaitForSingleObject(child.hProcess,20000)==WAIT_OBJECT_0,"sensor fixture completes");DWORD code{};need(GetExitCodeProcess(child.hProcess,&code)!=FALSE,"sensor result");
    if(code==77){std::cout<<"PawnIO/Intel hardware prerequisite unavailable; no driver changes made\n";return 77;}
    need(code==0,"private missing-module/recovery fixture failed");
    std::filesystem::remove_all(root);std::cout<<"Missing module, intact topology and live DTS recovery passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
