#include "fluxtop/release_update.hpp"
#include "fluxtop/windows.hpp"
#include <fstream>
#include <iostream>
#include <iterator>
#include <chrono>

namespace {
void check(bool condition,const char* message){ if(!condition) throw std::runtime_error(message); }
std::vector<unsigned char> read(const std::filesystem::path& path){ std::ifstream input(path,std::ios::binary); return {std::istreambuf_iterator<char>(input),{}}; }
template<class F> void rejects(F action){ bool failed=false; try{ action(); }catch(const std::exception&){ failed=true; } check(failed,"invalid update must fail"); }
}
int wmain(int argc,wchar_t** argv) {
    try {
        check(argc==4,"launcher, fixture directory and release tag required");
        using namespace fluxtop;
        check(newer_release(L"v0.2.0-preview.2",L"v0.2.0-preview.1"),"preview ordering");
        check(newer_release(L"v0.2.0",L"v0.2.0-preview.99"),"stable follows preview");
        check(newer_release(L"v0.10.0",L"v0.9.9"),"numeric not lexical ordering");
        check(!newer_release(L"v0.1.0",L"v0.2.0")&&!newer_release(L"../evil",L"v0.2.0"),"no downgrade or URL path injection");
        check(release_sha256({'a','b','c'})=="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad","SHA-256 known vector");
        const auto asset=LR"({"name":"FluxTop.exe","state":"uploaded","size":4096,"digest":"sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"})";
        const std::wstring json=L"[{\"tag_name\":\"v9.0.0\",\"draft\":true,\"assets\":["+std::wstring(asset)
            +L"]},{\"tag_name\":\"v0.3.0-preview.1\",\"draft\":false,\"assets\":["+asset
            +L"]},{\"tag_name\":\"v0.4.0\",\"draft\":false,\"assets\":[]}]";
        const auto release=release_from_json(json,L"v0.2.0-preview.1");
        check(release && release->tag==L"v0.3.0-preview.1"&&release->size==4096,"draft/incomplete skipped; valid newer preview selected");
        check(!release_from_json(json,L"v1.0.0"),"no downgrade from GitHub metadata");
        rejects([]{ release_from_json(L"broken",L"v0.2.0"); });
        const auto directory=std::filesystem::path(argv[2])/(L"release-test-"+std::to_wstring(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(directory);
        const auto stable=directory/L"FluxTop.exe";
        { std::ofstream(stable,std::ios::binary)<<"previous version"; }
        const auto old=read(stable),payload=read(argv[1]);
        Release candidate{argv[3],release_sha256(payload),payload.size()};
        auto wrong=candidate; wrong.digest=std::string(64,'0');
        rejects([&]{ activate_release(directory,payload,wrong,[](const auto&){return true;}); });
        check(read(stable)==old,"bad checksum cannot replace previous launcher");
        wrong=candidate; wrong.tag=L"v100.0.0";
        rejects([&]{ activate_release(directory,payload,wrong,[](const auto&){return true;}); });
        check(read(stable)==old,"mismatched embedded version cannot replace launcher");
        rejects([&]{ activate_release(directory,payload,candidate,[](const auto&){return false;}); });
        check(read(stable)==old,"failed startup restores previous launcher");
        check(activate_release(directory,payload,candidate,[](const auto& path){return std::filesystem::exists(path);}),"verified update activates");
        check(read(stable)==payload&&read(directory/L"FluxTop.previous.exe")==old,"new file and backup preserved");
        {
            Handle lock(CreateFileW(stable.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr));
            check(lock.valid(),"lock fixture");
            rejects([&]{ activate_release(directory,payload,candidate,[](const auto&){return true;}); });
        }
        check(read(stable)==payload,"locked file leaves current launcher intact");
        std::cout<<"Release parsing, versions, SHA-256, activation, rollback and locked-file checks passed\n";
        return 0;
    }catch(const std::exception& error){ std::cerr<<error.what()<<'\n'; return 1; }
}
