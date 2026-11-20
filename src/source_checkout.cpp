#include "fluxtop/source_checkout.hpp"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <regex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace fluxtop::source_checkout {
namespace {
struct Handle {
    HANDLE h{INVALID_HANDLE_VALUE};
    explicit Handle(HANDLE value=INVALID_HANDLE_VALUE):h(value){}
    Handle(const Handle&)=delete;Handle& operator=(const Handle&)=delete;
    ~Handle(){if(h&&h!=INVALID_HANDLE_VALUE)CloseHandle(h);}
    bool valid()const{return h&&h!=INVALID_HANDLE_VALUE;}
};
std::wstring wide(std::string_view value){
    if(value.empty())return {};
    const int n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.data(),
        static_cast<int>(value.size()),nullptr,0);
    if(n<=0)throw std::runtime_error("Invalid UTF-8 in Git output");
    std::wstring out(static_cast<size_t>(n),L'\0');
    MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),out.data(),n);
    return out;
}
std::string trim(std::string value){
    const auto start=value.find_first_not_of(" \t\r\n");
    if(start==std::string::npos)return {};
    const auto end=value.find_last_not_of(" \t\r\n");
    return value.substr(start,end-start+1);
}
std::wstring environment(const wchar_t* key){
    const DWORD n=GetEnvironmentVariableW(key,nullptr,0);
    if(!n||n>32767)return {};
    std::wstring out(n,L'\0');
    const DWORD written=GetEnvironmentVariableW(key,out.data(),n);
    if(!written||written>=n)return {};
    out.resize(written);return out;
}
fs::path search(const wchar_t* filename){
    const DWORD len=SearchPathW(nullptr,filename,nullptr,0,nullptr,nullptr);
    if(!len||len>32767)return {};
    std::wstring path(len,L'\0');
    const DWORD written=SearchPathW(nullptr,filename,nullptr,len,path.data(),nullptr);
    if(!written||written>=len)return {};
    path.resize(written);return path;
}
fs::path cmake_tool(){
    auto found=search(L"cmake.exe");if(!found.empty())return found;
    const auto pf=environment(L"ProgramFiles(x86)");
    if(pf.empty())throw std::runtime_error("CMake unavailable: install CMake or Visual Studio Build Tools");
    const auto vswhere=fs::path(pf)/L"Microsoft Visual Studio"/L"Installer"/L"vswhere.exe";
    if(fs::exists(vswhere)){
        const auto res=process(vswhere,{L"-latest",L"-products",L"*",L"-requires",
            L"Microsoft.VisualStudio.Component.VC.Tools.x86.x64",L"-property",L"installationPath"},
            vswhere.parent_path(),30);
        if(res.exit_code==0){
            const auto install=fs::path(wide(trim(res.output)));
            found=install/L"Common7"/L"IDE"/L"CommonExtensions"/L"Microsoft"/L"CMake"/L"CMake"/L"bin"/L"cmake.exe";
            if(fs::exists(found))return found;
        }
    }
    throw std::runtime_error("CMake not found in PATH or Visual Studio");
}
void append_log(const fs::path& state,std::string_view heading,std::string_view content){
    std::ofstream log(state/L"build.log",std::ios::binary|std::ios::app);
    log<<"\n== "<<heading<<" ==\n"<<content<<"\n";
}
std::vector<std::wstring> git_args(const fs::path& repo,const std::vector<std::wstring>& more){
    std::vector<std::wstring> a={L"-c",L"safe.directory="+repo.wstring(),L"-c",L"core.hooksPath=NUL",
        L"-c",L"merge.autoStash=false",L"-c",L"core.fsmonitor=false",L"-c",L"maintenance.auto=false",
        L"-c",L"gc.auto=0",L"-c",L"credential.interactive=false",L"-c",L"http.lowSpeedLimit=1",
        L"-c",L"http.lowSpeedTime=10"};
    a.insert(a.end(),more.begin(),more.end());return a;
}
Result git(const fs::path& repo,const std::vector<std::wstring>& args){
    // The checkout bootstrap must never open an interactive credential prompt.
    SetEnvironmentVariableW(L"GIT_TERMINAL_PROMPT",L"0");
    SetEnvironmentVariableW(L"GCM_INTERACTIVE",L"Never");
    SetEnvironmentVariableW(L"GIT_SSH_COMMAND",
        L"ssh -oBatchMode=yes -oConnectTimeout=10 -oStrictHostKeyChecking=yes");
    return process(L"git.exe",git_args(repo,args),repo,45);
}
std::string git_value(const fs::path& repo,const std::vector<std::wstring>& args){
    const auto result=git(repo,args);
    if(result.timed_out||result.exit_code!=0)throw std::runtime_error("Git command failed: "+trim(result.output));
    return trim(result.output);
}
struct State{std::string commit;bool dirty{true};};
State checkout(const fs::path& repo){
    const auto head=git(repo,{L"rev-parse",L"--verify",L"HEAD"});
    const auto status=git(repo,{L"status",L"--porcelain",L"--untracked-files=normal"});
    if(status.timed_out||status.exit_code!=0)throw std::runtime_error("Git status failed");
    return {head.exit_code==0?trim(head.output):"",!status.output.empty()};
}
std::string nonce(){return std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64());}
bool safe_version(std::string_view value){
    if(value.empty()||value.size()>90)return false;
    for(unsigned char c:value)if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'))return false;
    return true;
}
void publish_manifest(const fs::path& state,const std::string& version){
    const auto manifest=state/L"last-good.json";
    const auto temp=state/(L"last-good."+std::to_wstring(GetCurrentProcessId())+L".tmp");
    {std::ofstream out(temp,std::ios::binary|std::ios::trunc);
     if(!out)throw std::runtime_error("Cannot create last-good temporary manifest");
     out<<"{\"Version\":\""<<version<<"\"}\n";
     if(!out)throw std::runtime_error("Cannot flush last-good manifest");}
    if(!MoveFileExW(temp.c_str(),manifest.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)){
        fs::remove(temp);throw std::runtime_error("Cannot publish last-good.json");
    }
}
void run_checked(const fs::path& exe,const std::vector<std::wstring>& args,
                 const fs::path& repo,const fs::path& state,unsigned seconds,std::string_view name){
    const auto result=process(exe,args,repo,seconds);
    append_log(state,name,result.output);
    if(result.timed_out)throw std::runtime_error(std::string(name)+" timed out; see out/launcher/build.log");
    if(result.exit_code!=0)throw std::runtime_error(std::string(name)+" failed; see out/launcher/build.log");
}
}

