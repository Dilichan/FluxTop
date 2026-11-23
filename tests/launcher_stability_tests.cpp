#include "fluxtop/launcher_lock.hpp"
#include "fluxtop/windows.hpp"
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <thread>
namespace {
namespace fs=std::filesystem;
void check(bool yes,const char* text){if(!yes)throw std::runtime_error(text);}
struct Fixture {
    fs::path root;
    explicit Fixture(const fs::path& base):root(base/(L"launcher-stability-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()))){fs::create_directories(root);}
    ~Fixture(){std::error_code ec;fs::remove_all(root,ec);}
};
struct Child {
    PROCESS_INFORMATION process{};
    Child(const fs::path& exe,const fs::path& directory,const wchar_t* args){
        auto command=L"\""+exe.wstring()+L"\" "+args+L" --data-dir \""+directory.wstring()+L"\"";
        STARTUPINFOW start{};start.cb=sizeof(start);
        check(CreateProcessW(exe.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,directory.c_str(),&start,&process)!=FALSE,"launch owned fixture");
    }
    ~Child(){if(process.hProcess){if(WaitForSingleObject(process.hProcess,0)==WAIT_TIMEOUT){TerminateProcess(process.hProcess,54);WaitForSingleObject(process.hProcess,5000);}CloseHandle(process.hProcess);CloseHandle(process.hThread);}}
    DWORD exit(){check(WaitForSingleObject(process.hProcess,20000)==WAIT_OBJECT_0,"launcher must finish within fixture timeout");DWORD value{};check(GetExitCodeProcess(process.hProcess,&value)!=FALSE,"read child exit");return value;}
};
struct Lock {
    fluxtop::Handle handle;
    explicit Lock(const std::wstring& name):handle(CreateMutexW(nullptr,FALSE,name.c_str())){
        check(handle.valid(),"create bootstrap lock");const auto rc=WaitForSingleObject(handle.get(),5000);
        check(rc==WAIT_OBJECT_0 || rc==WAIT_ABANDONED,"acquire bootstrap lock");
    }
    ~Lock(){ReleaseMutex(handle.get());}
};
}
int wmain(int argc,wchar_t** argv){try{
    check(argc==3,"launcher and fixture root required");const fs::path launcher=argv[1];Fixture fixture(argv[2]);
    const auto isolated=fixture.root/L"isolated";fs::create_directories(isolated);
    {
        Lock unrelated(fluxtop::launcher_lock_name({},false));
        Child install(launcher,isolated,L"--install-only");
        check(install.exit()==0 && fs::is_regular_file(isolated/L"FluxTop.exe"),"custom installation must not silently disappear behind the real installed launcher's lock");
    }
    {
        const auto blocked=fixture.root/L"busy";fs::create_directories(blocked);
        Lock same(fluxtop::launcher_lock_name(blocked,true));Child install(launcher,blocked,L"--install-only");
        check(install.exit()==ERROR_BUSY && !fs::exists(blocked/L"FluxTop.exe"),"busy lock must return an explicit failure, not report a successful installation");
    }
    {
        const auto startup=fixture.root/L"startup";fs::create_directories(startup);
        Child delayed(launcher,startup,L"--autostart");std::this_thread::sleep_for(std::chrono::milliseconds(500));
        Child install(launcher,startup,L"--install-only");
        check(install.exit()==0 && fs::is_regular_file(startup/L"FluxTop.exe"),"autostart's 30-second delay must not lock out ordinary starts/installations");
        check(WaitForSingleObject(delayed.process.hProcess,0)==WAIT_TIMEOUT,"owned autostart must still be in its delay; no UAC or monitor started");
    }
    std::cout<<"Bootstrap isolation, explicit busy result and nonblocking autostart delay passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
