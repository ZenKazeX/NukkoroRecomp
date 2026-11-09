#pragma once

// The OS font library (OSFont.c: OSGetFontEncode, OSInitFont, OSGetFontTexture), owned natively
// (docs/agents/os-font.md; docs/GC_REMOVAL_PLAN.md, sdk:unidentified -> os.font).
//
// Original behaviour (main.dol, PAD GUPP8P). The library loads the console's font from the boot ROM and draws the
// OS message screens (disc errors); the game's own copy of that screen (0x8032D4CC, 0x8032D7CC, 0x8032DE9C) and the
// DVD library's ShowMessage (0x8037DF10) are not generated any more, so nothing in the translated program calls
// OSInitFont 0x80375DA0 or OSGetFontTexture 0x80375FAC. The one live entry is OSGetFontEncode 0x80375150, called by
// CARDInit (0x8038DE74), which keeps the result for the card format code (card ID +0x1C, read by the CARD library
// only; native_card.cpp serves that block).
//
// OSGetFontEncode returns the font encoding: 0 ANSI, 1 SJIS. The original answers from a cache (u16 at sda+0x2708,
// 0xFFFF = not yet computed) and otherwise from two inputs: the OS TV-format word at 0x800000CC (only 0 = NTSC
// consults the chip; every other format is ANSI) and bit 1 of the video encoder status register 0xCC00206E (a
// Japanese encoder). It then stores the string parser's address into sda+0x5A88 for OSGetFontTexture. The runtime
// serves the encoder status as 0 (no Japanese encoder; native_video.cpp, "video_encoder_status" - the sites this
// owner replaces), so the answer is 0 for every TV format.
//
// Native: the owner returns font_encode(tv format, encoder status 0). It writes nothing: the cache (sda+0x2708), the
// parser pointer (sda+0x5A88) and the header slots (sda+0x5A80/0x5A84) are read only by the OSFont bodies
// 0x80375150..0x80376044 (a scan of every load / store through r13 in the DOL's text finds no other reader; the
// guest watch over a full Westopolis replay confirms it, gate 3).
//
// OSInitFont and OSGetFontTexture are owned as never entered: the original fills the font buffers by reading the
// font from the boot ROM through __OSReadROM 0x80377C7C, which native_ipl.cpp refuses (this program has no boot
// ROM), so a native version could serve nothing. If the program ever enters them the machine stops with the entry's
// name, exactly as the refused ROM reader and the debug-monitor hand-off do. No result is faked.

#include "gekko/types.h"

#include <cstddef>

namespace gekko {
class Cpu;
}

namespace gekko::os_font {

constexpr u32 kGetFontEncode = 0x80375150u;   // OSGetFontEncode(): served natively
constexpr u32 kInitFont = 0x80375DA0u;        // OSInitFont(fontData): never entered, stops
constexpr u32 kGetFontTexture = 0x80375FACu;  // OSGetFontTexture(string, &image, &x, &y, &width): never entered, stops

constexpr u32 kTvFormatWord = 0x800000CCu;       // OS TV format (0 NTSC, 1 PAL, 2 MPAL, 5 EURGB60 ...)
constexpr u16 kEncodeAnsi = 0u;
constexpr u16 kEncodeSjis = 1u;
constexpr u16 kEncoderJapaneseBit = 0x0002u;     // bit 1 of the video encoder status 0xCC00206E
constexpr u16 kEncoderStatus = 0u;               // what the video contract serves: no Japanese encoder

// The font encoding the original computes (uncached) for a TV-format word and an encoder status.
u16 font_encode(u32 tv_format, u16 encoder_status) noexcept;

// The guest entries this owner serves: must equal its rows of extract/native-owners.txt (native_file_api_tests).
struct Owners {
    struct Address {
        u32 address;
        const char* name;
    };
    static const Address* addresses(std::size_t& count) noexcept {
        static constexpr Address kAddresses[] = {{kGetFontEncode, "OSGetFontEncode"},
                                                 {kInitFont, "OSInitFont"},
                                                 {kGetFontTexture, "OSGetFontTexture"}};
        count = sizeof(kAddresses) / sizeof(kAddresses[0]);
        return kAddresses;
    }
};

// OSGetFontEncode: r3 = the encoding. Reads the TV-format word; writes nothing else.
void serve_get_font_encode(Cpu& cpu) noexcept;
// OSInitFont / OSGetFontTexture entered: stops the run with the entry's name.
void serve_never_entered(Cpu& cpu, const char* name) noexcept;

// Installs the three owners on the CPU.
void install(Cpu& cpu);

}  // namespace gekko::os_font
