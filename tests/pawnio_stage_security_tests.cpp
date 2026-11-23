#include <filesystem>
#include <fstream>
#include <iterator>
#include <iostream>
#include <stdexcept>
#include <string>
namespace {
void require(bool ok,const char* msg) { if(!ok) throw std::runtime_error(msg); }
std::string read(const std::filesystem::path& file) {
    std::ifstream in(file,std::ios::binary);
    require(static_cast<bool>(in),"Missing PawnIO source fixture");
    return {std::istreambuf_iterator<char>(in),std::istreambuf_iterator<char>()};
}
}
int main(int argc,char** argv) {
    try {
        require(argc==2,"Expected repository path");
        const auto root=std::filesystem::u8path(argv[1]);
        const auto cpp=read(root/"src/pawnio_bundle.cpp");
        require(cpp.find("FluxTop.PawnIO.Security.v48.4")!=std::string::npos,
            "Secure staging marker missing");
        require(cpp.find("FOLDERID_ProgramFiles")!=std::string::npos,
            "Driver installer must be staged in a privileged directory");
        require(cpp.find("D:P(A;;FA;;;SY)(A;;FA;;;BA)")!=std::string::npos,
            "Privileged-only DACL missing");
        require(cpp.find("BCryptGenRandom")!=std::string::npos,
            "Unpredictable directory name missing");
        require(cpp.find("staged_file_matches_sha(file)")!=std::string::npos,
            "Staged executable hash must be verified");
        require(cpp.find("verify_authenticode(file)")!=std::string::npos,
            "Staged executable signature must be verified");
        require(cpp.find("process.lpDirectory=folder.c_str()")!=std::string::npos,
            "Driver installer working directory must be protected");
        require(cpp.find("process.lpParameters=L\"-install -silent\"")!=std::string::npos,
            "Unexpected silent installation arguments");
        require(cpp.find("cleanup.defer=true")!=std::string::npos,
            "In-progress driver installer must survive timeout cleanup");
        require(cpp.find("directory/(L\".pawnio-setup-\"")==std::string::npos,
            "User-writable staging regression");
        std::cout<<"PawnIO installer staging security contract passed\n";
        return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}