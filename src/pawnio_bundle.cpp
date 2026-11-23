#include "fluxtop/pawnio_bundle.hpp"
#include "fluxtop/pawnio_policy.hpp"
#include "fluxtop/diagnostics.hpp"
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <sddl.h>
#include <wincrypt.h>
#include <bcrypt.h>
#include <wintrust.h>
#include <softpub.h>
#include <array>
#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>
#include <cstdint>

namespace fluxtop {
namespace {
constexpr unsigned resource_id=207;
constexpr std::array<unsigned char,32> expected_sha{{
    0x1f,0x51,0x9a,0x22,0xe4,0x71,0x87,0xf7,
    0x0a,0x13,0x79,0xa4,0x8c,0xa6,0x04,0x98,
    0x1c,0x4f,0xcf,0x69,0x4f,0x4e,0x65,0xb7,
    0x34,0xaa,0xa7,0x4a,0x9f,0xba,0x30,0x32
}};
struct Module {
    HMODULE value{};
    explicit Module(const std::filesystem::path& file) noexcept :
        value(LoadLibraryExW(file.c_str(),nullptr,
            LOAD_LIBRARY_AS_DATAFILE|LOAD_LIBRARY_AS_IMAGE_RESOURCE)) {}
    ~Module(){if(value) FreeLibrary(value);}
    Module(const Module&)=delete;
    Module& operator=(const Module&)=delete;
};
// Read the exact embedded official setup; no internet access at runtime.
std::vector<unsigned char> installer_bytes(const std::filesystem::path& launcher) {
    Module module(launcher);
    if(!module.value) return {};
    const auto resource=FindResourceW(module.value,MAKEINTRESOURCEW(resource_id),RT_RCDATA);
    if(!resource)return {};
    const auto content=LoadResource(module.value,resource);
    const auto data=content?static_cast<const unsigned char*>(LockResource(content)):nullptr;
    const DWORD size=SizeofResource(module.value,resource);
    if(!data || size<1024 || size>32U*1024U*1024U)return {};
    return {data,data+size};
}
// Independent SHA-256 check before writing or executing a PE payload.
bool matches_sha(const unsigned char* data,std::size_t size) noexcept {
    if(!data || !size || size>32U*1024U*1024U)return false;
    std::array<BYTE,32> digest{};
    DWORD length=static_cast<DWORD>(digest.size());
    return CryptHashCertificate2(BCRYPT_SHA256_ALGORITHM,0,nullptr,data,
        static_cast<DWORD>(size),digest.data(),&length)!=FALSE
        && length==digest.size() && digest==expected_sha;
}
bool verify_authenticode(const std::filesystem::path& file) noexcept {
    WINTRUST_FILE_INFO info{};
    info.cbStruct=sizeof(info);
    info.pcwszFilePath=file.c_str();
    WINTRUST_DATA trust{};
    trust.cbStruct=sizeof(trust);
    trust.dwUIChoice=WTD_UI_NONE;
    trust.fdwRevocationChecks=WTD_REVOKE_NONE;
    trust.dwUnionChoice=WTD_CHOICE_FILE;
    trust.pFile=&info;
    trust.dwStateAction=WTD_STATEACTION_VERIFY;
    trust.dwProvFlags=WTD_CACHE_ONLY_URL_RETRIEVAL|WTD_REVOCATION_CHECK_NONE;
    GUID action=WINTRUST_ACTION_GENERIC_VERIFY_V2;
    const auto status=WinVerifyTrust(nullptr,&action,&trust);
    trust.dwStateAction=WTD_STATEACTION_CLOSE;
    (void)WinVerifyTrust(nullptr,&action,&trust);
    return status==ERROR_SUCCESS;
}
// FluxTop.PawnIO.Security.v48.4
// The elevated installer never executes a PE from %LOCALAPPDATA% or any
// directory writable by the unelevated user. The Windows Program Files
// directory is restricted; each staging subdirectory additionally receives
// an explicit Administrators+SYSTEM-only DACL.
std::filesystem::path secure_setup_directory() {
    PWSTR known{};
    const HRESULT hr=SHGetKnownFolderPath(FOLDERID_ProgramFiles,0,nullptr,&known);
    if(FAILED(hr)||!known) return {};
    const std::filesystem::path base(known);
    CoTaskMemFree(known);

    std::array<UCHAR,16> nonce{};
    if(BCryptGenRandom(nullptr,nonce.data(),static_cast<ULONG>(nonce.size()),
        BCRYPT_USE_SYSTEM_PREFERRED_RNG)<0) return {};
    constexpr wchar_t hex[]=L"0123456789abcdef";
    std::wstring suffix;
    for(const auto byte:nonce){suffix.push_back(hex[byte>>4]);suffix.push_back(hex[byte&15]);}
    const auto path=base/(L"FluxTop-PawnIO-"+suffix);

    PSECURITY_DESCRIPTOR security{};
    if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(
        L"D:P(A;;FA;;;SY)(A;;FA;;;BA)",SDDL_REVISION_1,&security,nullptr))return {};
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength=sizeof(attributes);
    attributes.lpSecurityDescriptor=security;
    attributes.bInheritHandle=FALSE;
    const BOOL created=CreateDirectoryW(path.c_str(),&attributes);
    LocalFree(security);
    return created ? path : std::filesystem::path{};
}
// A second hash check covers the precise file staged on disk, not only
// the bytes extracted from the resource. Never trust an altered payload.
bool staged_file_matches_sha(const std::filesystem::path& path) {
    const HANDLE handle=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,
        nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(handle==INVALID_HANDLE_VALUE)return false;
    LARGE_INTEGER length{};
    const bool size_ok=GetFileSizeEx(handle,&length)!=FALSE &&
        length.QuadPart>=1024 && length.QuadPart<=32LL*1024*1024;
    std::vector<unsigned char> bytes;
    bool ok=false;
    if(size_ok) {
        bytes.resize(static_cast<std::size_t>(length.QuadPart));
        DWORD read{};
        ok=ReadFile(handle,bytes.data(),static_cast<DWORD>(bytes.size()),&read,nullptr)!=FALSE
            && read==bytes.size();
    }
    CloseHandle(handle);
    return ok && matches_sha(bytes.data(),bytes.size());
}
struct StagedSetupCleanup {
    std::filesystem::path folder;
    std::filesystem::path file;
    bool defer{};
    ~StagedSetupCleanup() {
        if(defer) {
            // The installer could still be running: do not kill it or unlink
            // the executing image. Queue file before directory for next boot.
            (void)MoveFileExW(file.c_str(),nullptr,MOVEFILE_DELAY_UNTIL_REBOOT);
            (void)MoveFileExW(folder.c_str(),nullptr,MOVEFILE_DELAY_UNTIL_REBOOT);
        }else{
            std::error_code ec;
            std::filesystem::remove(file,ec);
            ec.clear();
            std::filesystem::remove(folder,ec);
        }
    }
};
bool stage_setup(const std::filesystem::path& destination,
                 const std::vector<unsigned char>& bytes) noexcept {
    if(bytes.empty() || !matches_sha(bytes.data(),bytes.size()))return false;
    // CREATE_NEW prevents quietly overwriting pre-existing executable content.
    const auto handle=CreateFileW(destination.c_str(),GENERIC_WRITE,0,nullptr,
        CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(handle==INVALID_HANDLE_VALUE)return false;
    DWORD written{};
    const bool ok=WriteFile(handle,bytes.data(),static_cast<DWORD>(bytes.size()),&written,nullptr)!=FALSE
        && written==bytes.size() && FlushFileBuffers(handle)!=FALSE;
    CloseHandle(handle);
    return ok;
}
} // namespace

bool pawnio_device_ready() noexcept {
    const auto device=CreateFileW(L"\\\\?\\GLOBALROOT\\Device\\PawnIO",
        GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,
        nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(device==INVALID_HANDLE_VALUE)return false;
    CloseHandle(device);
    return true;
}
bool pawnio_bundle_verified(const std::filesystem::path& launcher) noexcept {
    try {
        const auto payload=installer_bytes(launcher);
        return !payload.empty() && matches_sha(payload.data(),payload.size());
    }catch(...){return false;}
}
// FluxTop.PawnIO.Verification.v48.3: tests and production use one policy.
void maybe_install_embedded_pawnio(const std::filesystem::path& launcher,
                                  const std::filesystem::path& directory) noexcept {
    try {
        // Installation has been approved by the user at the launcher level.
        // Independently verify elevation before invoking a driver installer.
        HANDLE token{};
        bool elevated=false;
        if(OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token)) {
            TOKEN_ELEVATION elevation{};
            DWORD returned{};
            elevated=GetTokenInformation(token,TokenElevation,&elevation,
                sizeof(elevation),&returned)!=FALSE && elevation.TokenIsElevated!=0;
            CloseHandle(token);
        }
        const bool already_ready=pawnio_device_ready();
        if(already_ready) {
            diagnostics::write(diagnostics::Level::info,"pawnio.install",
                "Existing PawnIO device available; skipped");
            return;
        }
        if(pawnio_install_policy(false,true,elevated,false)==
           PawnioAction::skip_without_elevation) {
            diagnostics::write(diagnostics::Level::warning,"pawnio.install",
                "Driver installation skipped without elevated process token");
            return;
        }
        const auto bytes=installer_bytes(launcher);
        const bool verified=!bytes.empty() && matches_sha(bytes.data(),bytes.size());
        if(pawnio_install_policy(false,true,elevated,verified)!=PawnioAction::install_embedded) {
            diagnostics::write(diagnostics::Level::warning,"pawnio.install",
                "Missing or untrusted bundled PawnIO setup; optional DTS unavailable");
            return;
        }
        // The user approved the FluxTop installation. Driver installation
        // is never executed from user-writable FluxTop data or temporary paths.
        (void)directory; // Historical parameter retained for caller compatibility.
        const auto folder=secure_setup_directory();
        if(folder.empty()) {
            diagnostics::write(diagnostics::Level::warning,"pawnio.install",
                "Cannot create privileged-only PawnIO staging folder");
            return;
        }
        const auto file=folder/L"PawnIO_setup.exe";
        StagedSetupCleanup cleanup{folder,file,false};
        if(!stage_setup(file,bytes) || !staged_file_matches_sha(file) ||
            !verify_authenticode(file)){
            diagnostics::write(diagnostics::Level::warning,"pawnio.install",
                "Bundled installer failed SHA-256, write or Authenticode verification");
            return;
        }
        SHELLEXECUTEINFOW process{};
        process.cbSize=sizeof(process);
        process.fMask=SEE_MASK_NOCLOSEPROCESS|SEE_MASK_FLAG_NO_UI;
        process.lpVerb=L"open";
        process.lpFile=file.c_str();
        process.lpParameters=L"-install -silent";
        process.lpDirectory=folder.c_str();
        process.nShow=SW_HIDE;
        if(!ShellExecuteExW(&process)){
            diagnostics::write(diagnostics::Level::warning,"pawnio.install",
                "Unable to start bundled official PawnIO installer");
            return;
        }
        if(!process.hProcess){
            // Do not delete an executable whose child lifetime is unknown.
            cleanup.defer=true;
            diagnostics::write(diagnostics::Level::warning,"pawnio.install",
                "PawnIO installer started without a waitable process handle");
            return;
        }
        const DWORD wait=WaitForSingleObject(process.hProcess,90000);
        DWORD exit_code=ERROR_GEN_FAILURE;
        if(wait==WAIT_OBJECT_0)
            (void)GetExitCodeProcess(process.hProcess,&exit_code);
        CloseHandle(process.hProcess);
        if(wait!=WAIT_OBJECT_0){
            // Keep the protected staging directory until the child exits.
            cleanup.defer=true;
            diagnostics::write(diagnostics::Level::warning,"pawnio.install",
                "PawnIO installer exceeded 90 seconds; cleanup deferred, FluxTop continues");
            return;
        }
        if(exit_code==ERROR_SUCCESS_REBOOT_REQUIRED){
            diagnostics::write(diagnostics::Level::warning,"pawnio.install",
                "PawnIO installed; Windows restart required");
            return;
        }
        if(exit_code!=ERROR_SUCCESS){
            diagnostics::write(diagnostics::Level::warning,"pawnio.install",
                "Official PawnIO installer failed; optional DTS unavailable; exit="+
                std::to_string(exit_code));
            return;
        }
        diagnostics::write(pawnio_device_ready()?diagnostics::Level::info:diagnostics::Level::warning,
            "pawnio.install",pawnio_device_ready()?
            "Official PawnIO installer completed; device ready":
            "Official PawnIO installer completed; device not yet ready");
    }catch(...){
        diagnostics::write(diagnostics::Level::warning,"pawnio.install",
            "Optional PawnIO installation failed; FluxTop continues");
    }
}
} // namespace fluxtop