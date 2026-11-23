#include "fluxtop/process_enumerator.hpp"
#include <algorithm>
#include <cstring>
#include <iostream>
#include <stdexcept>
using namespace fluxtop;
namespace {
void need(bool yes,const char* message){if(!yes)throw std::runtime_error(message);}
enum class Mode {ready,unsupported,denied,corrupt,grow,oversize,missing_size};
Mode mode=Mode::ready;unsigned calls{},fallbacks{};
std::vector<std::byte> fixture(std::uint32_t pid=12){
    std::vector<std::byte> bytes(sizeof(BasicProcessRecord)+16);
    BasicProcessRecord entry{};entry.pid=reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(pid));entry.parent=reinterpret_cast<HANDLE>(4);entry.sequence=101;
    entry.name_bytes=8;entry.maximum_name_bytes=10;entry.name=reinterpret_cast<PWSTR>(bytes.data()+sizeof(entry));
    std::memcpy(bytes.data(),&entry,sizeof(entry));std::memcpy(bytes.data()+sizeof(entry),L"test",8);return bytes;
}
LONG NTAPI query(ULONG kind,void* target,ULONG capacity,ULONG* returned){
    need(kind==252,"only documented basic process information");++calls;
    if(mode==Mode::unsupported)return static_cast<LONG>(0xc0000003u);
    if(mode==Mode::denied)return static_cast<LONG>(0xc0000022u);
    if(mode==Mode::oversize){*returned=0xffffffffu;return static_cast<LONG>(0xc0000004u);}
    if(mode==Mode::grow && capacity<131072){*returned=131072;return static_cast<LONG>(0xc0000004u);}
    auto bytes=fixture();auto* entry=reinterpret_cast<BasicProcessRecord*>(bytes.data());entry->name=reinterpret_cast<PWSTR>(static_cast<std::byte*>(target)+sizeof(*entry));
    if(mode==Mode::corrupt)entry->next=1;
    need(capacity>=bytes.size(),"query capacity");std::memcpy(target,bytes.data(),bytes.size());*returned=mode==Mode::missing_size?0:static_cast<ULONG>(bytes.size());return 0;
}
std::vector<ProcessEntry> fallback(){++fallbacks;return {{99,4,"fallback"}};}
void reset(Mode value){calls=0;fallbacks=0;mode=value;}
void parser(){
    auto bytes=fixture();std::vector<ProcessEntry> rows;
    need(parse_basic_processes(bytes,rows) && rows.size()==1 && rows[0].pid==12 && rows[0].parent_pid==4 && rows[0].name=="test","complete identity and image name");
    auto* p=reinterpret_cast<BasicProcessRecord*>(bytes.data());
    p->name_bytes=3;need(!parse_basic_processes(bytes,rows) && rows.size()==1,"odd UTF-16 length does not replace complete output");p->name_bytes=8;
    auto* name=p->name;p->name=reinterpret_cast<PWSTR>(1);need(!parse_basic_processes(bytes,rows),"foreign name pointer");p->name=name;
    p->next=8;need(!parse_basic_processes(bytes,rows),"overlapping record");p->next=0;
    need(!parse_basic_processes({bytes.data(),sizeof(BasicProcessRecord)-1},rows),"truncated header");
    p->name_bytes=12;p->maximum_name_bytes=8;need(!parse_basic_processes(bytes,rows),"length exceeds maximum");
    auto first=fixture(),second=fixture();std::vector<std::byte> duplicate(first.size()+second.size());
    std::memcpy(duplicate.data(),first.data(),first.size());std::memcpy(duplicate.data()+first.size(),second.data(),second.size());
    auto* a=reinterpret_cast<BasicProcessRecord*>(duplicate.data());auto* b=reinterpret_cast<BasicProcessRecord*>(duplicate.data()+first.size());
    a->next=static_cast<ULONG>(first.size());a->name=reinterpret_cast<PWSTR>(duplicate.data()+sizeof(*a));b->name=reinterpret_cast<PWSTR>(duplicate.data()+first.size()+sizeof(*b));
    need(!parse_basic_processes(duplicate,rows),"duplicate PIDs cannot enter CPU/history aggregation");
}
void recovery(){
    reset(Mode::ready);ProcessEnumerator live(query,fallback);need(live.collect(0)[0].pid==12 && fallbacks==0,"one successful source, no parallel fallback");
    reset(Mode::unsupported);ProcessEnumerator old_windows(query,fallback);for(unsigned i=0;i<100;++i)need(old_windows.collect(i*1000)[0].pid==99,"old Windows keeps complete Tool Help path");
    need(calls==1 && fallbacks==100,"static unsupported availability is cached");
    reset(Mode::denied);ProcessEnumerator temporary(query,fallback);need(temporary.collect(0)[0].pid==99,"failure uses full fallback immediately");mode=Mode::ready;
    need(temporary.collect(29999)[0].pid==99 && calls==1,"unstable source does not flap each sample");
    need(temporary.collect(30000)[0].pid==12 && calls==2,"native source recovers after bounded availability retry");
    reset(Mode::grow);ProcessEnumerator growing(query,fallback);need(growing.collect(0)[0].pid==12 && calls==2,"growing reply retries and preserves all records");
    need(growing.collect(1)[0].pid==12 && calls==3,"query buffer is reused");
    for(auto fault:{Mode::corrupt,Mode::oversize,Mode::missing_size}){reset(fault);ProcessEnumerator bad(query,fallback);need(bad.collect(0)[0].pid==99 && fallbacks==1,"malformed native list never becomes partial/fake success");}
    reset(Mode::ready);ProcessEnumerator missing(nullptr,fallback);need(missing.collect(0)[0].pid==99 && calls==0,"missing export preserves Windows compatibility");
}
}
int main(){try{parser();recovery();ProcessEnumerator real;const auto rows=real.collect(GetTickCount64());need(std::any_of(rows.begin(),rows.end(),[](const auto& row){return row.pid==GetCurrentProcessId() && !row.name.empty();}),"real provider includes current process");
    std::cout<<"Process enumeration identity, bounds, availability, fallback and recovery passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
