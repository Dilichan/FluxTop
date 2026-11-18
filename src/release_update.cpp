#include "fluxtop/release_update.hpp"
#include "fluxtop/windows.hpp"
#include <winhttp.h>
#include <bcrypt.h>
#include <roapi.h>
#include <windows.data.json.h>
#include <wrl/client.h>
#include <wrl/wrappers/corewrappers.h>
#include <shellapi.h>
#include <array>
#include <chrono>
#include <fstream>
#include <cmath>

namespace fluxtop {
namespace {
using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Wrappers::HString;
using Microsoft::WRL::Wrappers::HStringReference;
using namespace ABI::Windows::Data::Json;
void require(HRESULT result) { if (FAILED(result)) throw std::runtime_error("Invalid release metadata"); }
std::optional<std::array<unsigned,5>> version(std::wstring_view text) {
    if (text.starts_with(L"v")) text.remove_prefix(1);
    std::array<unsigned,5> result{};
    const auto number=[&](unsigned& value) {
        bool any=false;
        while (!text.empty() && text.front()>=L'0' && text.front()<=L'9') {
            any=true; value=value*10+static_cast<unsigned>(text.front()-L'0'); text.remove_prefix(1);
            if (value>999999) return false;
        }
        return any;
    };
    for (unsigned i=0;i<3;++i) {
        if (!number(result[i])) return std::nullopt;
        if (i<2) { if (!text.starts_with(L".")) return std::nullopt; text.remove_prefix(1); }
    }
    if (text.empty()) { result[3]=1; return result; }
    if (!text.starts_with(L"-preview.")) return std::nullopt;
    text.remove_prefix(9);
    if (!number(result[4]) || !text.empty()) return std::nullopt;
    return result;
}
UINT32 size(IJsonArray* array) {
    ComPtr<ABI::Windows::Foundation::Collections::IVector<IJsonValue*>> vector;
    require(array->QueryInterface(IID_PPV_ARGS(&vector)));
    UINT32 result{}; require(vector->get_Size(&result)); return result;
}
std::wstring string(IJsonObject* object,const wchar_t* key) {
    HString value; require(object->GetNamedString(HStringReference(key).Get(),value.GetAddressOf()));
    UINT32 length{}; const auto* text=value.GetRawBuffer(&length); return {text,length};
}
struct Http {
    HINTERNET value{};
    explicit Http(HINTERNET handle):value(handle) { if (!value) throw windows_error("GitHub update connection"); }
    ~Http(){ WinHttpCloseHandle(value); }
};
std::vector<unsigned char> get(const wchar_t* host,const std::wstring& path,std::size_t limit) {
    Http session(WinHttpOpen(L"FluxTop-Updater/0.2",WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0));
    if (!WinHttpSetTimeouts(session.value,3000,3000,3000,5000)) throw windows_error("Update timeouts");
    Http connection(WinHttpConnect(session.value,host,INTERNET_DEFAULT_HTTPS_PORT,0));
    Http request(WinHttpOpenRequest(connection.value,L"GET",path.c_str(),nullptr,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,WINHTTP_FLAG_SECURE));
    DWORD policy=WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
    if (!WinHttpSetOption(request.value,WINHTTP_OPTION_REDIRECT_POLICY,&policy,sizeof(policy))) throw windows_error("Update redirect policy");
    if (!WinHttpSendRequest(request.value,L"Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28\r\n",static_cast<DWORD>(-1),nullptr,0,0,0)
        || !WinHttpReceiveResponse(request.value,nullptr)) throw windows_error("GitHub update request");
    DWORD code{}, length=sizeof(code);
    if (!WinHttpQueryHeaders(request.value,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,nullptr,&code,&length,nullptr) || code!=200)
        throw std::runtime_error("GitHub update HTTP status "+std::to_string(code));
    std::vector<unsigned char> bytes;
    std::array<unsigned char,65536> chunk{};
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(45);
    for (;;) {
        DWORD read{};
        if (!WinHttpReadData(request.value,chunk.data(),static_cast<DWORD>(chunk.size()),&read)) throw windows_error("Read GitHub update");
        if (!read) return bytes;
        if (bytes.size()+read>limit || std::chrono::steady_clock::now()>deadline) throw std::runtime_error("Update transfer limit exceeded");
        bytes.insert(bytes.end(),chunk.begin(),chunk.begin()+read);
    }
}
bool launch(const std::filesystem::path& path,bool tray) {
    SHELLEXECUTEINFOW start{}; start.cbSize=sizeof(start); start.lpFile=path.c_str();
    start.lpParameters=tray ? L"--tray" : nullptr; start.nShow=SW_SHOWNORMAL;
    return ShellExecuteExW(&start)!=FALSE;
}
std::wstring cached_tag(const std::filesystem::path& path) {
    const auto module=LoadLibraryExW(path.c_str(),nullptr,LOAD_LIBRARY_AS_DATAFILE|LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if (!module) return {};
    wchar_t value[128]{}; const auto length=LoadStringW(module,203,value,static_cast<int>(std::size(value))); FreeLibrary(module);
    return length>0 ? std::wstring(value,static_cast<std::size_t>(length)) : std::wstring{};
}
}
bool newer_release(std::wstring_view candidate,std::wstring_view current) {
    const auto a=version(candidate),b=version(current);
    return a && b && *a>*b;
}
std::optional<Release> release_from_json(std::wstring_view json,std::wstring_view current) {
    const auto init=RoInitialize(RO_INIT_MULTITHREADED);
    if (FAILED(init) && init!=RPC_E_CHANGED_MODE) throw std::runtime_error("Cannot initialize release parser");
    struct Cleanup { bool initialized; ~Cleanup(){ if(initialized) RoUninitialize(); } } cleanup{SUCCEEDED(init)};
    ComPtr<IJsonArrayStatics> factory;
    require(RoGetActivationFactory(HStringReference(RuntimeClass_Windows_Data_Json_JsonArray).Get(),IID_PPV_ARGS(&factory)));
    HString text; require(text.Set(json.data(),static_cast<UINT32>(json.size())));
    ComPtr<IJsonArray> releases; require(factory->Parse(text.Get(),&releases));
    std::optional<Release> selected;
    for (UINT32 i=0;i<size(releases.Get());++i) {
        try {
            ComPtr<IJsonObject> object; require(releases->GetObjectAt(i,&object));
            boolean draft{}; require(object->GetNamedBoolean(HStringReference(L"draft").Get(),&draft));
            const auto tag=string(object.Get(),L"tag_name");
            if (draft || !newer_release(tag,selected ? selected->tag : current)) continue;
            ComPtr<IJsonArray> assets; require(object->GetNamedArray(HStringReference(L"assets").Get(),&assets));
            for (UINT32 j=0;j<size(assets.Get());++j) {
                ComPtr<IJsonObject> asset; require(assets->GetObjectAt(j,&asset));
                if (string(asset.Get(),L"name")!=L"FluxTop.exe" || string(asset.Get(),L"state")!=L"uploaded") continue;
                const auto digest=string(asset.Get(),L"digest");
                if (!digest.starts_with(L"sha256:") || digest.size()!=71) continue;
                std::string hash;
                for (const auto c : digest.substr(7)) {
                    if (!((c>=L'0'&&c<=L'9')||(c>=L'a'&&c<=L'f'))) { hash.clear(); break; }
                    hash.push_back(static_cast<char>(c));
                }
                double bytes{}; require(asset->GetNamedNumber(HStringReference(L"size").Get(),&bytes));
                if (hash.size()!=64 || !std::isfinite(bytes) || bytes!=std::floor(bytes) || bytes<1024 || bytes>64*1024*1024) continue;
                selected=Release{tag,hash,static_cast<std::size_t>(bytes)}; break;
            }
        } catch(const std::exception&) { /* Ignore malformed/incomplete release entries. */ }
    }
    return selected;
}
std::string release_sha256(const std::vector<unsigned char>& bytes) {
    BCRYPT_ALG_HANDLE algorithm{}; BCRYPT_HASH_HANDLE hash{};
    struct Cleanup { BCRYPT_ALG_HANDLE& algorithm; BCRYPT_HASH_HANDLE& hash;
        ~Cleanup(){ if(hash) BCryptDestroyHash(hash); if(algorithm) BCryptCloseAlgorithmProvider(algorithm,0); } } cleanup{algorithm,hash};
    if (BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0
        || BCryptCreateHash(algorithm,&hash,nullptr,0,nullptr,0,0)<0
        || BCryptHashData(hash,const_cast<PUCHAR>(bytes.data()),static_cast<ULONG>(bytes.size()),0)<0)
        throw std::runtime_error("Cannot hash release download");
    std::array<unsigned char,32> digest{};
    if (BCryptFinishHash(hash,digest.data(),static_cast<ULONG>(digest.size()),0)<0) throw std::runtime_error("Cannot finish release hash");
    constexpr char hex[]="0123456789abcdef"; std::string result;
    for (const auto byte:digest) { result+=hex[byte>>4]; result+=hex[byte&15]; } return result;
}
bool activate_release(const std::filesystem::path& directory,const std::vector<unsigned char>& bytes,
    const Release& release,const std::function<bool(const std::filesystem::path&)>& start) {
    if (bytes.size()!=release.size || bytes.size()<1024 || release_sha256(bytes)!=release.digest
        || bytes[0]!='M' || bytes[1]!='Z') throw std::runtime_error("Release size, SHA-256 or executable header mismatch");
    const auto stable=directory/L"FluxTop.exe";
    const auto temporary=directory/(L"FluxTop.update."+std::to_wstring(GetCurrentProcessId())+L".exe");
    struct Cleanup { std::filesystem::path path; ~Cleanup(){ DeleteFileW(path.c_str()); } } cleanup{temporary};
    {
        Handle file(CreateFileW(temporary.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,0,nullptr));
        DWORD written{};
        if (!file.valid() || !WriteFile(file.get(),bytes.data(),static_cast<DWORD>(bytes.size()),&written,nullptr)
            || written!=bytes.size() || !FlushFileBuffers(file.get())) throw windows_error("Save downloaded update");
    }
    if (cached_tag(temporary)!=release.tag) throw std::runtime_error("Release tag does not match executable");
    const auto previous=directory/L"FluxTop.previous.exe";
    const bool existed=std::filesystem::exists(stable);
    if (existed && !MoveFileExW(stable.c_str(),previous.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))
        throw windows_error("Keep previous launcher");
    if (!MoveFileExW(temporary.c_str(),stable.c_str(),MOVEFILE_WRITE_THROUGH)) {
        if (existed) MoveFileExW(previous.c_str(),stable.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH);
        throw std::runtime_error("Cannot publish update; previous launcher retained");
    }
    bool started=false;
    try { started=start(stable); } catch(...) { started=false; }
    if (!started) {
        DeleteFileW(stable.c_str());
        if (existed && !MoveFileExW(previous.c_str(),stable.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))
            throw windows_error("Restore previous launcher");
        throw std::runtime_error("Cannot activate update; previous launcher retained");
    }
    return true;
}
std::optional<Release> find_release_update(std::wstring_view current) {
    const auto json_bytes=get(L"api.github.com",L"/repos/Dilichan/FluxTop/releases?per_page=20",1024*1024);
    const auto length=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,
        reinterpret_cast<const char*>(json_bytes.data()),static_cast<int>(json_bytes.size()),nullptr,0);
    if (length<=0) throw std::runtime_error("Invalid GitHub response encoding");
    std::wstring json(static_cast<std::size_t>(length),L'\0');
    MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,
        reinterpret_cast<const char*>(json_bytes.data()),static_cast<int>(json_bytes.size()),json.data(),length);
    return release_from_json(json,current);
}
std::vector<unsigned char> download_release_update(const Release& release) {
    if (!version(release.tag)) throw std::runtime_error("Invalid release version");
    return get(L"github.com",L"/Dilichan/FluxTop/releases/download/"+release.tag+L"/FluxTop.exe",64*1024*1024);
}
bool try_release_update(const std::filesystem::path& directory,const std::filesystem::path& self,std::wstring_view current,bool tray) noexcept {
    try {
        const auto stable=directory/L"FluxTop.exe";
        std::error_code error;
        if (!std::filesystem::equivalent(self,stable,error) && newer_release(cached_tag(stable),current)) return launch(stable,tray);
        const auto release=find_release_update(current);
        if (!release) return false;
        const auto bytes=download_release_update(*release);
        return activate_release(directory,bytes,*release,[tray](const auto& path){ return launch(path,tray); });
    } catch(const std::exception& error) {
        try { std::ofstream(directory/L"update.log",std::ios::trunc)<<error.what()<<'\n'; } catch(...) {}
        return false;
    }
}
}
