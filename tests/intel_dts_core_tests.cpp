#include "fluxtop/intel_dts.hpp"
#include <iostream>
#include <cmath>
int main(){
    using namespace fluxtop;
    int failures=0;
    auto check=[&](bool ok,const char* what){if(!ok){std::cerr<<what<<'\n';++failures;}};
    IntelDtsSnapshot sample;
    sample.logical_processors=5;
    sample.package_c=68.0;
    sample.cores={
        {0,0,0,true,60.0,2,false},
        {0,2,1,true,70.0,2,false},
        {0,4,2,false,55.0,1,true}
    };
    intel_dts_finalize(sample);
    check(sample.physical_cores==3 && sample.logical_processors==5,"P/E topology and HT sibling counts");
    check(sample.available_cores==3 && !sample.partial,"full coverage");
    check(sample.core_max_c==70.0 && sample.core_average_c && std::abs(*sample.core_average_c-185.0/3.0)<0.00001,
        "aggregate max and average");
    sample.cores[1].celsius.reset();
    intel_dts_finalize(sample);
    check(sample.available_cores==2 && sample.partial,"partial sensor coverage");
    check(sample.core_max_c==60.0 && sample.core_average_c==57.5,"missing not zero-filled");
    sample.package_c.reset();
    for(auto& core:sample.cores)core.celsius.reset();
    intel_dts_finalize(sample);
    check(!sample.core_max_c && !sample.core_average_c && sample.partial,"expired values never retained");
    sample.cores[0].celsius=60.0;
    sample.cores[1].celsius=60.0;
    intel_dts_finalize(sample);
    check(sample.available_cores==2,"equal temperatures do not mean duplicate sensor");
    check(sample.cores[2].sensor_independent_unverified && !sample.cores[0].sensor_independent_unverified,
        "E-core independence qualification preserved");
    return failures?1:0;
}