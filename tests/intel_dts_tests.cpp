#include "fluxtop/intel_dts.hpp"
#include <iostream>
int main(){
    const auto decode=fluxtop::intel_dts_decode;
    constexpr std::uint64_t target=100ULL<<16;
    constexpr std::uint64_t valid=1ULL<<31;
    int failures=0;
    auto require=[&](bool test,const char* what){if(!test){std::cerr<<what<<'\n';++failures;}};
    require(decode(target,valid|(33ULL<<16))==67.0,"package 67 C");
    require(!decode(target,33ULL<<16),"invalid bit must be rejected");
    require(!decode(0,valid),"missing TjMax must be rejected");
    require(!decode(50ULL<<16,valid),"implausible TjMax rejected");
    require(!decode(target,valid|(110ULL<<16)),"distance greater than TjMax rejected");
    require(decode(105ULL<<16,valid|(7ULL<<16))==98.0,"non-100 TjMax respected");
    return failures?1:0;
}