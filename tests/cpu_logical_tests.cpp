#include "fluxtop/cpu_logical.hpp"
#include <cmath>
#include <iostream>
#include <limits>
int main() {
    using fluxtop::cpu_logical_percent;
    unsigned fail=0;
    const auto check=[&](bool okay,const char* label){
        if(!okay){std::cerr<<label<<'\n';++fail;}
    };
    check(!cpu_logical_percent(std::numeric_limits<double>::quiet_NaN()),"nan");
    check(!cpu_logical_percent(std::numeric_limits<double>::infinity()),"infinity");
    check(!cpu_logical_percent(-10.0),"negative");
    check(!cpu_logical_percent(150.0),"overflow");
    check(cpu_logical_percent(0.0)==0.0,"true idle zero");
    check(cpu_logical_percent(45.5)==45.5,"45.5 percent");
    check(cpu_logical_percent(100.0)==100.0,"full load");
    check(cpu_logical_percent(-0.001)==0.0,"PDH rounding below zero");
    check(cpu_logical_percent(100.001)==100.0,"PDH rounding above 100");
    return fail?1:0;
}