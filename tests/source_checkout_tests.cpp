#include "fluxtop/source_checkout.hpp"
#include <windows.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {
namespace fs=std::filesystem;
namespace source=fluxtop::source_checkout;
void require(bool okay,std::string_view why){if(!okay)throw std::runtime_error(std::string(why));}
fs::path absolute(int n,wchar_t** args,int at){require(n>at,"fixture argument missing");return fs::absolute(args[at]);}
void save(const fs::path& p,std::string_view value){
    fs::create_directories(p.parent_path());
    std::ofstream out(p,std::ios::binary|std::ios::trunc);out<<value;
    require(static_cast<bool>(out),"Cannot write fixture");
}
std::string trim(std::string s){
    const auto begin=s.find_first_not_of(" \r\n\t");
    if(begin==std::string::npos)return {};
    return s.substr(begin,s.find_last_not_of(" \r\n\t")-begin+1);
}
std::string git(const fs::path& dir,std::vector<std::wstring> arguments){
    // Fixtures must remain unattended even with a developer's global Git settings.
    std::vector<std::wstring> args={L"-c",L"user.name=FluxTop Tests",L"-c",L"user.email=tests@example.invalid",
                                    L"-c",L"core.hooksPath=NUL",L"-c",L"commit.gpgsign=false",
                                    L"-c",L"core.fsmonitor=false",L"-c",L"maintenance.auto=false",
                                    L"-c",L"gc.auto=0"};
    args.insert(args.end(),arguments.begin(),arguments.end());
    const auto result=source::process(L"git.exe",args,dir,30);
    if(result.exit_code!=0||result.timed_out)throw std::runtime_error("Fixture git failed: "+result.output);
    return trim(result.output);
}
void commit(const fs::path& dir,std::string_view value){
    save(dir/L"note.txt",value);
    git(dir,{L"add",L"note.txt"});
    std::wstring message(value.begin(),value.end());
    git(dir,{L"commit",L"-m",message});
}
struct Cleanup{fs::path path;~Cleanup(){std::error_code ignored;fs::remove_all(path,ignored);}};
fs::path fixture(const fs::path& root){
    const auto path=root/(L"native-source-tests-"+std::to_wstring(GetCurrentProcessId())+L"-"+
        std::to_wstring(GetTickCount64()));
    fs::create_directories(path);return path;
}
void test_git(const fs::path& root){
    Cleanup clean{fixture(root)};const auto base=clean.path;
    // Print checkpoints immediately: a CI timeout must identify the stage.
    const auto phase=[](const char* message){std::cerr<<"source_git_update: "<<message<<'\n';};
    phase("initialize local repositories");
    const auto remote=base/L"remote.git",seed=base/L"seed",client=base/L"client";
    git(base,{L"init",L"--bare",L"--initial-branch=main",remote.wstring()});
    git(base,{L"init",L"--initial-branch=main",seed.wstring()});
    phase("unborn HEAD and no-upstream");
    require(source::update(seed)=="no-head","Unborn checkout should skip");
    commit(seed,"first");
    require(source::update(seed)=="no-upstream","Missing upstream must skip");
    git(seed,{L"remote",L"add",L"origin",remote.wstring()});
    git(seed,{L"push",L"-u",L"origin",L"main"});
    git(base,{L"clone",remote.wstring(),client.wstring()});
    phase("cloned checkout and fast-forward");
    require(source::update(client)=="up-to-date","Initial clone must be current");
    commit(seed,"second");git(seed,{L"push"});
    require(source::update(client)=="updated","Remote fast forward failed");
    require(source::update(client)=="up-to-date","Update not idempotent");
    phase("tracked and untracked edit protection");
    save(client/L"note.txt","local edit");
    const auto before=git(client,{L"rev-parse",L"HEAD"});
    require(source::update(client)=="local-changes","Tracked edit allowed update");
    require(before==git(client,{L"rev-parse",L"HEAD"}),"Dirty HEAD changed");
    git(client,{L"restore",L"note.txt"});
    save(client/L"untracked.txt","preserved");
    require(source::update(client)=="local-changes","Untracked file allowed update");
    fs::remove(client/L"untracked.txt");
    phase("detached HEAD protection");
    git(client,{L"checkout",L"--detach"});
    require(source::update(client)=="detached-head","Detached HEAD not protected");
    git(client,{L"checkout",L"main"});
    phase("merge/rebase marker protection");
    const auto marker=client/L".git"/L"MERGE_HEAD";
    save(marker,before);
    require(source::update(client)=="operation-in-progress","Merge in progress not protected");
    fs::remove(marker);
    phase("local commits and divergence");
    commit(client,"local commit");
    require(source::update(client)=="local-commits-or-diverged","Local ahead permitted");
    commit(seed,"remote third");git(seed,{L"push"});
    require(source::update(client)=="local-commits-or-diverged","Divergent history permitted");
    phase("offline fetch and unsafe manifest");
    git(client,{L"remote",L"set-url",L"origin",(base/L"missing.git").wstring()});
    require(source::update(client)=="fetch-failed","Missing remote should fail offline");
    const auto state=base/L"state";fs::create_directories(state);
    save(state/L"last-good.json","{\"Version\":\"../../outside\"}");
    require(source::last_good(state).empty(),"Unsafe manifest path accepted");
    std::cout<<"Native Git fast-forward, isolation and manifest security passed\n";
}
void test_process(const fs::path& root,const fs::path& own){
    Cleanup clean{fixture(root)};
    const std::wstring input=L"space \"quote\" and trailing\\";
    const auto result=source::process(own,{L"--echo",input},clean.path,15);
    const std::string expected="space \"quote\" and trailing\\";
    require(result.exit_code==0&&result.output==expected,"Native argv quoting roundtrip failed");
    const auto timeout=source::process(own,{L"--sleep"},clean.path,1);
    require(timeout.timed_out&&timeout.exit_code==-1,"Child timeout was not enforced");
    std::cout<<"Native quoting and child timeout passed\n";
}
void test_fallback(const fs::path& root,const fs::path& monitor,const fs::path& launcher){
    Cleanup clean{fixture(root)};const auto repo=clean.path;
    const auto state=repo/L"out"/L"launcher";
    const auto previous=state/L"versions"/L"previous"/L"fluxtop.exe";
    fs::create_directories(previous.parent_path());
    fs::copy_file(monitor,previous);
    save(state/L"last-good.json","{\"Version\":\"previous\"}\n");
    save(repo/L"CMakeLists.txt","cmake_minimum_required(VERSION 3.24)\nmessage(FATAL_ERROR \"intentional fixture error\")\n");
    require(source::prepare(repo,state,true,false)==previous,"Broken build must keep previous binary");
    bool threw=false;
    try{(void)source::prepare(repo,state,true,true);}catch(const std::exception&){threw=true;}
    require(threw,"--prepare-only failed to surface broken build");
    require(source::last_good(state)==previous,"Broken build replaced manifest");
    const auto locked=state/L"update.lock";
    HANDLE lock=CreateFileW(locked.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    require(lock&&lock!=INVALID_HANDLE_VALUE,"Could not open checkout lock fixture");
    const auto test=source::process(launcher,{L"--repo",repo.wstring(),L"--no-update",L"--prepare-only"},repo,15);
    CloseHandle(lock);
    require(test.exit_code!=0&&test.output.find("already active")!=std::string::npos,"Parallel native launcher bypassed lock");
    git(repo,{L"init",L"--initial-branch=main"});
    save(repo/L".gitignore","/out/\n");
    git(repo,{L"add",L".gitignore",L"CMakeLists.txt"});
    git(repo,{L"commit",L"-m",L"fixture"});
    const auto head=git(repo,{L"rev-parse",L"HEAD"});
    const auto cached=state/L"versions"/fs::path(head)/L"fluxtop.exe";
    fs::create_directories(cached.parent_path());fs::copy_file(monitor,cached);
    save(state/L"last-good.json","{\"Version\":\""+head+"\"}\n");
    const auto log=state/L"build.log";
    const auto before=fs::last_write_time(log);
    require(source::prepare(repo,state,true,true)==cached,"Verified clean commit rebuilt unnecessarily");
    require(before==fs::last_write_time(log),"Cache hit modified build log");
    std::cout<<"Native launcher fallback, prepare-only, locking and cache passed\n";
}
}
int wmain(int argc,wchar_t** argv){
    try{
        if(argc==3&&std::wstring_view(argv[1])==L"--echo"){
            // This fixture deliberately has ASCII-only test text.
            std::string text;
            for(const wchar_t* p=argv[2];*p;++p)text.push_back(static_cast<char>(*p));
            std::cout<<text;return 0;
        }
        if(argc==2&&std::wstring_view(argv[1])==L"--sleep"){
            Sleep(10000);return 0;
        }
        require(argc==5,"Expected: root monitor launcher git-test-host");
        const auto root=absolute(argc,argv,1);
        const auto monitor=absolute(argc,argv,2);
        const auto launcher=absolute(argc,argv,3);
        const auto mode=std::wstring_view(argv[4]);
        if(mode==L"git")test_git(root);
        else if(mode==L"process")test_process(root,fs::absolute(argv[0]));
        else if(mode==L"fallback")test_fallback(root,monitor,launcher);
        else throw std::runtime_error("Unsupported native checkout test mode");
        return 0;
    }catch(const std::exception& error){
        std::cerr<<"Native source checkout fixture failed: "<<error.what()<<'\n';return 1;
    }
}
