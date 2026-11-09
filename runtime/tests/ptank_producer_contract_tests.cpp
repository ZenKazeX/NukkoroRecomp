// Original words are a reference-only contract oracle. No production adapter.
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
void check(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
constexpr std::array<u32, 10> entries{0x8044A25C,0x8044A620,0x8044A9E4,0x8044ADAC,
    0x8044B1B8,0x8044B590,0x8044B98C,0x8044BDF8,0x8044C260,0x8044C6A4};
constexpr std::array<u32, 8> channel_bits{0x01000000,0x00010000,0x00020000,
    0x00040000,0x00100000,0x00200000,0x00400000,0x00800000};
constexpr u32 descriptor = 0x80070000, stack = 0x800F0000, return_pc = 0x800F1000;
u32 bits(float f) { u32 n; std::memcpy(&n, &f, 4); return n; }
struct Recorder : MmioDevice {
    std::vector<u8> bytes;
    const char* name() const override { return "test-only-original-stream"; }
    void append(u32 n, unsigned width) {
        check(bytes.size() + width <= 4096, "reference output exceeds fixture bound");
        for (unsigned i = width; i; --i) bytes.push_back(u8(n >> ((i-1)*8)));
    }
    void write8(u32, u8 n) override { append(n,1); }
    void write16(u32, u16 n) override { append(n,2); }
    void write32(u32, u32 n) override { append(n,4); }
};
struct Result {
    std::vector<u8> bytes;
    u32 changed_secondary_lanes = 0, status = 0;
};
struct Oracle {
    GuestMemory bus;
    Cpu cpu{bus};
    Recorder output;
    explicit Oracle(const char* path) {
        std::ifstream file(path, std::ios::binary);
        std::vector<u8> raw{std::istreambuf_iterator<char>(file), {}};
        check(raw.size() >= 256, "original DOL missing");
        const auto be = [&](std::size_t p) {
            check(p+4 <= raw.size(), "DOL header extent");
            return u32(raw[p])<<24 | u32(raw[p+1])<<16 | u32(raw[p+2])<<8 | raw[p+3];
        };
        for (unsigned i=0; i<18; ++i) {
            const u32 p=be(i*4), at=be(0x48+i*4), size=be(0x90+i*4);
            if (!size) continue;
            check(u64(p)+size <= raw.size() && at >= 0x80000000, "DOL section extent");
            auto* target=bus.host_pointer(at,size);
            check(target != nullptr, "DOL oracle RAM extent");
            std::memcpy(target,raw.data()+p,size);
        }
        for (auto at: entries) check(bus.read32(at)==0x9421FF18, "producer entry drift");
        check(bus.read32(0x805FC118)==0xCC008000, "original publication pointer drift");
        const std::array<u8,5> types{4,6,5,7,0}, scales{0,6,0,14,0};
        for (unsigned i=0;i<5;++i) {
            check(bus.read8(0x805FC0F8+i)==types[i], "original type table drift");
            check(bus.read8(0x805FC100+i)==scales[i], "original scale table drift");
        }
        output.bytes.reserve(4096);
        bus.register_mmio(0xCC008000,0x100,&output);
        bus.write32(0x805EEE28,0x805FC110);
        bus.write32(0x805EEE2C,0x8051CE08);
    }
    void setup(u32 seed) {
        cpu.reset();
        cpu.spr[SPR_HID2]=kHid2PSE|kHid2LSQE;
        cpu.set_msr(kMsrFP|kMsrIR|kMsrDR); // No asynchronous observer closure claim.
        cpu.select_memory_window();
        cpu.fpscr=kFpscrNI;
        cpu.lr=return_pc; cpu.cr=0xA5B67432;
        cpu.gpr[1]=stack; cpu.gpr[2]=0x805FA780; cpu.gpr[13]=0x805EC500;
        for (unsigned i=14;i<32;++i) cpu.gpr[i]=seed*0x12345+i;
        for (unsigned i=0;i<32;++i) for (unsigned lane=0;lane<2;++lane)
            cpu.fpr[i].bits[lane]=double_to_bits(double(seed+i+lane*97)*0.125);
    }
    void selector(u32 authored) {
        setup(7);
        cpu.set_entry(0x804496A8); cpu.gpr[3]=0x80071900;
        bus.write32(0x805F24F0,0x20); // SDA +5FF0, plugin offset input.
        bus.write32(0x80071920,0x80072000);
        std::memset(bus.host_pointer(0x80072000,0x100),0xA5,0x100);
        bus.write32(0x800720A0,authored);
        unsigned selected=2;
        if (authored&8) selected=0;
        else if (authored&0x8000) selected=2;
        else if (authored&0x20) selected=(authored&4)?8:6;
        else if (authored&4) selected=4;
        if (authored&0x01000000) ++selected;
        u32 mask=1u<<selected;
        if (authored&0x80) mask|=0x00100000;
        else if (authored&0x100) mask|=0x00200000;
        else if (authored&0x80000) mask|=0x00400000;
        else if (authored&0x100000) mask|=0x00800000;
        if (authored&2) mask|=0x00010000;
        else if (authored&0x40) mask|=0x00020000;
        else if (authored&0x40000) mask|=0x00040000;
        else mask|=0x00080000;
        if (authored&0x10) mask|=0x01000000;
        unsigned steps=0;
        while (!cpu.stopped() && cpu.pc!=return_pc && steps++<200) cpu.step();
        check(!cpu.stopped() && cpu.pc==return_pc,"original selector failed");
        check(bus.read32(0x80072028)==entries[selected] && bus.read32(0x80072044)==mask,
              "original selector branch priority/derived channels drift");
        for (u32 offset:{0x24u,0x2Cu,0x30u,0x34u,0x38u})
            check(bus.read32(0x80072000+offset)==0,"original selector reset contract");
    }
    Result run(u32 entry,u32 flags,u32 count,u32 position_format,u32 normal_format,u32 seed) {
        setup(seed);
        cpu.set_entry(entry); cpu.gpr[3]=count; cpu.gpr[4]=flags; cpu.gpr[5]=descriptor;
        cpu.spr[SPR_GQR0+6]=position_format; cpu.spr[SPR_GQR7]=normal_format;
        std::array<u32,18> preserved{};
        std::array<std::array<u64,2>,18> fp{};
        for (unsigned i=14;i<32;++i) {
            preserved[i-14]=cpu.gpr[i];
            for (unsigned lane=0;lane<2;++lane) fp[i-14][lane]=cpu.fpr[i].bits[lane];
        }
        std::memset(bus.host_pointer(descriptor,0x800),0xA5,0x800);
        const auto pointer = [&](u32 field,u32 source,u32 stride) {
            bus.write32(descriptor+field,source); bus.write32(descriptor+field+4,stride);
        };
        pointer(4,0x80070100,12); pointer(0xC,0x80070200,64); pointer(0x14,0x80070300,12);
        pointer(0x1C,0x80070400,8); pointer(0x24,0x80070500,4); pointer(0x2C,0x80070500,16);
        pointer(0x34,0x80070440,4); pointer(0x3C,0x80070600,16); pointer(0x44,0x80070600,32);
        bus.write32(descriptor+0x4C,0x80070700);
        for (unsigned i=0;i<6;++i) bus.write32(0x80070700+i*4,bits(float(i+1)*0.2f));
        for (unsigned i=0;i<2;++i) {
            bus.write32(descriptor+0x68+i*4,bits(float(i+1)*0.11f));
            bus.write32(descriptor+0x70+i*4,bits(float(i+2)*0.6f));
        }
        for (unsigned i=0;i<4;++i) {
            bus.write32(descriptor+0x80+i*4,0x34567890+i*0x11111111);
            for (unsigned axis=0;axis<2;++axis)
                bus.write32(descriptor+0x90+i*8+axis*4,bits(float(i*2+axis)*0.13f));
        }
        for (unsigned p=0;p<3;++p) {
            for (unsigned axis=0;axis<3;++axis) {
                bus.write32(0x80070100+p*12+axis*4,bits(float(p+axis)*1.3f-2.7f));
                bus.write32(0x80070300+p*12+axis*4,bits(float(p+axis)*0.07f-0.2f));
            }
            for (unsigned i=0;i<16;++i)
                bus.write32(0x80070200+p*64+i*4,bits(float(i+p)*0.17f-0.8f));
            for (unsigned i=0;i<2;++i) bus.write32(0x80070400+p*8+i*4,bits(float(p+i+1)*0.9f));
            bus.write32(0x80070440+p*4,bits(float(p)*0.25f-0.1f));
            for (unsigned i=0;i<4;++i) bus.write32(0x80070500+p*16+i*4,0x10203040+p*0x112233+i);
            for (unsigned i=0;i<8;++i) bus.write32(0x80070600+p*32+i*4,bits(float(p+i)*0.13f));
        }
        std::array<u8,0x800> before{};
        std::memcpy(before.data(),bus.host_pointer(descriptor,before.size()),before.size());
        output.bytes.clear();
        unsigned steps=0;
        while (!cpu.stopped() && cpu.pc!=return_pc && steps++<2000) cpu.step();
        check(!cpu.stopped() && cpu.pc==return_pc,"original producer did not return");
        check(cpu.gpr[1]==stack && cpu.lr==return_pc && cpu.cr==0xA5B67432,
              "original stack/LR/CR restoration contract");
        check(cpu.spr[SPR_GQR0]==0 && cpu.spr[SPR_GQR0+6]==position_format && cpu.spr[SPR_GQR7]==normal_format,
              "producer changed resource formats");
        check(std::memcmp(before.data(),bus.host_pointer(descriptor,before.size()),before.size())==0,
              "original producer mutated source fixture");
        Result result; result.bytes=output.bytes; result.status=cpu.fpscr;
        for (unsigned i=14;i<32;++i) {
            check(cpu.gpr[i]==preserved[i-14],"original preserved GPR changed");
            check(cpu.fpr[i].bits[0]==fp[i-14][0],"original primary FP lane changed");
            if (cpu.fpr[i].bits[1]!=fp[i-14][1]) result.changed_secondary_lanes|=1u<<i;
        }
        check(bus.diagnostics().unmapped_reads==0 && bus.diagnostics().unmapped_writes==0,"unmapped original access");
        return result;
    }
};
unsigned width(u32 format) { const auto t=format&7; return t==4||t==6 ? 1 : t==5||t==7 ? 2 : 4; }
std::vector<u8> incoming_normal(u32 seed,u32 format) {
    std::vector<u8> bytes;
    const std::array<double,3> values{double(seed+19)*0.125,double(seed+19+97)*0.125,double(seed+18)*0.125};
    const u32 type=format&7, raw=(format>>8)&63;
    const int exponent=raw<32 ? int(raw) : int(raw)-64;
    for (auto value:values) {
        u32 encoded=bits(float(value));
        if (type>=4) {
            double factor=1;
            for (int i=0;i<exponent;++i) factor*=2;
            for (int i=0;i>exponent;--i) factor*=0.5;
            double n=value*factor;
            const double low=type==6 ? -128 : type==7 ? -32768 : 0;
            const double high=type==4 ? 255 : type==5 ? 65535 : type==6 ? 127 : 32767;
            n=n<low ? low : n<=high ? n : high;
            encoded=u32(s32(n));
        }
        for (unsigned i=width(format);i;--i) bytes.push_back(u8(encoded>>((i-1)*8)));
    }
    return bytes;
}
}
int main(int argc,char** argv) {
    try {
        check(argc==2,"usage: ptank_producer_contract_tests main.dol");
        auto oracle=std::make_unique<Oracle>(argv[1]);
        constexpr std::array<u32,13> authored_bits{8,0x8000,0x20,4,0x01000000,
            0x10,2,0x40,0x40000,0x80,0x100,0x80000,0x100000};
        for (u32 mask=0;mask<8192;++mask) {
            u32 authored=0;
            for (unsigned i=0;i<authored_bits.size();++i) if (mask&(1u<<i)) authored|=authored_bits[i];
            oracle->selector(authored);
        }
        std::cout<<"original_selector_cases=8192 branch_priority=pass derived_channels=pass reset_contract=pass\n";
        std::uint64_t cases=0, output_bytes=0;
        for (auto entry:entries) {
            u32 changed_lanes=0;
            std::uint64_t incoming_dependency_cases=0;
            const auto test=[&](u32 flags,u32 count,u32 pos,u32 normal) {
                auto a=oracle->run(entry,flags,count,pos,normal,7);
                auto b=oracle->run(entry,flags,count,pos,normal,193);
                const auto stride=3*width(pos)+((flags&0x01000000)?3*width(normal):0)+
                    ((flags&0x00070000)?4:0)+((flags&0x00F00000)?8:0);
                check(a.bytes.size()==std::size_t(count)*4*stride,"original output extent contract");
                const bool inherited_normal=entry<=0x8044B1B8 &&
                    (flags&0x01000000) && (flags&0x00100000) && count!=0;
                const auto normal_a=incoming_normal(7,normal), normal_b=incoming_normal(193,normal);
                if (inherited_normal) {
                    ++incoming_dependency_cases;
                    for (std::size_t v=0;v<count*4;++v) for (std::size_t byte=0;byte<normal_a.size();++byte) {
                        const auto at=v*stride+3*width(pos)+byte;
                        check(a.bytes[at]==normal_a[byte] && b.bytes[at]==normal_b[byte],
                              "inherited normal no longer matches exact incoming lanes");
                    }
                }
                bool unexpected=a.status!=b.status;
                for (std::size_t i=0;i<a.bytes.size();++i) {
                    const auto offset=i%stride;
                    const bool normal_byte=inherited_normal && offset>=3*width(pos) && offset<3*(width(pos)+width(normal));
                    if (!normal_byte && a.bytes[i]!=b.bytes[i]) unexpected=true;
                }
                if (unexpected) {
                    std::cerr<<"entry="<<std::hex<<entry<<" flags="<<flags<<" count="<<count
                             <<" position_format="<<pos<<" normal_format="<<normal
                             <<" status_a="<<a.status<<" status_b="<<b.status<<std::dec<<'\n';
                    for (std::size_t i=0;i<a.bytes.size();++i) if (a.bytes[i]!=b.bytes[i]) {
                        std::cerr<<"first_byte="<<i<<" a="<<unsigned(a.bytes[i])<<" b="<<unsigned(b.bytes[i])<<'\n'; break;
                    }
                    check(false,"output depends on supplied FP/GPR state");
                }
                changed_lanes|=a.changed_secondary_lanes|b.changed_secondary_lanes;
                output_bytes+=a.bytes.size(); ++cases;
            };
            for (u32 mask=0;mask<256;++mask) {
                u32 flags=0;
                for (unsigned i=0;i<8;++i) if (mask&(1u<<i)) flags|=channel_bits[i];
                for (u32 count: {0u,1u,3u}) test(flags,count,0,0);
            }
            // Every original authored position/normal kind combination. Signed
            // scales include both extremal binary exponents as adversarial cases.
            constexpr std::array<u32,5> kinds{0,4,6,5,7};
            for (auto pos:kinds) for (auto normal:kinds) for (u32 scale: {0u,6u,14u,31u,32u,63u})
                for (u32 channels=0;channels<8;++channels) {
                    const u32 flags=((channels&1)?0x01000000:0)|((channels&2)?0x00020000:0)|((channels&4)?0x00200000:0);
                    const u32 pos_half=pos|(scale<<8), normal_half=normal|(scale<<8);
                    test(flags,3,pos_half|(pos_half<<16),normal_half|(normal_half<<16));
                }
            std::cout<<"producer="<<std::hex<<entry<<" modified_secondary_lanes="<<changed_lanes<<std::dec
                     <<" inherited_normal_cases="<<incoming_dependency_cases<<'\n';
        }
        std::cout<<"cases="<<cases<<" original_executions="<<cases*2<<" output_bytes="<<output_bytes
                 <<" only_classified_incoming_normal_dependence=pass source_fixture_immutable=pass\n";
        std::cout<<"scope=reference-only asynchronous_alias_context_closure=false native_retirement=false\n";
        return 0;
    } catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
