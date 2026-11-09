// liveness-roots: reference-interpreter-only
#pragma once

// Test-only execution of the ORIGINAL output microcode, read from main.dol.
// Bounded ISA subset, flat nonwrapping data arrays, synchronous fixture DMA.
// Unknown instructions/register uses fail. No timing, cache or hardware proof.
// Instruction encodings and register behavior checked against Dolphin's primary
// DSPTables.cpp / Interpreter sources; docs/AUDIO_OUTPUT_CONTRACT.md cites them.
#include <array>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <functional>
#include <iterator>
#include <stdexcept>
#include <vector>

namespace audio_reference {
using u16 = std::uint16_t; using u32 = std::uint32_t;
using s32 = std::int32_t; using s64 = std::int64_t;
inline void require(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
inline s64 signed_bits(std::uint64_t value, unsigned width) {
    const auto sign = std::uint64_t{1} << (width - 1);
    return s64(value & (sign - 1)) - s64(value & sign);
}

class OutputWords {
public:
    explicit OutputWords(const char* path) {
        std::ifstream file(path, std::ios::binary);
        const std::vector<unsigned char> raw{std::istreambuf_iterator<char>(file), {}};
        std::uint64_t hash = 0xCBF29CE484222325ull;
        for (auto byte : raw) hash = (hash ^ byte) * 0x100000001B3ull;
        require(hash == 0xD2CFB4A1CA897F9Bull, "unexpected original DOL");
        const auto word = [&](std::size_t at) { require(at + 4 <= raw.size(), "DOL bounds");
            return (u32(raw[at]) << 24) | (u32(raw[at+1]) << 16) | (u32(raw[at+2]) << 8) | raw[at+3]; };
        for (unsigned pc = 0; pc < code_.size(); ++pc) {
            const u32 address = 0x80564E60u + pc * 2;
            bool found = false;
            for (unsigned section = 0; section < 18; ++section) {
                const u32 base = word(0x48 + section*4), size = word(0x90 + section*4);
                if (address < base || std::uint64_t(address) + 2 > std::uint64_t(base) + size) continue;
                const auto at = std::size_t(word(section*4)) + address - base;
                require(at + 2 <= raw.size(), "DOL section bounds");
                code_[pc] = u16((u16(raw[at]) << 8) | raw[at+1]); found = true; break;
            }
            require(found, "missing output microcode word");
        }
    }
    std::array<u16, 0x1000> data{};
    std::array<u16, 0x1000> host{}; // byte addresses 0x10000..0x11fff
    // Optional bounded external memory for composition with the original CPU
    // oracle. It supplies transfers only; instruction execution is unchanged.
    std::function<void(bool,u32,u16*,unsigned)> transfer;
    std::uint64_t instructions = 0;
    std::vector<std::array<u32, 4>> transfers; // direction, host byte address, data word address, bytes
    u16 instruction(unsigned pc) const { return code_.at(pc); }
    void put32(unsigned at, s32 value) { data.at(at) = u16(u32(value) >> 16); data.at(at+1) = u16(value); }
    s32 get32(unsigned at) const { return s32((u32(data.at(at)) << 16) | data.at(at+1)); }
    void run(unsigned entry, const std::vector<u32>& args) {
        regs_.fill(0); acc_.fill(0); loops_.clear(); calls_.clear(); io_.fill(0); transfers.clear();
        wide_ = true; logic_zero_ = false; regs_[0] = 0xC00;
        for (unsigned i = 0; i < args.size(); ++i) put32(0xC00 + i*2, s32(args[i]));
        unsigned pc = entry;
        for (unsigned steps = 0; steps < 100000; ++steps) {
            if (pc == 0x68) return; // original command dispatcher, not a substituted routine
            const unsigned here = pc; const u16 op = code_.at(pc++); ++instructions;
            const auto next = [&] { return code_.at(pc++); };
            std::vector<std::array<u16, 2>> pending;
            const auto queue = [&](u16 r, u16 v) { pending.push_back({r,v}); };
            const auto load = [&](u16 r, u16 v) { write_reg(r,v); if (wide_ && r >= 30) acc_[r-30] = signed_bits(v,16)*65536; };
            const auto increment = [&](unsigned r) { require(regs_[r] < 0x1000, "non-flat address increment"); return u16(regs_[r]+1); };
            // Extended loads/stores see the register state before the main op.
            if (op >= 0x3000) {
                const unsigned ext = op & (op < 0x4000 ? 0x7f : 0xff);
                if (ext == 0) {}
                else if ((ext & 0xc4) == 0x40) {
                    const unsigned r = 24 + ((ext >> 3)&7), ar = ext&3;
                    const u16 value = read_data(regs_[ar]); queue(u16(r),value); queue(u16(ar),increment(ar));
                    if (wide_ && r >= 30) { queue(u16(r-14),value&0x8000 ? 0xffff : 0); queue(u16(r-2),0); }
                } else if ((ext & 0xe4) == 0x20) {
                    const unsigned ar = ext&3; write_data(regs_[ar],read_reg(28 + ((ext>>3)&3)));
                    queue(u16(ar),increment(ar));
                } else if ((ext & 0xcc) == 0xc0 && (ext&3) != 3) {
                    const unsigned ar = ext&3;
                    require((regs_[ar]>>10) != (regs_[3]>>10), "dual load in same memory bank unsupported");
                    queue(u16(24+((ext>>4)&2)),read_data(regs_[ar]));
                    queue(u16(25+((ext>>3)&2)),read_data(regs_[3]));
                    queue(u16(ar),increment(ar)); queue(3,increment(3));
                } else { fail(here,op,"extension"); }
            }
            if ((op & 0xffe0) == 0x0080) load(op&31,next());
            else if ((op & 0xffe0) == 0x00c0) load(op&31,read_data(next()));
            else if ((op & 0xffe0) == 0x00e0) write_data(next(),read_reg(op&31));
            else if ((op & 0xf800) == 0x0800) load(24+((op>>8)&7),u16(signed_bits(op&255,8)));
            else if ((op & 0xfc00) == 0x1c00) load((op>>5)&31,read_reg(op&31));
            else if ((op & 0xff00) == 0x1600) write_data(u16(0xff00 | (op&255)),next());
            else if ((op & 0xf800) == 0x2000) load(24+((op>>8)&7),read_data(u16(0xff00 | (op&255))));
            else if ((op & 0xfc00) == 0x2c00) write_data(u16(0xff00 | (op&255)),read_reg(28+((op>>8)&3)));
            else if ((op & 0xff80) == 0x1900) { const unsigned ar=(op>>5)&3; const auto v=read_data(regs_[ar]);
                load(op&31,v); regs_[ar]=increment(ar); }
            else if ((op & 0xff80) == 0x1b00) { const unsigned ar=(op>>5)&3;
                write_data(regs_[ar],read_reg(op&31)); regs_[ar]=increment(ar); }
            else if ((op & 0xfeff) == 0x02a0) logic_zero_ = (read_reg(30+((op>>8)&1)) & next()) == 0;
            else if (op == 0x029c || op == 0x029f) { const auto dest=next(); if (op==0x029f || !logic_zero_) pc=dest; }
            else if (op == 0x02bf) { const auto dest=next(); calls_.push_back(pc); pc=dest; }
            else if (op == 0x02df) { require(!calls_.empty(), "unexpected return"); pc=calls_.back(); calls_.pop_back(); }
            else if ((op & 0xffe0) == 0x0060 || (op & 0xff00) == 0x1100) {
                const unsigned count=(op&0xff00)==0x1100 ? op&255 : read_reg(op&31); const auto end=next();
                require(count>0, "zero block loop unsupported"); loops_.push_back({pc,end,count});
            }
            else if ((op & 0xf700) == 0x8100) acc_[(op>>11)&1]=0;
            else if ((op & 0xff00) == 0x8e00) wide_=false;
            else if ((op & 0xff00) == 0x8f00) wide_=true;
            else if ((op & 0xf700) == 0x8000) {}
            else if ((op & 0xfc00) == 0x4800) { const unsigned d=(op>>8)&1,s=(op>>9)&1;
                acc_[d]=signed_bits(std::uint64_t(acc_[d]+signed_bits((u32(regs_[26+s])<<16)|regs_[24+s],32)),40); }
            else if ((op & 0xfe00) == 0x4c00) { const unsigned d=(op>>8)&1; acc_[d]=signed_bits(std::uint64_t(acc_[d]+acc_[1-d]),40); }
            else if ((op & 0xfc00) == 0x6800) acc_[(op>>8)&1]=signed_bits((u32(regs_[26+((op>>9)&1)])<<16)|regs_[24+((op>>9)&1)],32);
            else if ((op & 0xfe00) == 0x7c00) { const unsigned d=(op>>8)&1; acc_[d]=signed_bits(std::uint64_t(-acc_[d]),40); }
            else fail(here,op,"primary");
            for (const auto& w : pending) write_reg(w[0],w[1]);
            if (!loops_.empty() && here==loops_.back()[1]) {
                if (--loops_.back()[2]) pc=loops_.back()[0]; else loops_.pop_back();
            }
        }
        throw std::runtime_error("output microcode step limit");
    }
private:
    [[noreturn]] static void fail(unsigned pc,u16 op,const char* kind) {
        char text[100]; std::snprintf(text,sizeof(text),"unknown %s at %04x: %04x",kind,pc,op); throw std::runtime_error(text);
    }
    u16 read_reg(unsigned r) const {
        if (r==16 || r==17) return u16(acc_[r-16]>>32);
        if (r==28 || r==29) return u16(acc_[r-28]);
        if (r>=30) { const auto v=acc_[r-30]; if (wide_ && (v>INT32_MAX || v<INT32_MIN)) return v<0 ? 0x8000 : 0x7fff;
            return u16(v>>16); }
        require(r<12 || (r>=24 && r<28),"unsupported register read"); return regs_[r];
    }
    void write_reg(unsigned r,u16 v) {
        if (r==16 || r==17) acc_[r-16]=signed_bits((std::uint64_t(v&255)<<32)|(std::uint64_t(acc_[r-16])&0xffffffffu),40);
        else if (r>=28) { const unsigned d=r&1, shift=r>=30 ? 16 : 0;
            acc_[d]=signed_bits((std::uint64_t(acc_[d])&~(std::uint64_t(0xffff)<<shift))|(std::uint64_t(v)<<shift),40); }
        else { require(r<12 || (r>=24 && r<28),"unsupported register write"); regs_[r]=v; }
    }
    u16 read_data(u16 address) const { if (address==0xffc9) return 0; return data.at(address); }
    void write_data(u16 address,u16 value) {
        if (address<0x1000) { data.at(address)=value; return; }
        require(address>=0xffc9 && address<=0xffcf,"unsupported I/O write"); io_[address-0xffc9]=value;
        if (address!=0xffcb) return;
        const u32 from=(u32(io_[5])<<16)|io_[6], local=io_[4];
        require(io_[0]<=1 && (from&1)==0 && (value&1)==0 &&
                local+value/2<=data.size(),"invalid fixture transfer");
        transfers.push_back({io_[0],from,local,value});
        if (transfer) { transfer(io_[0]!=0,from,data.data()+local,value/2); return; }
        require(from>=0x10000 && std::uint64_t(from)+value<=0x12000,"invalid fixture host range");
        for (unsigned i=0; i<unsigned(value)/2; ++i) {
            auto& h=host.at((from-0x10000)/2+i); auto& d=data.at(local+i);
            if (io_[0]) h=d; else d=h;
        }
    }
    std::array<u16,0x1000> code_{};
    std::array<u16,32> regs_{};
    std::array<s64,2> acc_{};
    std::array<u16,7> io_{};
    std::vector<unsigned> calls_;
    std::vector<std::array<unsigned,3>> loops_;
    bool wide_=false, logic_zero_=false;
};
} // namespace audio_reference
