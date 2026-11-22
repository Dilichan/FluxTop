#include "fluxtop/gpu_process_memory.hpp"
#include "fluxtop/model.hpp"
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>

int main() {
    try {
        using namespace fluxtop;
        constexpr std::uint64_t mib=1048576ULL;
        const auto game=gpu_process_memory_from_pdh(4565.1 * static_cast<double>(mib),8ULL*1024*mib);
        if(!game || *game<4565*mib || *game>4566*mib)
            throw std::runtime_error("verified Windows game local usage not accepted");
        if(gpu_process_memory_from_pdh(0.0,8ULL*1024*mib)!=0ULL)
            throw std::runtime_error("confirmed zero lost");
        if(gpu_process_memory_from_pdh(-1.0,8ULL*1024*mib) ||
            gpu_process_memory_from_pdh(std::numeric_limits<double>::infinity(),8ULL*1024*mib) ||
            gpu_process_memory_from_pdh(std::numeric_limits<double>::quiet_NaN(),8ULL*1024*mib) ||
            gpu_process_memory_from_pdh(9000.0*mib,8ULL*1024*mib))
            throw std::runtime_error("invalid GPU memory must be unknown");
        if(gpu_process_memory_from_pdh(1234.0,std::nullopt)!=1234ULL)
            throw std::runtime_error("unknown capacity must not synthesize a limit");
        if(gpu_process_memory_consistent(*game,std::nullopt,8ULL*1024*mib)==false)
            throw std::runtime_error("verified local sample rejected without adapter-wide value");
        if(gpu_instance_pid(L"pid_43392_luid_0x00000002_0x489b0d8f_phys_0")!=43392 ||
            gpu_instance_pid(L"pid_43392") || gpu_instance_pid(L"pid_invalid_luid_0"))
            throw std::runtime_error("Windows PDH instance PID parsing changed");
        std::cout<<"GPU PDH local memory fallback contracts passed\n";
        return 0;
    } catch(const std::exception& e) {
        std::cerr<<e.what()<<'\n';return 1;
    }
}