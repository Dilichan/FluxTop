// Offline Windows integration fixtures for the standalone FluxTop launcher.
// Both modes use --data-dir, so real installation/Startup/registry state is untouched.
#include <windows.h>
#include <bcrypt.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>
#include <array>
#include <system_error>
#include <stdexcept>
#include <cstdint>
#include <iterator>
#include <algorithm>

namespace {
namespace fs=std::filesystem;
void check(bool ok,const char* message) { if(!ok)throw std::runtime_error(message); }
struct Fixture {
    fs::path root;
    explicit Fixture(const fs::path& build,const wchar_t* type) {
        root=build/(std::wstring(type)+L"-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
        fs::create_directories(root);
    }
    ~Fixture() { std::error_code ec;fs::remove_all(root,ec); }
};
void put(const fs::path& path,const std::string& contents) {
    std::ofstream out(path,std::ios::binary|std::ios::trunc);
    check(static_cast<bool>(out),"Cannot create fixture file");
    out.write(contents.data(),static_cast<std::streamsize>(contents.size()));
    check(static_cast<bool>(out),"Cannot write fixture file");
}
std::string get(const fs::path& path) {
    std::ifstream in(path,std::ios::binary);
    check(static_cast<bool>(in),"Cannot read fixture file");
    return {std::istreambuf_iterator<char>(in),std::istreambuf_iterator<char>()};
}
bool same_file(const fs::path& a,const fs::path& b) {
    std::ifstream x(a,std::ios::binary),y(b,std::ios::binary);
    if(!x || !y)return false;
    return std::equal(std::istreambuf_iterator<char>(x),std::istreambuf_iterator<char>(),
        std::istreambuf_iterator<char>(y),std::istreambuf_iterator<char>());
}
// SHA-256 via Windows CNG; mirrors the release launcher's content-addressed cache path.
std::wstring sha256(const fs::path& file) {
    BCRYPT_ALG_HANDLE algorithm{};
    BCRYPT_HASH_HANDLE handle{};
    std::vector<UCHAR> object;
    std::array<UCHAR,32> digest{};
    try {
        check(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)>=0,"CNG provider unavailable");
        DWORD length{},returned{};
        check(BCryptGetProperty(algorithm,BCRYPT_OBJECT_LENGTH,reinterpret_cast<PUCHAR>(&length),
            sizeof(length),&returned,0)>=0 && length>0,"CNG object size unavailable");
        object.resize(length);
        check(BCryptCreateHash(algorithm,&handle,object.data(),length,nullptr,0,0)>=0,"CNG hash creation failed");
        std::ifstream stream(file,std::ios::binary);
        check(static_cast<bool>(stream),"Cannot open payload for SHA-256");
        std::array<char,65536> chunk{};
        while(stream) {
            stream.read(chunk.data(),static_cast<std::streamsize>(chunk.size()));
            const auto bytes=stream.gcount();
            if(bytes>0)check(BCryptHashData(handle,reinterpret_cast<PUCHAR>(chunk.data()),
                static_cast<ULONG>(bytes),0)>=0,"CNG hashing failed");
        }
        check(BCryptFinishHash(handle,digest.data(),static_cast<ULONG>(digest.size()),0)>=0,
            "CNG hash finalize failed");
    }catch(...){if(handle)BCryptDestroyHash(handle);if(algorithm)BCryptCloseAlgorithmProvider(algorithm,0);throw;}
    if(handle)BCryptDestroyHash(handle);
    if(algorithm)BCryptCloseAlgorithmProvider(algorithm,0);
    constexpr wchar_t hex[]=L"0123456789abcdef";
    std::wstring result;
    result.reserve(digest.size()*2);
    for(auto byte:digest){result.push_back(hex[byte>>4]);result.push_back(hex[byte&15]);}
    return result;
}
// Execute directly with CreateProcessW; never elevate, never launch a command shell.
DWORD launch(const fs::path& executable,const fs::path& fixture,const wchar_t* command) {
    std::wstring arguments=L"\""+executable.wstring()+L"\" "+command+
        L" --data-dir \""+fixture.wstring()+L"\"";
    STARTUPINFOW startup{};startup.cb=sizeof(startup);
    PROCESS_INFORMATION process{};
    if(!CreateProcessW(executable.c_str(),arguments.data(),nullptr,nullptr,FALSE,
        CREATE_NO_WINDOW,nullptr,fixture.c_str(),&startup,&process))
        throw std::runtime_error("CreateProcessW launcher failed: "+std::to_string(GetLastError()));
    const DWORD waited=WaitForSingleObject(process.hProcess,30000);
    if(waited!=WAIT_OBJECT_0){TerminateProcess(process.hProcess,1);WaitForSingleObject(process.hProcess,5000);}
    DWORD exitCode=1;
    GetExitCodeProcess(process.hProcess,&exitCode);
    CloseHandle(process.hThread);CloseHandle(process.hProcess);
    check(waited==WAIT_OBJECT_0,"Launcher fixture timed out");
    return exitCode;
}
void portable(const fs::path& launcher,const fs::path& monitor,const fs::path& build) {
    Fixture fixture(build,L"portable native space");
    check(launch(launcher,fixture.root,L"--prepare-only")==0,"Portable preparation failed");
    const fs::path extracted=fixture.root/L"versions"/sha256(monitor)/L"fluxtop.exe";
    check(fs::is_regular_file(extracted),"Embedded monitor is missing");
    check(same_file(monitor,extracted),"Extracted monitor differs from source payload");
    for(const auto* module:{L"IntelMSR.bin",L"AMDFamily17.bin"})
        check(same_file(monitor.parent_path()/module,extracted.parent_path()/module),"Bundled CPU module mismatch");
    check(fs::is_regular_file(fixture.root/L"FluxTop.exe"),"Stable launcher copy is missing");
    const auto history=fixture.root/L"history.db";
    put(history,"history sentinel");
    put(extracted,"deliberately corrupted cache");
    check(launch(launcher,fixture.root,L"--prepare-only")==0,"Corrupt cache repair failed");
    check(same_file(monitor,extracted),"Corrupt cache was not repaired");
    check(get(history)=="history sentinel","History was changed by portable repair");
}
void install(const fs::path& launcher,const fs::path& build) {
    Fixture fixture(build,L"install-native");
    const auto instance=GetModuleHandleW(nullptr);
    constexpr wchar_t monitor_class[]=L"FluxTop.Overlay.Window";
    WNDCLASSW window_class{};
    window_class.hInstance=instance;
    window_class.lpfnWndProc=DefWindowProcW;
    window_class.lpszClassName=monitor_class;
    check(RegisterClassW(&window_class)!=0,"Cannot register simulated running monitor");
    struct MonitorWindow {
        HWND window;
        ~MonitorWindow(){if(window)DestroyWindow(window);}
    } monitor{CreateWindowExW(0,monitor_class,L"",WS_POPUP,0,0,1,1,nullptr,nullptr,instance,nullptr)};
    check(monitor.window!=nullptr,"Cannot create simulated running monitor");
    const auto history=fixture.root/L"history.db";
    put(history,"history remains unchanged");
    check(launch(launcher,fixture.root,L"--install-only")==0,"Fixture installation failed");
    const auto installed=fixture.root/L"FluxTop.exe";
    check(fs::is_regular_file(installed),"Installed entry point missing");
    check(same_file(launcher,installed),"Installed launcher content mismatch");
    check(fs::is_regular_file(fixture.root/L"Uninstall-FluxTop.exe"),"Native uninstaller payload was not extracted");
    check(launch(launcher,fixture.root,L"--install-only")==0,"Repeat install failed");
    check(get(history)=="history remains unchanged","Installer altered history");
    check(IsWindow(monitor.window)!=FALSE,"Isolated installation closed the running monitor");
}
}
int wmain(int argc,wchar_t** argv) {
    if(argc!=5){std::cerr<<"usage: native_launcher_fixture_tests <portable|installation_fixture> <launcher> <monitor> <build>\n";return 2;}
    try {
        const fs::path launcher=argv[2],monitor=argv[3],build=argv[4];
        if(std::wstring_view(argv[1])==L"portable")portable(launcher,monitor,build);
        else if(std::wstring_view(argv[1])==L"installation_fixture")install(launcher,build);
        else throw std::runtime_error("Unknown fixture mode");
        std::cout<<"Native launcher fixture passed\n";return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
