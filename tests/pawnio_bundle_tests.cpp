#include "fluxtop/pawnio_bundle.hpp"
#include <windows.h>
#include <wintrust.h>
#include <softpub.h>
#include <wincrypt.h>
#include <filesystem>
#include <iostream>
int main(int argc,char** argv) {
    if(argc<2){std::cerr<<"Expected launcher file argument\n";return 1;}
    const std::filesystem::path launcher=std::filesystem::u8path(argv[1]);
    if(!std::filesystem::is_regular_file(launcher)){
        std::cerr<<"Launcher missing\n";return 1;
    }
    // No driver installation, service changes or UAC: test only embedded bytes.
    if(!fluxtop::pawnio_bundle_verified(launcher)){
        std::cerr<<"Offline signed-setup payload is missing or does not match pinned SHA-256\n";
        return 1;
    }
    std::cout<<"Offline PawnIO resource hash matches pinned official v2.2.0\n";
    return 0;
}