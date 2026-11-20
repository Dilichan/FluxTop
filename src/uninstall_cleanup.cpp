#include "fluxtop/uninstall_cleanup.hpp"
#include <windows.h>
#include <filesystem>
#include <stdexcept>
#include <system_error>

namespace fluxtop {
namespace {
namespace fs=std::filesystem;
void remove_file(const fs::path& file) {
    for(unsigned attempt=0;attempt<40;++attempt) {
        const auto attributes=GetFileAttributesW(file.c_str());
        if(attributes==INVALID_FILE_ATTRIBUTES) {
            if(GetLastError()==ERROR_FILE_NOT_FOUND || GetLastError()==ERROR_PATH_NOT_FOUND)return;
            throw std::system_error(static_cast<int>(GetLastError()),std::system_category(),"Inspect installed file");
        }
        if(attributes & (FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))
            throw std::runtime_error("Refusing to remove a non-file or reparse point");
        if(DeleteFileW(file.c_str()))return;
        const DWORD error=GetLastError();
        if((error!=ERROR_ACCESS_DENIED && error!=ERROR_SHARING_VIOLATION) || attempt==39)
            throw std::system_error(static_cast<int>(error),std::system_category(),"Remove installed file");
        Sleep(250);
    }
}
}
void remove_installed_binaries(const fs::path& root) {
    // Refuse redirected application directories: never walk a junction/symlink.
    const auto parent_attributes=GetFileAttributesW(root.c_str());
    if(parent_attributes==INVALID_FILE_ATTRIBUTES || (parent_attributes&FILE_ATTRIBUTE_REPARSE_POINT))
        throw std::runtime_error("Unsafe or missing FluxTop installation directory");
    const auto versions=root/L"versions";
    const auto attributes=GetFileAttributesW(versions.c_str());
    if(attributes!=INVALID_FILE_ATTRIBUTES) {
        if(attributes&FILE_ATTRIBUTE_REPARSE_POINT)
            throw std::runtime_error("Refusing to traverse redirected versions directory");
        if(!(attributes&FILE_ATTRIBUTE_DIRECTORY))
            throw std::runtime_error("Versions path is not a directory");
        std::error_code error;
        fs::remove_all(versions,error);
        if(error)throw fs::filesystem_error("Remove installed versions",versions,error);
    } else if(GetLastError()!=ERROR_FILE_NOT_FOUND && GetLastError()!=ERROR_PATH_NOT_FOUND) {
        throw std::system_error(static_cast<int>(GetLastError()),std::system_category(),"Inspect versions directory");
    }
    remove_file(root/L"FluxTop.previous.exe");
    remove_file(root/L"FluxTop.exe");
    remove_file(root/L"Uninstall-FluxTop.cmd");
}
} // namespace fluxtop
