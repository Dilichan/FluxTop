#include "fluxtop/uninstall_cleanup.hpp"
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
namespace fs=std::filesystem;
void check(bool value,const char* reason) {if(!value)throw std::runtime_error(reason);}
void put(const fs::path& file,const std::string& content) {
    std::ofstream out(file,std::ios::binary);
    if(!out || !(out<<content))throw std::runtime_error("Cannot write fixture file");
}
std::string read(const fs::path& file) {
    std::ifstream in(file,std::ios::binary);
    if(!in)throw std::runtime_error("Fixture sentinel is missing");
    return {std::istreambuf_iterator<char>(in),std::istreambuf_iterator<char>()};
}
}
int wmain(int argc,wchar_t** argv) {
    if(argc!=2)return 2;
    const fs::path root=fs::path(argv[1])/
        (L"native-uninstall-fixture-"+std::to_wstring(GetCurrentProcessId()));
    try {
        fs::create_directories(root/L"versions"/L"hash");
        for(const wchar_t* file:{L"FluxTop.exe",L"FluxTop.previous.exe",L"Uninstall-FluxTop.cmd"})
            put(root/file,"executable placeholder");
        put(root/L"versions"/L"hash"/L"fluxtop.exe","embedded monitor");
        for(const wchar_t* data:{L"fluxtop.db",L"fluxtop.db-wal",L"fluxtop.db-shm",
                                 L"activity.log",L"overlay.ini",L"other-user-file.txt",L"Uninstall-FluxTop.exe"})
            put(root/data,"preserve marker");
        fluxtop::remove_installed_binaries(root);
        check(!fs::exists(root/L"FluxTop.exe"),"Main launcher was retained");
        check(!fs::exists(root/L"FluxTop.previous.exe"),"Rollback executable was retained");
        check(!fs::exists(root/L"Uninstall-FluxTop.cmd"),"Legacy CMD was retained");
        check(!fs::exists(root/L"versions"),"Extracted monitor cache was retained");
        for(const wchar_t* data:{L"fluxtop.db",L"fluxtop.db-wal",L"fluxtop.db-shm",
                                 L"activity.log",L"overlay.ini",L"other-user-file.txt",L"Uninstall-FluxTop.exe"})
            check(read(root/data)=="preserve marker","Private user data was modified");
        fluxtop::remove_installed_binaries(root);
        fs::remove_all(root);
        std::cout<<"Native uninstall keeps data and removes only owned executables\n";
        return 0;
    }catch(const std::exception& e){
        std::cerr<<e.what()<<'\n';
        std::error_code ec;fs::remove_all(root,ec);
        return 1;
    }
}
