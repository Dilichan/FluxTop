#include "fluxtop/pawnio_policy.hpp"
#include "fluxtop/pawnio_bundle.hpp"
#include <filesystem>
#include <fstream>
#include <iterator>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
using namespace fluxtop;
namespace {
void check(bool ok,const char* why) { if(!ok) throw std::runtime_error(why); }
std::string read(const std::filesystem::path& file) {
    std::ifstream in(file,std::ios::binary);
    check(static_cast<bool>(in),"source fixture missing");
    return {std::istreambuf_iterator<char>(in),std::istreambuf_iterator<char>()};
}
}
int main(int argc,char** argv) {
    try {
        check(argc==3,"usage: policy_test LAUNCHER SOURCE_ROOT");
        const auto launcher=std::filesystem::u8path(argv[1]);
        const auto root=std::filesystem::u8path(argv[2]);
        // Eight truth-table combinations: installed driver always wins and no
        // invalid/no-consent/non-elevated case can start driver installation.
        for(int consent=0;consent!=2;++consent)
            for(int elevation=0;elevation!=2;++elevation)
                for(int valid=0;valid!=2;++valid) {
                    check(pawnio_install_policy(true,consent,elevation,valid)==
                        PawnioAction::reuse_existing,"existing driver would be reinstalled");
                    const auto decision=pawnio_install_policy(false,consent,elevation,valid);
                    const auto expected=!consent?PawnioAction::skip_without_consent:
                        !elevation?PawnioAction::skip_without_elevation:
                        !valid?PawnioAction::skip_invalid_bundle:PawnioAction::install_embedded;
                    check(decision==expected,"driver installation policy mismatch");
                }
        check(pawnio_bundle_verified(launcher),"bundled installer missing or SHA-256 invalid");
        // The driver is shared and is never removed by FluxTop's uninstaller.
        const auto uninstall=read(root/"src/uninstaller.cpp");
        const auto cleanup=read(root/"src/uninstall_cleanup.cpp");
        for(const auto& source:{uninstall,cleanup}){
            check(source.find("PawnIO")==std::string::npos &&
                  source.find("pawnio")==std::string::npos,
                  "uninstaller must not touch PawnIO");
            check(source.find("DeleteService(")==std::string::npos &&
                  source.find("DeleteServiceW(")==std::string::npos,
                  "uninstaller must not delete services");
        }
        const auto installation=read(root/"src/installation.cpp");
        const auto fixture_guard=installation.find("if (!register_windows) return;");
        const auto driver_call=installation.find("maybe_install_embedded_pawnio(installed,data_directory);");
        check(fixture_guard!=std::string::npos && driver_call>fixture_guard,
            "fixture path might attempt driver installation");
        std::cout<<"PawnIO install policy: 16 synthetic cases passed; "
                 <<"real hardware and Windows state untouched\n";
        return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}