// liveness-roots: reference-interpreter-only
#include "audio/output_stage.h"
#include "support/audio_output_reference.h"

#include <algorithm>
#include <cstdio>

using shadow::audio::OutputStage;
using namespace audio_reference;

int main(int argc, char** argv) {
    try {
        require(argc==2,"usage: audio_output_contract_tests main.dol");
        OutputWords original(argv[1]);
        require(original.instruction(0xC9D+7)==0x066A && original.instruction(0xC9D+17)==0x020B &&
                original.instruction(0xC9D+4)==0x0509 && original.instruction(0xC9D+5)==0x051D,
                "output dispatch table changed");
        u32 random=0x71AC32u;
        const auto sample=[&]() { random^=random<<13; random^=random>>17; random^=random<<5;
            return s32(random&0xFFFFFFu)-0x800000; };
        unsigned folds=0, returns=0;
        for (auto mode : {OutputStage::Mode::Stereo,OutputStage::Mode::Opposite}) {
            OutputStage stage;
            for (unsigned frame=0; frame<100; ++frame) {
                OutputStage::Buffer left{},right{},surround{},zero{};
                const auto before=stage.history();
                for (unsigned i=0;i<160;++i) {
                    const auto v=before.surround[i];
                    original.host[i*2]=u16(u32(v)>>16); original.host[i*2+1]=u16(v);
                    original.put32(i*2,123); original.put32(0x140+i*2,-456);
                    original.put32(0x280+i*2,789);
                }
                original.run(mode==OutputStage::Mode::Stereo ? 0x066A : 0x020B,{0x10000});
                stage.begin(mode,left,right,surround);
                for (unsigned i=0;i<160;++i) {
                    require(left[i]==original.get32(i*2) && right[i]==original.get32(0x140+i*2) &&
                            surround[i]==original.get32(0x280+i*2),"native surround differs from original words");
                    surround[i]=sample();
                }
                require(original.transfers.size()==2 && original.transfers[0][3]==32 &&
                        original.transfers[1][3]==608,"surround transfer bounds changed");
                stage.finish(mode,left,right,surround,zero,zero);
                if (frame%7==0) { OutputStage restored; restored.restore(stage.history()); stage=restored; }
                ++folds;
            }
            stage.reset();
            require(std::all_of(stage.history().surround.begin(),stage.history().surround.end(),
                    [](auto v) { return v==0; }),"surround reset retained history");
        }
        // Recover send/return composition from original words; this part is not
        // connected to the production effect owner yet. Values span 24-bit mix
        // headroom; this does not assert behavior for arbitrary corrupt buses.
        for (unsigned bus=0;bus<2;++bus) for (unsigned frame=0;frame<32;++frame) {
            std::array<s32,480> main{},send{},returned{};
            const unsigned send_base=bus ? 0x7C0 : 0x400;
            for (unsigned i=0;i<480;++i) {
                main[i]=sample()/4; send[i]=sample()/4; returned[i]=sample()/4;
                original.put32(i*2,main[i]); original.put32(send_base+i*2,send[i]);
                original.host[0x800+i*2]=u16(u32(returned[i])>>16); original.host[0x800+i*2+1]=u16(returned[i]);
            }
            original.run(bus ? 0x051D : 0x0509,{0x10000,0x11000});
            for (unsigned i=0;i<480;++i) {
                require(original.get32(i*2)==main[i]+returned[i],"original auxiliary return composition differs");
                require(s32((u32(original.host[i*2])<<16)|original.host[i*2+1])==send[i],"auxiliary send differs");
            }
            ++returns;
        }
        // Mode 2 routes four effect returns, then matrix output combines the
        // returned B pair with direct main L/R. Exercise both original programs.
        unsigned matrix_blocks=0, four_channel_blocks=0;
        OutputStage matrix;
        std::array<OutputStage::Buffer,2> previous_matrix{};
        for (unsigned frame=0;frame<32;++frame) {
            OutputStage::Buffer left{},right{},surround{},bl{},br{};
            const auto previous=previous_matrix;
            for (unsigned i=0;i<160;++i) {
                left[i]=sample()/4; right[i]=sample()/4; surround[i]=sample()/4;
                bl[i]=sample()/4; br[i]=sample()/4;
                original.put32(i*2,left[i]); original.put32(0x140+i*2,right[i]);
                original.put32(0x280+i*2,surround[i]);
                original.put32(0x7C0+i*2,bl[i]); original.put32(0x900+i*2,br[i]);
                for (unsigned c=0;c<2;++c) {
                    const auto v=previous[c][i];
                    original.host[0x800+c*320+i*2]=u16(u32(v)>>16);
                    original.host[0x801+c*320+i*2]=u16(v);
                }
            }
            original.run(0x0DA1,{0x10000,0x11000});
            matrix.finish(OutputStage::Mode::Matrix,left,right,surround,bl,br);
            for (unsigned i=0;i<160;++i) {
                require(left[i]==original.get32(i*2) && right[i]==original.get32(0x140+i*2) &&
                        surround[i]==original.get32(0x280+i*2),"matrix output differs from original words");
                for (unsigned c=0;c<2;++c) {
                    const auto at=c*320+i*2;
                    require(s32((u32(original.host[at])<<16)|original.host[at+1])==(c ? br[i] : bl[i]),
                            "matrix send differs");
                }
            }
            previous_matrix={bl,br};
            ++matrix_blocks;
        }
        for (unsigned frame=0;frame<32;++frame) {
            std::array<std::array<s32,160>,4> main{},send{},returned{};
            constexpr unsigned destinations[]={0,0x140,0x7C0,0x900};
            constexpr unsigned send_sources[]={0x400,0x540,0x680,0xA40};
            for (unsigned c=0;c<4;++c) for (unsigned i=0;i<160;++i) {
                main[c][i]=sample()/4; send[c][i]=sample()/4; returned[c][i]=sample()/4;
                original.put32(destinations[c]+i*2,main[c][i]);
                original.put32(send_sources[c]+i*2,send[c][i]);
                original.host[0x800+c*320+i*2]=u16(u32(returned[c][i])>>16);
                original.host[0x801+c*320+i*2]=u16(returned[c][i]);
            }
            original.run(0x0E17,{0x10000,0x10780,0x11000,0x11280,0x11500,0x11780});
            for (unsigned c=0;c<4;++c) for (unsigned i=0;i<160;++i) {
                require(original.get32(destinations[c]+i*2)==main[c][i]+returned[c][i],
                        "four-channel return destination differs");
                const auto at=c*320+i*2;
                require(s32((u32(original.host[at])<<16)|original.host[at+1])==send[c][i],
                        "four-channel send differs");
            }
            ++four_channel_blocks;
        }
        std::printf("PASS output words: %u surround blocks, %u three-channel send/returns, %u matrix blocks, %u four-channel returns, %llu instructions\n",
                    folds,returns,matrix_blocks,four_channel_blocks,static_cast<unsigned long long>(original.instructions));
        std::puts("Scope: original microcode + software ISA semantics; fixture transfers are synchronous, no device/timing proof.");
        return 0;
    } catch (const std::exception& error) { std::fprintf(stderr,"FAIL output contract: %s\n",error.what()); return 1; }
}