std::wstring quote(std::wstring_view text){
    std::wstring result=L"\"";unsigned backslashes=0;
    for(const wchar_t ch:text){
        if(ch==L'\\'){++backslashes;continue;}
        if(ch==L'\"'){
            result.append(backslashes*2+1,L'\\');result.push_back(L'\"');backslashes=0;continue;
        }
        result.append(backslashes,L'\\');backslashes=0;result.push_back(ch);
    }
    result.append(backslashes*2,L'\\');result.push_back(L'\"');return result;
}

Result process(const fs::path& exe,const std::vector<std::wstring>& args,
               const fs::path& directory,unsigned timeout_seconds){
    static std::atomic<unsigned> sequence{};
    wchar_t temp_dir[MAX_PATH]{};
    const DWORD n=GetTempPathW(MAX_PATH,temp_dir);
    if(!n||n>=MAX_PATH)throw std::runtime_error("Cannot get temporary directory");
    const auto temporary=fs::path(temp_dir)/
        (L"fluxtop-native-"+std::to_wstring(GetCurrentProcessId())+L"-"+
         std::to_wstring(sequence.fetch_add(1))+L".tmp");
    SECURITY_ATTRIBUTES sa{sizeof(sa),nullptr,TRUE};
    Handle log(CreateFileW(temporary.c_str(),GENERIC_READ|GENERIC_WRITE,
        FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,&sa,CREATE_ALWAYS,
        FILE_ATTRIBUTE_TEMPORARY|FILE_FLAG_DELETE_ON_CLOSE,nullptr));
    if(!log.valid())throw std::runtime_error("Cannot create temporary process output");
    Handle input(CreateFileW(L"NUL",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,
        &sa,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr));
    if(!input.valid())throw std::runtime_error("Cannot open NUL");
    std::wstring command=quote(exe.wstring());
    for(const auto& arg:args){command.push_back(L' ');command+=quote(arg);}
    STARTUPINFOW start{sizeof(start)};
    start.dwFlags=STARTF_USESTDHANDLES|STARTF_USESHOWWINDOW;
    start.wShowWindow=SW_HIDE;
    start.hStdInput=input.h;start.hStdOutput=log.h;start.hStdError=log.h;
    PROCESS_INFORMATION pi{};
    // The caller intentionally uses only argv, never a command interpreter.
    if(!CreateProcessW(nullptr,command.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW,
        nullptr,directory.c_str(),&start,&pi))
        throw std::runtime_error("Could not launch child process (Windows error "+std::to_string(GetLastError())+")");
    Handle child(pi.hProcess),thread(pi.hThread);
    Handle job(CreateJobObjectW(nullptr,nullptr));
    if(job.valid()){
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if(!SetInformationJobObject(job.h,JobObjectExtendedLimitInformation,&limits,sizeof(limits))||
           !AssignProcessToJobObject(job.h,child.h)){
            // Some CI hosts already constrain children with an incompatible job.
            CloseHandle(job.h);job.h=INVALID_HANDLE_VALUE;
        }
    }
    const DWORD wait=WaitForSingleObject(child.h,timeout_seconds*1000U);
    bool timed_out=wait==WAIT_TIMEOUT;
    if(timed_out){
        if(job.valid()){TerminateJobObject(job.h,1);}
        else {TerminateProcess(child.h,1);}
        WaitForSingleObject(child.h,5000);
    }else if(wait!=WAIT_OBJECT_0){throw std::runtime_error("WaitForSingleObject failed");}
    DWORD code{};if(!GetExitCodeProcess(child.h,&code))code=1;
    LARGE_INTEGER beginning{};
    if(!SetFilePointerEx(log.h,beginning,nullptr,FILE_BEGIN))throw std::runtime_error("Cannot read child output");
    std::string output;
    char buffer[8192];DWORD got{};
    constexpr std::size_t cap=8U*1024U*1024U;
    while(output.size()<cap && ReadFile(log.h,buffer,static_cast<DWORD>(
            std::min<std::size_t>(sizeof(buffer),cap-output.size())),&got,nullptr) && got)
        output.append(buffer,got);
    return {timed_out?-1:static_cast<int>(code),timed_out,std::move(output)};
}

