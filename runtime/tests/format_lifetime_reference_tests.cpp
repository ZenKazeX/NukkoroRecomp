// Original-word execution only. External allocator callbacks are synthetic,
// explicitly intercepted fixtures; this is not a production Cpu adapter.
// Retained fixture backing proves ordering, never a real allocator UAF outcome.
// liveness-roots: this test enters no generated code - the original words it
// compares with run in the reference interpreter - so its literals must not
// keep the replaced bodies in the shipping image:
// liveness-roots: reference-interpreter-only
#include "gekko/cpu.h"
#include "gekko/guest_memory.h"
#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <vector>

using namespace gekko;
namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
constexpr u32 kObject=0x80080000, kOld=0x80081040, kNew=0x80082040;
constexpr u32 kStack=0x800F0000, kReturn=0x800F1000;
constexpr u32 kAlloc=0x800F2000, kFree=0x800F2004, kSlotOffset=0x40;
struct Callback {
    u32 target, argument, tag, slot;
};
struct Oracle {
    GuestMemory bus;
    Cpu cpu{bus};
    std::vector<Callback> callbacks;
    u32 allocation_result=kNew;
    unsigned calls=0;
    explicit Oracle(const char* path) {
        std::ifstream file(path,std::ios::binary);
        std::vector<u8> raw{std::istreambuf_iterator<char>(file),{}};
        require(raw.size()>=256,"original DOL missing");
        const auto be=[&](std::size_t p) {
            require(p+4<=raw.size(),"original DOL header extent");
            return u32(raw[p])<<24|u32(raw[p+1])<<16|u32(raw[p+2])<<8|raw[p+3];
        };
        for (unsigned i=0;i<18;++i) {
            const u32 off=be(i*4),address=be(0x48+i*4),size=be(0x90+i*4);
            if (!size) continue;
            require(u64(off)+size<=raw.size()&&address>=0x80000000,"DOL section extent");
            auto* destination=bus.host_pointer(address,size);
            require(destination!=nullptr,"DOL oracle RAM extent");
            std::memcpy(destination,raw.data()+off,size);
        }
        for (auto pair:std::array<std::array<u32,2>,10>{{
            {0x80463FCC,0x98830000},{0x80463FD8,0x98830001},
            {0x80463FE4,0x7C632214},{0x80463FF4,0x9883000A},
            {0x80463FFC,0x38C00004},{0x8046406C,0x9421FFF0},
            {0x80464110,0x9421FFF0},{0x80464154,0x9421FFE0},
            {0x80463E78,0x38000000},{0x80463E84,0x9421FFF0}}})
            require(bus.read32((pair[0]))==pair[1],"original format entry drift");
        require(bus.read32(0x804641AC)==0x7FDDF92E&&
                bus.read32(0x804641B4)==0xA0640016&&
                bus.read32(0x80463EA4)==0x4800026D,"original ownership order drift");
        cpu.reset();
        cpu.set_msr(kMsrIR|kMsrDR);
        cpu.select_memory_window();
        // Production's policy: an access the window cannot serve (the null
        // store below) stops the machine and latches the fault.
        cpu.set_unmapped_access_stops(true);
        bus.write32(0x805F25C8,kSlotOffset); // SDA13+60C8.
        bus.write32(0x805F265C,0x80083000); // registered allocator table fixture.
        bus.write32(0x80083108,kAlloc); bus.write32(0x8008310C,kFree);
        callbacks.reserve(2);
    }
    void fill(u32 descriptor) {
        std::memset(bus.host_pointer(descriptor-32,384),0xA5,384);
    }
    void invoke(u32 entry,u32 r3,u32 r4=0,u32 r5=0,u32 r6=0,bool expect_null_fault=false) {
        callbacks.clear(); ++calls;
        cpu.lr=kReturn; cpu.gpr[1]=kStack; cpu.gpr[13]=0x805EC500;
        cpu.gpr[3]=r3; cpu.gpr[4]=r4; cpu.gpr[5]=r5; cpu.gpr[6]=r6;
        std::array<u32,18> preserved{};
        for (unsigned i=14;i<32;++i) cpu.gpr[i]=preserved[i-14]=0xA1000000+i;
        cpu.set_entry(entry);
        unsigned steps=0;
        while (!cpu.stopped()&&cpu.pc!=kReturn&&steps<128) {
            ++steps;
            if (cpu.pc==kAlloc||cpu.pc==kFree) {
                require(callbacks.size()<2,"unexpected external fixture callback count");
                callbacks.push_back({cpu.pc,cpu.gpr[3],cpu.gpr[4],bus.read32(kObject+kSlotOffset)});
                if (cpu.pc==kAlloc) cpu.gpr[3]=allocation_result;
                // The free fixture intentionally retains backing. Do not infer a
                // valid same-pointer sole-reference replacement from this.
                cpu.pc=cpu.lr;
            } else cpu.step();
        }
        if (expect_null_fault&&cpu.stopped()&&cpu.stop().reason==StopReason::ExecutionFault&&
            cpu.memory_fault().latched&&cpu.memory_fault().unmapped&&cpu.memory_fault().write&&
            cpu.memory_fault().effective==0&&cpu.memory_fault().pc==0x8046409C&&
            cpu.stop().address==0x8046409C&&cpu.stop().instruction==0&&
            bus.read32((cpu.memory_fault().pc))==0x98C30000&&
            cpu.pc==0x804640A0&&steps==14&&callbacks.size()==1&&callbacks[0].target==kAlloc) return;
        if (expect_null_fault||cpu.stopped()||cpu.pc!=kReturn) {
            std::ostringstream message;
            message<<"original format body did not return: call="<<calls
                   <<" entry=0x"<<std::hex<<entry<<" pc=0x"<<cpu.pc<<" lr=0x"<<cpu.lr
                   <<" arguments=0x"<<r3<<",0x"<<r4<<",0x"<<r5<<",0x"<<r6
                   <<std::dec<<" steps="<<steps<<" callbacks="<<callbacks.size()
                   <<" stop_reason="<<static_cast<unsigned>(cpu.stop().reason)
                   <<" stop_address=0x"<<std::hex<<cpu.stop().address
                   <<" stop_instruction=0x"<<cpu.stop().instruction
                   <<" detail="<<cpu.stop().detail;
            throw std::runtime_error(message.str());
        }
        require(cpu.gpr[1]==kStack&&cpu.lr==kReturn,"original stack/LR drift");
        for (unsigned i=14;i<32;++i) require(cpu.gpr[i]==preserved[i-14],"original preserved integer value drift");
        require(bus.diagnostics().unmapped_reads==0&&bus.diagnostics().unmapped_writes==0,
                "original fixture unmapped access");
    }
    std::array<u8,24> bytes(u32 descriptor) {
        std::array<u8,24> result{};
        std::memcpy(result.data(),bus.host_pointer(descriptor,result.size()),result.size());
        return result;
    }
    void reset_and_mutators() {
        std::array<u8,24> expected{};
        for (unsigned i=0;i<10;++i) expected[i]=4;
        expected[10]=5; expected[11]=1; expected[23]=1;
        fill(kNew); invoke(0x80463FFC,kNew);
        require(bytes(kNew)==expected,"original complete descriptor reset mismatch");
        require(bus.read8(kNew-1)==0xA5&&bus.read8(kNew+24)==0xA5,"reset overrun");
        for (unsigned kind=0;kind<256;++kind) for (unsigned fraction=0;fraction<256;++fraction) {
            for (unsigned normal=0;normal<2;++normal) {
                fill(kNew); auto after=bytes(kNew);
                after[normal]=u8(kind); after[12+normal]=u8(fraction);
                invoke(normal?0x80463FD8:0x80463FCC,kNew,kind|0xABCDEF00,fraction|0xFEDCBA00);
                require(bytes(kNew)==after,"setter low-byte narrowing or unrelated byte mutation");
            }
        }
        for (u32 index:{0xFFFFFFF3u,0xFFFFFFFFu,0u,1u,8u,9u,10u,11u,12u,256u}) {
            fill(kNew);
            std::array<u8,384> after{};
            std::memcpy(after.data(),bus.host_pointer(kNew-32,384),384);
            const auto displacement=static_cast<std::int32_t>(index);
            after[32+displacement+1]=0xCF; after[32+displacement+13]=0x8A;
            invoke(0x80463FE4,kNew,index,0xABCDEFcf,0x7654328A);
            require(std::memcmp(after.data(),bus.host_pointer(kNew-32,384),384)==0,
                    "indexed setter exact original extent mismatch");
        }
        for (unsigned color=0;color<256;++color) {
            fill(kNew); auto after=bytes(kNew); after[10]=u8(color);
            invoke(0x80463FF4,kNew,0x12345600|color);
            require(bytes(kNew)==after,"color setter extent mismatch");
        }
    }
    void creation() {
        fill(kNew); allocation_result=kNew;
        invoke(0x8046406C,0);
        const auto made=bytes(kNew);
        require(callbacks.size()==1&&callbacks[0].target==kAlloc&&callbacks[0].argument==24&&
                callbacks[0].tag==0x3050D&&cpu.gpr[3]==kNew,"original allocation request/result mismatch");
        fill(kNew); invoke(0x80463FFC,kNew);
        require(bytes(kNew)==made,"create/reset initial bytes differ");
    }
    void null_allocation_fault() {
        // Explicit synthetic callback result, actual original store instruction,
        // DR enabled with no null-address mapping. Run last: the precise data
        // fault latches until reset, and must not be erased to force a return.
        // This is not a proof of a real gameplay allocation-exhaustion route.
        require(bus.read32(0x8046409C)==0x98C30000,"null first-store drift");
        std::array<u8,24> low{};
        std::memcpy(low.data(),bus.host_pointer(0x80000000,24),24);
        std::memset(bus.host_pointer(0x80000000,24),0x5A,24);
        allocation_result=0; invoke(0x8046406C,0,0,0,0,true);
        for (unsigned i=0;i<24;++i) require(bus.read8(0x80000000+i)==0x5A,"faulting null store changed the OS low memory");
        require(cpu.spr[SPR_DAR]==0&&cpu.spr[SPR_DSISR]==0x42000000,
                "null allocation data-fault architected latch mismatch");
        std::memcpy(bus.host_pointer(0x80000000,24),low.data(),24);
        allocation_result=kNew;
    }
    void releases_and_attachment() {
        for (unsigned count=0;count<65536;++count) {
            fill(kOld); bus.write16(kOld+22,u16(count));
            invoke(0x80464110,kOld);
            if (count==1) {
                require(callbacks.size()==1&&callbacks[0].target==kFree&&callbacks[0].argument==kOld,
                        "release final reference callback mismatch");
            } else {
                require(callbacks.empty()&&bus.read16(kOld+22)==u16(count-1),
                        "release 16-bit decrement/wrap mismatch");
            }
            fill(kNew); bus.write16(kNew+22,u16(count));
            bus.write32(kObject+kSlotOffset,0);
            invoke(0x80464154,kObject,kNew);
            require(callbacks.empty()&&bus.read32(kObject+kSlotOffset)==kNew&&
                    bus.read16(kNew+22)==u16(count+1),"attachment 16-bit increment/wrap mismatch");
        }
        for (unsigned old_count:{1u,2u,65535u}) for (bool same:{false,true}) {
            fill(kOld); fill(kNew); bus.write16(kOld+22,u16(old_count));
            bus.write16(kNew+22,7);
            bus.write32(kObject+kSlotOffset,kOld);
            const u32 replacement=same?kOld:kNew;
            invoke(0x80464154,kObject,replacement);
            require(bus.read32(kObject+kSlotOffset)==replacement,"attachment slot mismatch");
            if (old_count==1) {
                require(callbacks.size()==1&&callbacks[0].argument==kOld&&callbacks[0].slot==kOld,
                        "old slot must remain published during release callback");
            } else require(callbacks.empty(),"non-final reference unexpectedly freed");
            require(bus.read16(replacement+22)==u16(same?(old_count==1?2:old_count):8),
                    "synthetic retained-backing attachment order mismatch");
        }
        bus.write32(kObject+kSlotOffset,0xAABBCCDD);
        invoke(0x80463E78,kObject,kSlotOffset);
        require(bus.read32(kObject+kSlotOffset)==0,"plugin construction did not zero slot");
        invoke(0x80463E84,kObject,kSlotOffset);
        require(callbacks.empty()&&cpu.gpr[3]==kObject,"null-slot plugin destruction mismatch");
        for (unsigned count:{1u,2u}) {
            bus.write16(kOld+22,u16(count));
            bus.write32(kObject+kSlotOffset,kOld);
            invoke(0x80463E84,kObject,kSlotOffset);
            require(bus.read32(kObject+kSlotOffset)==kOld&&cpu.gpr[3]==kObject,
                    "destructor unexpectedly cleared slot or changed return owner");
            require((count==1&&callbacks.size()==1)||(count==2&&callbacks.empty()),"destructor release mismatch");
        }
    }
};
}
int main(int argc,char** argv) {
    try {
        require(argc==2,"usage: format_lifetime_reference_tests ORIGINAL_DOL");
        auto oracle=std::make_unique<Oracle>(argv[1]);
        oracle->reset_and_mutators(); oracle->creation(); oracle->releases_and_attachment();
        oracle->null_allocation_fault();
        std::cout<<"Original format fixture invocations "<<oracle->calls
                 <<"; all byte-value/setter narrowing and 65536-count release/attach populations pass.\n"
                 <<"External allocation/free effects are synthetic; alias/error/serialization closure and native retirement unproved.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr<<"original format ownership fixture failed: "<<error.what()<<'\n'; return 1;
    }
}
