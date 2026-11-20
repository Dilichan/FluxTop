// Validate real collector JSONL output without a PowerShell JSON parser.
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <regex>
#include <stdexcept>
#include <string>
#include <vector>
#include <sstream>

namespace {
void require(bool condition,const char* message) {if(!condition)throw std::runtime_error(message);}
std::string capture(const std::filesystem::path& program,const std::filesystem::path& fixture) {
    HANDLE reader{},writer{};
    SECURITY_ATTRIBUTES sa{sizeof(sa),nullptr,TRUE};
    require(CreatePipe(&reader,&writer,&sa,0)!=0,"Unable to make capture pipe");
    if(!SetHandleInformation(reader,HANDLE_FLAG_INHERIT,0)){CloseHandle(reader);CloseHandle(writer);throw std::runtime_error("Pipe protection failed");}
    std::wstring command=L"\""+program.wstring()+L"\" --samples 2 --interval-ms 250 --no-gpu --jsonl";
    STARTUPINFOW start{};start.cb=sizeof(start);start.dwFlags=STARTF_USESTDHANDLES;
    start.hStdOutput=writer;start.hStdError=GetStdHandle(STD_ERROR_HANDLE);
    start.hStdInput=GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION proc{};
    const auto created=CreateProcessW(program.c_str(),command.data(),nullptr,nullptr,TRUE,
        CREATE_NO_WINDOW,nullptr,fixture.c_str(),&start,&proc);
    CloseHandle(writer);
    if(!created){CloseHandle(reader);throw std::runtime_error("Unable to start collector: "+std::to_string(GetLastError()));}
    std::string output;char buffer[4096];DWORD count{};
    while(ReadFile(reader,buffer,static_cast<DWORD>(sizeof(buffer)),&count,nullptr)&&count>0) {
        output.append(buffer,count);
        if(output.size()>1024*1024){TerminateProcess(proc.hProcess,1);break;}
    }
    CloseHandle(reader);
    const auto waited=WaitForSingleObject(proc.hProcess,30000);
    if(waited!=WAIT_OBJECT_0)TerminateProcess(proc.hProcess,1);
    DWORD status{};GetExitCodeProcess(proc.hProcess,&status);
    CloseHandle(proc.hThread);CloseHandle(proc.hProcess);
    require(waited==WAIT_OBJECT_0 && status==0,"Collector failed or timed out");
    return output;
}
}
int wmain(int argc,wchar_t** argv) {
    if(argc!=3)return 2;
    try {
        const std::filesystem::path program=argv[1],build=argv[2];
        const auto db=build/L"ci-fluxtop-native.db";
        const auto log=build/L"ci-activity-native.log";
        require(SetEnvironmentVariableW(L"FLUXTOP_DATA_DB",db.c_str())!=0,"Cannot set DB path");
        require(SetEnvironmentVariableW(L"FLUXTOP_LOG_PATH",log.c_str())!=0,"Cannot set log path");
        const std::string result=capture(program,build);
        std::istringstream stream(result);
        std::string line;unsigned lines=0;
        while(std::getline(stream,line)) {
            // Windows text stdout uses CRLF. getline removes LF but retains CR.
            if(!line.empty() && line.back()=='\r')line.pop_back();
            if(line.empty())continue;
            ++lines;
            require(line.front()=='{' && line.back()=='}',"Invalid JSONL object framing");
            require(std::regex_search(line,std::regex(R"("schema_version"\s*:\s*1\b)")),"Wrong JSON schema");
            require(std::regex_search(line,std::regex(R"("gpu_status"\s*:\s*"disabled")")),"GPU was not disabled");
            const std::regex count(R"("process_count"\s*:\s*([0-9]+))");
            std::smatch match;
            require(std::regex_search(line,match,count) && std::stoull(match[1])>0,"No process count");
            const std::regex working(R"("working_set_bytes"\s*:\s*([0-9]+))");
            require(std::regex_search(line,match,working) && std::stoull(match[1])>0,"No resident memory");
            require(!std::regex_search(line,std::regex(R"("history_error"\s*:\s*"[^"\r\n]+")")),"History writer reported error");
        }
        require(lines==2,"Collector must output exactly two JSONL samples");
        require(std::filesystem::is_regular_file(db)&&std::filesystem::file_size(db)>0,
            "Collector did not persist history database at shutdown");
        std::cout<<"Native JSONL contract passed\n";
        return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}