std::string clean_commit(const fs::path& repo){
    try {const auto state=checkout(repo);return state.dirty?"":state.commit;}
    catch(const std::exception&) {return {};}
}
std::string update(const fs::path& repo){
    State before;
    try{before=checkout(repo);}catch(...){return "git-unavailable";}
    if(before.commit.empty())return "no-head";
    if(before.dirty)return "local-changes";
    const auto branch=git(repo,{L"symbolic-ref",L"--quiet",L"--short",L"HEAD"});
    if(branch.exit_code!=0)return "detached-head";
    // All these markers are worktree-local. Resolve the Git directory once,
    // instead of starting six separate git.exe processes on each update check.
    // --absolute-git-dir also handles linked worktrees and subdirectories.
    const auto directory_result=git(repo,{L"rev-parse",L"--absolute-git-dir"});
    if(directory_result.exit_code!=0 || directory_result.timed_out)return "git-error";
    auto directory=fs::path(wide(trim(directory_result.output)));
    if(directory.empty())return "git-error";
    if(directory.is_relative())directory=repo/directory;
    for(const auto* marker:{L"MERGE_HEAD",L"CHERRY_PICK_HEAD",L"REVERT_HEAD",
                            L"rebase-merge",L"rebase-apply",L"sequencer"}){
        if(fs::exists(directory/marker))return "operation-in-progress";
    }
    const auto remote=git(repo,{L"for-each-ref",L"--format=%(upstream:remotename)",L"--",
                                L"refs/heads/"+wide(trim(branch.output))});
    const auto upstream=git(repo,{L"for-each-ref",L"--format=%(upstream:remoteref)",L"--",
                                  L"refs/heads/"+wide(trim(branch.output))});
    if(remote.exit_code!=0||upstream.exit_code!=0)return "git-error";
    const auto name=trim(remote.output),ref=trim(upstream.output);
    if(name.empty()||name=="."||ref.rfind("refs/heads/",0)!=0)return "no-upstream";
    const auto fetch=git(repo,{L"fetch",L"--no-tags",L"--no-recurse-submodules",L"--",wide(name),wide(ref)});
    if(fetch.exit_code!=0||fetch.timed_out)return "fetch-failed";
    const auto target=git(repo,{L"rev-parse",L"--verify",L"FETCH_HEAD^{commit}"});
    if(target.exit_code!=0)return "git-error";
    try{
        const auto after=checkout(repo);
        const auto after_branch=git(repo,{L"symbolic-ref",L"--quiet",L"--short",L"HEAD"});
        if(after.dirty||after.commit!=before.commit||after_branch.exit_code!=0||
           trim(after_branch.output)!=trim(branch.output))return "checkout-changed";
    }catch(...){return "checkout-changed";}
    if(trim(target.output)==before.commit)return "up-to-date";
    const auto ancestor=git(repo,{L"merge-base",L"--is-ancestor",wide(before.commit),wide(trim(target.output))});
    if(ancestor.exit_code!=0)return "local-commits-or-diverged";
    const auto merge=git(repo,{L"merge",L"--ff-only",L"--no-edit",L"--",wide(trim(target.output))});
    return merge.exit_code==0?"updated":"merge-failed";
}

