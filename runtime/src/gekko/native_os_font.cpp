// The OS font library, native. Contract: include/gekko/native_os_font.h.

#include "gekko/native_os_font.h"

#include <string>

#include "gekko/cpu.h"
#include "gekko/guest_memory.h"

namespace gekko::os_font {

u16 font_encode(u32 tv_format, u16 encoder_status) noexcept {
    // 0x80375150: cmpwi r0,0 on the TV-format word; only 0 (NTSC) reads the encoder status (bit 1 -> SJIS).
    if (tv_format != 0u) return kEncodeAnsi;
    return (encoder_status & kEncoderJapaneseBit) != 0u ? kEncodeSjis : kEncodeAnsi;
}

void serve_get_font_encode(Cpu& cpu) noexcept {
    const u32 tv_format = cpu.guest_memory().peek32(kTvFormatWord);
    cpu.gpr[3] = font_encode(tv_format, kEncoderStatus);
}

void serve_never_entered(Cpu& cpu, const char* name) noexcept {
    cpu.request_stop(StopReason::ExecutionFault,
                     std::string("native OS font: ") + name +
                         " was entered; it loads the font from the boot ROM (__OSReadROM), which this program does not "
                         "have, and nothing in the program calls it");
}

void install(Cpu& cpu) {
    cpu.install_hle(kGetFontEncode, [](Cpu& c) {
        serve_get_font_encode(c);
        return true;  // an owner never declines
    }, "native OS font: OSGetFontEncode");
    cpu.install_hle(kInitFont, [](Cpu& c) {
        serve_never_entered(c, "OSInitFont");
        return true;
    }, "native OS font: OSInitFont (never entered)");
    cpu.install_hle(kGetFontTexture, [](Cpu& c) {
        serve_never_entered(c, "OSGetFontTexture");
        return true;
    }, "native OS font: OSGetFontTexture (never entered)");
}

}  // namespace gekko::os_font
