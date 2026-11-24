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
#include <algorithm>

namespace {
void require(bool condition,const char* message) {if(!condition)throw std::runtime_error(message);}
std::string capture(const std::filesystem::path& program,const std::filesystem::path& fixture) {
    HANDLE reader{},writer{};
    SECURITY_ATTRIBUTES sa{sizeof(sa),nullptr,TRUE};
    require(CreatePipe(&reader,&writer,&sa,0)!=0,"Unable to make capture pipe");
    if(!SetHandleInformation(reader,HANDLE_FLAG_INHERIT,0)){CloseHandle(reader);CloseHandle(writer);throw std::runtime_error("Pipe protection failed");}
    std::wstring command=L"\""+program.wstring()+L"\" --samples 2 --interval-ms 250 --no-gpu --jsonl";
    // Keep the child's real error text even when CTest does not provide a
    // usable inherited console handle. Do not merge stderr into JSONL stdout.
    const auto error_path=fixture/L"ci-jsonl-child-stderr.log";
    HANDLE error_stream=CreateFileW(error_path.c_str(),GENERIC_WRITE,FILE_SHARE_READ,
        &sa,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(error_stream==INVALID_HANDLE_VALUE) {
        const DWORD code=GetLastError();
        CloseHandle(reader);CloseHandle(writer);
        throw std::runtime_error("Unable to create child stderr: "+std::to_string(code));
    }
    STARTUPINFOW start{};start.cb=sizeof(start);start.dwFlags=STARTF_USESTDHANDLES;
    start.hStdOutput=writer;start.hStdError=error_stream;
    start.hStdInput=GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION proc{};
    const auto created=CreateProcessW(program.c_str(),command.data(),nullptr,nullptr,TRUE,
        CREATE_NO_WINDOW,nullptr,fixture.c_str(),&start,&proc);
    const DWORD launch_error=created?0:GetLastError();
    CloseHandle(writer);
    CloseHandle(error_stream);
    if(!created){CloseHandle(reader);throw std::runtime_error("Unable to start collector: "+std::to_string(launch_error));}
    std::string output;char buffer[4096];DWORD count{};
    const auto deadline=GetTickCount64()+30000ULL;
    DWORD waited=WAIT_TIMEOUT;
    bool overflow=false,pipe_error=false,extension_points_disabled=false;
    // Poll pipe availability: blocking ReadFile must not bypass the child deadline.
    while(GetTickCount64()<deadline) {
        DWORD available=0;
        if(!PeekNamedPipe(reader,nullptr,0,nullptr,&available,nullptr)) {
            const auto err=GetLastError();
            if(err!=ERROR_BROKEN_PIPE)pipe_error=true;
            else {
                const auto now=GetTickCount64();
                waited=WaitForSingleObject(proc.hProcess,
                    now<deadline?static_cast<DWORD>(deadline-now):0);
            }
            break;
        }
        if(available) {
            // The first real JSONL bytes prove the production entry point ran.
            if(output.empty()) {
                PROCESS_MITIGATION_EXTENSION_POINT_DISABLE_POLICY policy{};
                extension_points_disabled=GetProcessMitigationPolicy(proc.hProcess,
                    ProcessExtensionPointDisablePolicy,&policy,sizeof(policy)) && policy.DisableExtensionPoints;
            }
            const DWORD to_read=std::min<DWORD>(available,static_cast<DWORD>(sizeof(buffer)));
            if(!ReadFile(reader,buffer,to_read,&count,nullptr)) {pipe_error=true;break;}
            output.append(buffer,count);
            if(output.size()>1024*1024) {overflow=true;break;}
            continue;
        }
        waited=WaitForSingleObject(proc.hProcess,10);
        if(waited==WAIT_FAILED) {pipe_error=true;break;}
        // Drain any bytes remaining in the pipe after process exit.
        if(waited==WAIT_OBJECT_0) {
            DWORD remaining=0;
            if(!PeekNamedPipe(reader,nullptr,0,nullptr,&remaining,nullptr)||remaining==0)break;
        }
    }
    if(waited!=WAIT_OBJECT_0 || overflow || pipe_error) {
        TerminateProcess(proc.hProcess,1);
        WaitForSingleObject(proc.hProcess,5000);
    }
    CloseHandle(reader);
    DWORD status{};GetExitCodeProcess(proc.hProcess,&status);
    CloseHandle(proc.hThread);CloseHandle(proc.hProcess);
    if(waited!=WAIT_OBJECT_0 || status!=0 || overflow || pipe_error) {
        std::ostringstream problem;
        problem<<"Collector failed: timeout_or_wait="<<waited<<" overflow="<<overflow
            <<" pipe_error="<<pipe_error<<" exit_code=0x"<<std::hex
            <<status<<std::dec<<" ("<<status<<")";
        std::ifstream error(error_path,std::ios::binary);
        if(error) {
            char excerpt[4096]{};
            error.read(excerpt,static_cast<std::streamsize>(sizeof(excerpt)));
            const auto bytes=error.gcount();
            if(bytes>0)problem<<" stderr="<<std::string(excerpt,static_cast<std::size_t>(bytes));
        }
        if(!output.empty())problem<<" stdout_prefix="<<output.substr(0,512);
        throw std::runtime_error(problem.str());
    }
    require(extension_points_disabled,"Monitor must disable legacy in-process extension points at startup");
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
        const std::string restarted=capture(program,build);
        require(restarted.find("\"schema_version\":1")!=std::string::npos,
            "JSONL schema changed after collector restart");
        require(restarted.find("\"disk_measurement_quality\":")!=std::string::npos,
            "Extended disk diagnostics missing after restart");
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
