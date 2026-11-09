// Original instructions are the numerical oracle; no generated game code is linked.
// liveness-roots: reference-interpreter-only
#include "math/hermite.h"
#include "gekko/cpu.h"
#include "gekko/guest_memory.h"
#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <vector>
#include <immintrin.h>

namespace {
using namespace gekko;
using shadow::math::HermiteKey;
constexpr u32 kBuffer=0x81001000u, kReturn=0x817FF000u, kStack=0x817E0000u;
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
float as_float(u32 bits) { float value; std::memcpy(&value,&bits,4); return value; }
u32 as_bits(float value) { u32 bits; std::memcpy(&bits,&value,4); return bits; }
u32 random_word(u32& seed) { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return seed; }
struct Oracle {
    GuestMemory memory;
    Cpu cpu{memory};
    unsigned cases=0;
    u64 instructions=0;
    explicit Oracle(const char* path) {
        std::ifstream file(path,std::ios::binary);
        const std::vector<u8> dol{std::istreambuf_iterator<char>(file),{}};
        u64 hash=14695981039346656037ull;
        for (u8 byte:dol) hash=(hash^byte)*1099511628211ull;
        require(hash==0xd2cfb4a1ca897f9bull,"original Hermite image changed");
        for (unsigned i=0;i<18;++i) {
            const u32 offset=load_be32(dol.data()+i*4), address=load_be32(dol.data()+0x48+i*4),
                      size=load_be32(dol.data()+0x90+i*4);
            require(u64(offset)+size<=dol.size(),"invalid DOL section");
            if (size) memory.write_block(address,dol.data()+offset,size);
        }
    }
    void check(const std::array<u32,9>& values, unsigned operation, bool flush) {
        for (unsigned i=0;i<8;++i) memory.write32(kBuffer+i*4,values[i]);
        const HermiteKey left{as_float(values[0]),as_float(values[1]),as_float(values[2]),as_float(values[3])};
        const HermiteKey right{as_float(values[4]),as_float(values[5]),as_float(values[6]),as_float(values[7])};
        cpu.reset(); cpu.pc=operation==2?0x80416580u:operation==1?0x8041669Cu:0x80416728u; cpu.lr=kReturn;
        cpu.gpr[1]=kStack; cpu.gpr[2]=0x805FA780u; cpu.gpr[3]=kBuffer; cpu.gpr[4]=kBuffer+16u;
        if (operation==2) {
            cpu.gpr[3]=kBuffer+64; cpu.gpr[4]=kBuffer+68;
            cpu.gpr[5]=kBuffer; cpu.gpr[6]=kBuffer+16;
            memory.write32(kBuffer+64,0xA5A5A5A5u); memory.write32(kBuffer+68,0xA5A5A5A5u);
        }
        cpu.fpr[1].bits[0]=double_to_bits(double(as_float(values[8])));
        cpu.fpscr=flush?4u:0u;
        const auto tick=cpu.game_clock().now();
        unsigned steps=0;
        while (cpu.pc!=kReturn && !cpu.stopped() && steps++<100) cpu.run_steps(1);
        require(!cpu.stopped() && cpu.pc==kReturn && steps<100 && cpu.gpr[1]==kStack &&
                cpu.game_clock().now()==tick,"original Hermite return/stack/time changed");
        require(cpu.fpscr==(flush?4u:0u),"original Hermite changed floating control/status");
        for (unsigned i=0;i<8;++i) require(memory.peek32(kBuffer+i*4)==values[i],"original Hermite wrote key data");
        const auto control=_mm_getcsr()&0xffc0u;
        const auto mode=flush?shadow::math::SubnormalMode::Flush:shadow::math::SubnormalMode::Preserve;
        const float result=operation==1?shadow::math::hermite_derivative(left,right,as_float(values[8]),mode)
                                     :shadow::math::hermite_value(left,right,as_float(values[8]),mode);
        require((_mm_getcsr()&0xffc0u)==control,"native Hermite changed host floating control");
        const auto both=shadow::math::evaluate_hermite_segment(left,right,as_float(values[8]),mode);
        const auto actual=double_to_bits(double(operation==2?both.value:result));
        const auto expected=operation==2?double_to_bits(double(as_float(memory.peek32(kBuffer+64)))):cpu.fpr[1].bits[0];
        const bool derivative_equal=operation!=2 || as_bits(both.derivative)==memory.peek32(kBuffer+68);
        if (actual!=expected || !derivative_equal) {
            std::fprintf(stderr,"Hermite case=%u operation=%u flush=%u expected=%016llX actual=%016llX\n",
                         cases,operation,unsigned(flush),
                         static_cast<unsigned long long>(expected),static_cast<unsigned long long>(actual));
            for (unsigned i=0;i<values.size();++i) std::fprintf(stderr,"input[%u]=%08X\n",i,values[i]);
            throw std::runtime_error("native Hermite differs from original instructions");
        }
        ++cases; instructions+=steps;
    }
    struct TrackResult {
        std::array<u32,24> storage{};
        std::array<u32,19> nonvolatile{};
        u64 value;
        u32 cr, fpscr;
        unsigned calls;
        bool operator==(const TrackResult& b) const {
            return storage==b.storage && nonvolatile==b.nonvolatile && value==b.value &&
                   cr==b.cr && fpscr==b.fpscr && calls==b.calls;
        }
    };
    TrackResult track(float time, u32 flags, bool derivative, bool native) {
        constexpr u32 track=kBuffer+0x100u, packed=kBuffer+0x200u, globals=0x805E2380u;
        for (u32 at=track;at<track+0x100u;at+=4) memory.write32(at,0);
        for (u32 at=kStack-64u;at<kStack+32u;at+=4) memory.write32(at,0xA5A5A5A5u);
        memory.write16(track,0); memory.write16(track+2,2);
        memory.write32(track+4,flags); memory.write32(track+8,packed);
        memory.write32(track+16,as_bits(17.0f));
        for (unsigned key=0;key<2;++key) {
            memory.write32(track+32+key*16,as_bits(float(key)));
            memory.write32(track+36+key*16,as_bits(1.0f));
            memory.write32(track+40+key*16,as_bits(1.0f));
            memory.write32(track+44+key*16,as_bits(float(key)));
            memory.write16(packed+key*6,u16(0x8002u+key));
            memory.write16(packed+key*6+2,0x3C00u);
            memory.write16(packed+key*6+4,u16(0x4000u+key*0x200u));
        }
        memory.write32(globals+12,as_bits(time));
        const u32 entry=derivative?0x8041669Cu:0x80416728u;
        memory.write32(globals+20,entry);
        cpu.reset(); cpu.pc=0x804162ACu;cpu.lr=kReturn;cpu.gpr[1]=kStack;
        cpu.gpr[2]=0x805FA780u;cpu.gpr[3]=track;cpu.gpr[13]=0x805EC500u;
        for (unsigned i=14;i<32;++i) cpu.gpr[i]=0x12340000u+i;
        cpu.fpscr=4u; const auto time_before=cpu.game_clock().now();
        unsigned steps=0,hits=0;
        while (cpu.pc!=kReturn && !cpu.stopped() && steps++<3000) {
            if (cpu.pc==entry) {
                ++hits;
                if (native) {
                    HermiteKey keys[2];
                    for (unsigned key=0;key<2;++key) {
                        std::array<u32,4> words{};
                        for (unsigned i=0;i<4;++i) words[i]=memory.peek32(cpu.gpr[3+key]+i*4);
                        std::memcpy(keys+key,words.data(),sizeof(HermiteKey));
                    }
                    const double current=bits_to_double(cpu.fpr[1].bits[0]);
                    require(double_to_bits(double(float(current)))==cpu.fpr[1].bits[0],
                            "original track callback time is not a float value");
                    const float result=derivative?shadow::math::hermite_derivative(keys[0],keys[1],float(current),shadow::math::SubnormalMode::Flush)
                                                 :shadow::math::hermite_value(keys[0],keys[1],float(current),shadow::math::SubnormalMode::Flush);
                    cpu.fpr[1].bits[0]=double_to_bits(double(result));cpu.pc=cpu.lr;
                    continue;
                }
            }
            cpu.run_steps(1);++instructions;
        }
        require(cpu.pc==kReturn && !cpu.stopped() && steps<3000 && cpu.gpr[1]==kStack &&
                cpu.game_clock().now()==time_before,"original track caller did not preserve return/stack/time");
        TrackResult result{};
        for (unsigned i=0;i<16;++i) result.storage[i]=memory.peek32(track+i*4);
        for (unsigned i=0;i<3;++i) result.storage[16+i]=memory.peek32(packed+i*4);
        for (unsigned i=0;i<5;++i) result.storage[19+i]=memory.peek32(globals+i*4);
        for (unsigned i=13;i<32;++i) result.nonvolatile[i-13]=cpu.gpr[i];
        result.value=cpu.fpr[1].bits[0];result.cr=cpu.cr;result.fpscr=cpu.fpscr;result.calls=hits;
        return result;
    }
    void callers() {
        unsigned pairs=0,hits=0;
        for (float time:{-2.0f,-0.001f,0.0f,0.001f,0.5f,0.999f,1.0f,1.001f,1.5f,2.0f,2.5f,3.0f,4.0f})
            for (u32 flags:{0u,1u,8u,9u}) for (bool derivative:{false,true}) {
                const auto original=track(time,flags,derivative,false);
                require(track(time,flags,derivative,true)==original,"native callback changed original compressed-track behavior");
                ++pairs;hits+=original.calls;
            }
        require(hits>0,"track test never exercised its interpolation callbacks");
        std::printf("PASS original compressed-track caller: %u pairs, %u callbacks per arm; key advancement, cached/constant values, endpoint branches and extrapolation; full track/packed input and result equality.\n",pairs,hits);
    }
    void modes(const std::array<u32,9>& values) {
        for (unsigned operation:{0u,1u,2u}) for (bool flush:{false,true}) check(values,operation,flush);
    }
};
}
int main(int argc,char** argv) {
    try {
        require(argc==2,"expected original DOL path");
        require((_mm_getcsr()&0xffc0u)==0x1f80u,"expected nearest rounding with host DAZ/FTZ off");
        Oracle oracle(argv[1]);
        const std::array<u32,9> ordinary{{as_bits(0),as_bits(1),as_bits(1),as_bits(0),as_bits(1),
                                        as_bits(1),as_bits(1),as_bits(1),as_bits(0.5f)}};
        const std::array<u32,24> edge{{0,0x80000000u,1,0x80000001u,0x007fffffu,0x807fffffu,
            0x00800000u,0x80800000u,0x00800001u,0x80800001u,0x3f000000u,0x3f7fffffu,
            0x3f800000u,0x3f800001u,0x40000000u,0xc0000000u,0x7f7fffffu,0xff7fffffu,
            0x7f800000u,0xff800000u,0x7f800001u,0xff800001u,0x7fc12345u,0xffc54321u}};
        for (unsigned slot=0;slot<ordinary.size();++slot) for (u32 word:edge) {
            auto values=ordinary;values[slot]=word;oracle.modes(values);
        }
        for (u32 a:edge) for (u32 b:edge) {
            auto values=ordinary;values[0]=a;values[4]=b;oracle.modes(values);
        }
        u32 seed=0x65a2e9f7u;
        for (unsigned i=0;i<6000;++i) {
            std::array<u32,9> values{};
            for (u32& word:values) word=random_word(seed);
            if (i<3000) {
                for (u32& word:values) word=as_bits(float(int(word%4097)-2048)/32.0f);
            }
            oracle.modes(values);
        }
        oracle.callers();
        std::printf("PASS native Hermite: %u original comparisons; value/derivative/combined, both subnormal modes, "
                    "extrapolation, reversed/equal times, finite/exceptional and random inputs; %llu original instructions. "
                    "Float time contract; caller integration and production retirement pending.\n",
                    oracle.cases,static_cast<unsigned long long>(oracle.instructions));
        return 0;
    } catch (const std::exception& error) { std::fprintf(stderr,"FAIL native Hermite: %s\n",error.what()); return 1; }
}
