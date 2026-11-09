// Test-only execution of original source-view words; never a shipping adapter.
// liveness-roots: this test enters no generated code - the original words it
// compares with run in the reference interpreter - so its literals must not
// keep the replaced bodies in the shipping image:
// liveness-roots: reference-interpreter-only
#include "gekko/cpu.h"
#include "gekko/guest_memory.h"
#include <array>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <vector>
using namespace gekko;
namespace {
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
constexpr u32 object=0x80070000, origin=object, extension=0x80070400;
constexpr u32 done=0x800F1000;
constexpr std::array<u32,9> channels{1,2,4,8,16,32,64,128,256};
constexpr std::array<u32,9> fields{0x4C,0x6C,0x64,0x54,0x5C,0x7C,0x74,0x84,0x8C};
struct Oracle {
    GuestMemory bus;
    Cpu cpu{bus};
    std::size_t cases=0;
    explicit Oracle(const char* path) {
        std::ifstream file(path,std::ios::binary);
        std::vector<u8> raw{std::istreambuf_iterator<char>(file),{}};
        check(raw.size()>=256,"DOL missing");
        const auto be=[&](std::size_t offset) {
            check(offset+4<=raw.size(),"DOL header extent");
            return u32(raw[offset])<<24|u32(raw[offset+1])<<16|u32(raw[offset+2])<<8|raw[offset+3];
        };
        for(unsigned i=0;i<18;++i) {
            const auto start=be(i*4), address=be(0x48+i*4), size=be(0x90+i*4);
            if(!size) continue;
            check(u64(start)+size<=raw.size() && address>=0x80000000,"DOL section extent");
            auto* target=bus.host_pointer(address,size);
            check(target!=nullptr,"original oracle RAM extent");
            std::memcpy(target,raw.data()+start,size);
        }
        check(bus.read32(0x80449458)==0x800D5FF0 && bus.read32(0x804495E4)==0x4E800020,
              "original source-view boundary drift");
        bus.write32(0x805F24F0,0x20);
    }
    void run(u32 requested,u32 supported,u32 stride,u32 mode,bool null_stream,u32 output) {
        cpu.reset();
        cpu.set_msr(kMsrIR|kMsrDR); cpu.select_memory_window();
        cpu.set_entry(0x80449458); cpu.lr=done;
        cpu.gpr[3]=object; cpu.gpr[4]=output; cpu.gpr[5]=requested;
        cpu.gpr[6]=mode; cpu.gpr[13]=0x805EC500;
        auto* fixture=bus.host_pointer(origin,0x1000);
        check(fixture!=nullptr,"fixture extent");
        std::memset(fixture,0xA5,0x1000);
        bus.write32(origin+0x20,extension);
        bus.write32(origin+0x400+0xA0,supported);
        bus.write32(origin+0x400+0x9C,stride);
        bus.write32(origin+0x400+0x3C,0x10000000);
        for(unsigned i=0;i<channels.size();++i) {
            bus.write32(origin+0x400+fields[i],null_stream?0:0x12340000+i*0x100);
            bus.write32(origin+0x400+fields[i]+4,4+i*4);
        }
        std::array<u8,0x1000> expected{};
        std::memcpy(expected.data(),fixture,expected.size());
        const auto read=[&](u32 address) {
            const auto p=address-object;
            check(u64(p)+4<=expected.size(),"expected read extent");
            return u32(expected[p])<<24|u32(expected[p+1])<<16|u32(expected[p+2])<<8|expected[p+3];
        };
        const auto write=[&](u32 address,u32 value) {
            const auto p=address-object;
            check(u64(p)+4<=expected.size(),"expected write extent");
            for(unsigned i=0;i<4;++i) expected[p+i]=u8(value>>(24-8*i));
        };
        u32 result=1;
        if((supported&requested)==0) {
            write(output,0); write(output+4,0); result=0;
        } else {
            for(unsigned i=0;i<channels.size();++i) if(requested==channels[i]) {
                // Both input words precede either output write in the original.
                const auto pointer=read(extension+fields[i]);
                const auto source_stride=read(extension+fields[i]+4);
                write(output,pointer); write(output+4,source_stride);
            }
            const auto interleaved=read(extension+0x9C);
            if(interleaved) write(output+4,interleaved);
            if(read(output)!=0 && (mode&0x40000000)) {
                // A second plugin lookup is genuinely observable if output aliases it.
                const auto owner=read(object+0x20);
                const auto dirty=read(owner+0x3C);
                write(owner+0x3C,dirty|mode|requested);
            }
        }
        unsigned steps=0;
        while(!cpu.stopped() && cpu.pc!=done && steps++<120) cpu.step();
        check(!cpu.stopped() && cpu.pc==done,"original source-view did not return");
        check(cpu.gpr[3]==result,"original source-view result differs");
        check(std::memcmp(expected.data(),fixture,expected.size())==0,
              "source view bytes/alias/dirty effects differ");
        check(bus.diagnostics().unmapped_reads==0 && bus.diagnostics().unmapped_writes==0,
              "original source-view unmapped access");
        ++cases;
    }
};
}
int main(int argc,char** argv) {
    try {
        check(argc==2,"original DOL argument required");
        auto oracle=std::make_unique<Oracle>(argv[1]);
        std::array<u32,11> supports{0,511,1,2,4,8,16,32,64,128,256};
        for(u32 request=0;request<512;++request)
            for(auto support:supports) for(auto stride:{0u,44u})
                for(auto mode:{0u,0x40000000u,0xFFFFFFFFu}) for(bool empty:{false,true})
                    oracle->run(request,support,stride,mode,empty,object+0x800);
        for(auto request:{0x80000000u,0xFFFFFFFFu,0x40000001u})
            for(auto stride:{0u,44u}) oracle->run(request,0xFFFFFFFF,stride,0,true,object+0x800);
        // Valid field aliases with a known mapped second plugin lookup.
        for(unsigned i=0;i<channels.size();++i) for(auto stride:{0u,44u})
            for(auto mode:{0u,0x40000000u})
                for(auto output:{extension+fields[i],extension+fields[i]+4,
                                 extension+0x9C,extension+0xA0,extension+0x3C})
                    oracle->run(channels[i],511,stride,mode,false,output);
        // Object slot can alias the output when no dirty lookup will consume it.
        oracle->run(1,511,44,0,false,object+0x20);
        std::cout<<"source_view_original_executions="<<oracle->cases
                 <<" entire_fixture_bytes=4096 native_retirement=false\n";
        return 0;
    } catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
