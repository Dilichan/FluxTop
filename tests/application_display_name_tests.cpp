#include "fluxtop/application_display_name.hpp"
#include <iostream>
#include <stdexcept>
#include <vector>

int main() {
    try {
        using namespace fluxtop;
        Process normal{};
        normal.pid=56736;
        normal.name="chrome.exe";
        normal.exe_path="c:\\program files\\google\\chrome\\application\\chrome.exe";
        normal.dedicated_gpu_bytes=0;
        Process portable{};
        portable.pid=57548;
        portable.name="chrome.exe";
        portable.exe_path="c:\\users\\dityan\\downloads\\chrome-win\\chrome-win\\chrome.exe";
        portable.dedicated_gpu_bytes=11513364;
        std::vector<Process> both{normal,portable};
        if(application_display_name(both[0],both)!="chrome (google)")
            throw std::runtime_error("standard Chrome group label differs");
        if(application_display_name(both[1],both)!="chrome (chrome-win)")
            throw std::runtime_error("portable Chrome group label differs");
        if(application_display_name(both[0],{both[0]})!="chrome")
            throw std::runtime_error("unique group name must remain unchanged");
        if(both[0].dedicated_gpu_bytes!=0 || both[1].dedicated_gpu_bytes!=11513364)
            throw std::runtime_error("presentation changed process metrics");
        Process unknown{};
        unknown.pid=123;
        unknown.name="chrome.exe";
        std::vector<Process> mixed{normal,unknown};
        if(application_display_name(mixed[1],mixed)!="chrome (PID 123)")
            throw std::runtime_error("missing EXE identity must remain distinct");
        std::cout<<"Application display name contracts passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr<<e.what()<<'\n';return 1;
    }
}