fs::path last_good(const fs::path& state){
    std::ifstream in(state/L"last-good.json",std::ios::binary);
    if(!in)return {};
    const std::string contents((std::istreambuf_iterator<char>(in)),std::istreambuf_iterator<char>());
    std::smatch match;
    if(!std::regex_search(contents,match,std::regex(R"rx("Version"\s*:\s*"([A-Za-z0-9-]+)")rx")))return {};
    const std::string version=match[1].str();
    if(!safe_version(version))return {};
    const auto executable=state/L"versions"/wide(version)/L"fluxtop.exe";
    return fs::is_regular_file(executable)?executable:fs::path{};
}
fs::path build_and_publish(const fs::path& repo,const fs::path& state,const std::string& version){
    if(!safe_version(version))throw std::runtime_error("Invalid build version identifier");
    const auto cmake=cmake_tool();
    const auto ctest=cmake.parent_path()/L"ctest.exe";
    if(!fs::exists(ctest))throw std::runtime_error("Ctest not found next to CMake");
    const auto build=state/L"build"/wide(version);
    fs::create_directories(build);
    {std::ofstream clear(state/L"build.log",std::ios::binary|std::ios::trunc);}
    std::vector<std::wstring> configure={L"-S",repo.wstring(),L"-B",build.wstring(),L"-DBUILD_TESTING=ON"};
    const auto generator=environment(L"CMAKE_GENERATOR");
    const bool ninja=generator.find(L"Ninja")!=std::wstring::npos;
    if(ninja){configure.insert(configure.end(),{L"-G",L"Ninja",L"-DCMAKE_BUILD_TYPE=Release"});}
    else{configure.insert(configure.end(),{L"-A",L"x64"});}
    run_checked(cmake,configure,repo,state,300,"configure");
    run_checked(cmake,{L"--build",build.wstring(),L"--config",L"Release"},
                repo,state,600,"build");
    run_checked(ctest,{L"--test-dir",build.wstring(),L"-C",L"Release",L"--output-on-failure",L"--timeout",L"75"},
                repo,state,240,"ctest");
    const auto binary=ninja?build/L"fluxtop.exe":build/L"Release"/L"fluxtop.exe";
    if(!fs::is_regular_file(binary))throw std::runtime_error("Built monitor is missing");
    const auto target=state/L"versions"/wide(version);
    fs::create_directories(target);
    const auto pending=target/L"fluxtop.pending.exe";
    fs::copy_file(binary,pending,fs::copy_options::overwrite_existing);
    const auto published=target/L"fluxtop.exe";
    if(!MoveFileExW(pending.c_str(),published.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)){
        fs::remove(pending);throw std::runtime_error("Cannot publish monitor binary");
    }
    publish_manifest(state,version);
    return published;
}
fs::path prepare(const fs::path& repo,const fs::path& state,bool no_update,bool strict_prepare){
    fs::create_directories(state);
    auto previous=last_good(state);
    if(previous.empty())for(const auto& candidate:{repo/L"build"/L"Release"/L"fluxtop.exe",
                                                  repo/L"build"/L"ninja"/L"fluxtop.exe"}){
        if(fs::is_regular_file(candidate)){previous=candidate;break;}
    }
    try{
        if(!no_update){
            const auto status=update(repo);
            std::ofstream log(state/L"source-launcher.log",std::ios::binary|std::ios::app);
            log<<"Git update: "<<status<<"\n";
        }
        const auto commit=clean_commit(repo);
        const auto version=commit.empty()?"local-"+nonce():commit;
        if(auto verified=last_good(state);!verified.empty()&&
             verified.parent_path().filename()==wide(version))return verified;
        return build_and_publish(repo,state,version);
    }catch(const std::exception& error){
        std::ofstream log(state/L"source-launcher.log",std::ios::binary|std::ios::app);
        log<<"Build/update failure: "<<error.what()<<"\n";
        if(!previous.empty()&&!strict_prepare)return previous;
        throw;
    }
}
fs::path locate_repository(const fs::path& executable){
    auto folder=fs::absolute(executable).parent_path();
    while(!folder.empty()){
        if(fs::is_regular_file(folder/L"CMakeLists.txt")&&
           fs::is_regular_file(folder/L"src"/L"main.cpp"))return folder;
        const auto parent=folder.parent_path();if(parent==folder)break;folder=parent;
    }
    throw std::runtime_error("Cannot locate a FluxTop source checkout; use --repo PATH");
}
}
