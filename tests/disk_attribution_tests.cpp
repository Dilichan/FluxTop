#include "fluxtop/disk_attribution.hpp"
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <vector>
int main() {
    using namespace fluxtop;
    const std::vector<PhysicalVolumeExtent> extents{{0,1048576,1000000,L'C'},
        {0,3145728,1000000,L'D'},{1,0,1000000,L'E'}};
    const auto expect=[](bool ok,const char* msg){if(!ok)throw std::runtime_error(msg);};
    try {
        expect(disk_io_volume_for(extents,0,1048576,4096)==L'C',"physical offset resolves C");
        expect(disk_io_volume_for(extents,0,3145728,4096)==L'D',"second partition resolves D");
        expect(!disk_io_volume_for(extents,0,2048000,200000),"partition crossing is ambiguous");
        expect(!disk_io_volume_for(extents,3,0,4096),"unmapped disk remains unknown");
        expect(!disk_io_volume_for(extents,0,UINT64_MAX-8,32),"offset overflow rejected");
        expect(!disk_io_volume_for(extents,0,1048576,0),"zero byte I/O rejected");
        expect(!disk_io_fully_attributed(79,100,false),"partial observed bytes cannot certify zeros");
        expect(!disk_io_fully_attributed(80,100,false),"80 percent is not a storage standard");
        expect(disk_io_fully_attributed(100,100,false),"full observed coverage certifies zeros");
        expect(!disk_io_fully_attributed(100,100,true),"lost ETW events invalidate samples");
        expect(!disk_io_fully_attributed(120,100,false),"impossible coverage fails closed");
        expect(disk_io_publishable_bytes(1,100,false),
            "one positively matched transfer remains a valid lower bound");
        expect(!disk_io_publishable_bytes(0,100,false),
            "no matched physical bytes cannot be published");
        expect(!disk_io_publishable_bytes(100,100,true),
            "lost ETW events forbid publishing even positive matches");
        expect(!disk_io_publishable_bytes(101,100,false),
            "impossible bytes cannot be published");
        const auto p=disk_io_pressure(70.0,200,1000);
        expect(p && std::abs(*p-14.0)<1e-9,"per-app pressure is physical-byte share times busy percent");
        expect(!disk_io_pressure(std::nullopt,200,1000),"unknown activity is not zero");
        expect(!disk_io_pressure(50.0,100,0),"unknown total bytes is not zero");
        expect(!disk_io_pressure(40.0,200,100),"impossible per-app physical share rejected");
        std::cout<<"ETW disk attribution invariants passed\n";
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
    return 0;
}
