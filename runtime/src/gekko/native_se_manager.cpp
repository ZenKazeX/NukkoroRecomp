// The game's sound-effect manager, owned natively (gekko/native_se_manager.h). Each function names the original it
// replaces; docs/SE_MANAGER_MAP.md has the field layouts.

#include "gekko/native_se_manager.h"

#include "gekko/cpu.h"
#include "gekko/guest_memory.h"
#include "gekko/native_se_math.h"
#include "gekko/ppc_ops.h"

namespace gekko::se_manager {

namespace {

// Manager fields.
constexpr u32 kSound = 0x0u, kChannelArray = 0x4u, kTables = 0x8u, kNormalCursor = 0xCu, kSpecialCursor = 0xDu;
constexpr u32 kPauseMode = 0x10u, kGates = 0x14u, kRingCooldown = 0x18u;
constexpr u32 kPauseGate = 0u, kRequestGate = 1u;
// Channel fields.
constexpr u32 kIndex = 0x0u, kBank = 0x1u, kProgram = 0x2u, kPriority = 0x3u, kPan = 0x4u, kSurround = 0x5u;
constexpr u32 kVolume = 0x6u, kVolumeOffset = 0x7u, kVolumeCeiling = 0x8u, kNoteMarks = 0x9u, kBend = 0xAu;
constexpr u32 kFramesLeft = 0xCu, kReload = 0xEu, kAge = 0x10u, kId = 0x12u, kFlags = 0x14u, kOwner = 0x18u;
constexpr u32 kPosition = 0x1Cu, kMaxDistance = 0x28u, kPreviousDistance = 0x2Cu, kRow = 0x30u;
// Row fields.
constexpr u32 kRowVolumePercent = 0xAu, kRowDuration = 0xDu, kRowFlags = 0xEu, kRowPriority = 0xFu;
// Channel flags (c + 0x14).
constexpr u32 kStartPending = 0x1u, kRefreshed = 0x2u, kStopAtExpiry = 0x4u, kPositional = 0x10u;
constexpr u32 kOneShotVolume = 0x20u, kPositionalPan = 0x40u, kPanOnce = 0x80u, kDoppler = 0x100u;
constexpr u32 kBendOnce = 0x200u, kPositionalAtStart = 0x400u, kRetrigger = 0x4000u;
// Globals.
constexpr u32 kRingToggle = 0x805F0A18u, kRingGuard = 0x805F0A1Cu;  // r13 + 0x4518 / + 0x451C
constexpr u32 kRingPan = 0x8053DF1Cu;                               // {0xC0, 0x40}
constexpr u32 kRingId = 0xE00Bu;
constexpr u32 kRingCooldownBits = 0x3D088889u;                      // K(-0x38FC) = 0.0333333351
constexpr u32 kDefaultDuration = 120u;
constexpr u32 kSingleZero = 0x00000000u;                            // K(-0x38CC)
constexpr u32 kFirstSpecial = 0x26u, kSpecialEnd = 0x2Bu, kNormalCount = 0x20u;

// The gate test 0x80014E34 plus the exemption lists: true when the gate bit is clear, or the id is an exempt
// system sound (nibble 0xE, id & 0xFFFF in E000.. for `exempt` ids: the request list 0x8053DF08 holds ten,
// the pause list 0x804D1590 six).
bool passes(GuestMemory& m, u32 manager, u32 bit, u32 id, u32 exempt) noexcept {
    if (((m.peek32(manager + kGates) >> bit) & 1u) == 0u) return true;
    if (((id >> 12) & 0xFu) != 0xEu) return false;
    return (id & 0xFFFFu) - 0xE000u < exempt;
}
bool request_allowed(GuestMemory& m, u32 manager, u32 id) noexcept { return passes(m, manager, kRequestGate, id, 10u); }
bool pause_allowed(GuestMemory& m, u32 manager, u32 id) noexcept { return passes(m, manager, kPauseGate, id, 6u); }

// lfs / stfs: the Vec3 copy 0x800091D0 and single constants.
u32 copy_single(u32 word) noexcept { return double_to_single_bits_flush(single_to_double_bits(word)); }
void copy_vec3(GuestMemory& m, u32 to, u32 from) noexcept {
    for (u32 i = 0; i < 12u; i += 4u) m.write32(to + i, copy_single(m.peek32(from + i)));
}
u64 single(u32 bits) noexcept { return single_to_double_bits(bits); }

// The original's int -> single conversion: (double)(0x43300000:x^0x80000000) - (2^52 + 2^31), then fsubs.
u64 single_from_int(Cpu& cpu, s32 value) noexcept {
    const u64 biased = (u64{0x43300000u} << 32) | (static_cast<u32>(value) ^ 0x80000000u);
    return ppc::fps_sub(cpu, biased, 0x4330000080000000ull);
}

struct Context {
    Cpu& cpu;
    GuestMemory& m;
    Services& services;

    Report& report() noexcept { return services.report; }

    // Route 0x801D9BEC.
    static void route(s32 index, s32& port, u32& channel) noexcept {
        if (index > static_cast<s32>(kFirstSpecial)) {
            port = static_cast<s8>(index - 0x23);
            channel = 0u;
        } else {
            port = index >> 4;
            channel = static_cast<u32>(index) & 0xFu;
        }
    }

    // The sound object behind GetSoundManager()->sound (the manager thunks 0x801D60B0..0x801D6238: nothing when it is
    // missing), for a port the native methods serve.
    template <typename F>
    void sound(u32 manager, s32 port, F&& call) noexcept {
        const u32 object = m.peek32(manager + kSound);
        if (object == 0u) return;
        if (port < 0 || port >= 3) {
            ++report().unsupported_port;
            return;
        }
        gcax_midi::SoundObject o{cpu, object, services.midi};
        call(o);
    }

    // The volume sent for attenuation `v` (s8): unchanged without a row volume, else
    // trunc(clamp(fmadds(127, fmuls(0.01, (float)rowA), (float)v), -127, 127)) (0x801D8E10.., 0x801D8FB0..).
    s32 scaled_volume(u32 row, s32 raw) noexcept {
        const s32 percent = static_cast<s16>(m.peek16(row + kRowVolumePercent));
        if (percent == 0) return raw;
        u64 x = ppc::fps_mul(cpu, single(0x3C23D70Au), single_from_int(cpu, percent));  // K(-0x38C4) = 0.01
        x = ppc::fps_madd(cpu, single(0x42FE0000u), x, single_from_int(cpu, static_cast<s8>(raw)));  // 127
        const double v = bits_to_double(x);
        if (v < -127.0) x = single(0xC2FE0000u);       // K(-0x38C0)
        else if (v >= 127.0) x = single(0x42FE0000u);
        return static_cast<s32>(static_cast<u32>(ppc::fctiw(cpu, x, true)));
    }

    // SendPan 0x801D934C: CC62 instead of pan when surround output has a surround value.
    void send_pan(u32 manager, u32 c) noexcept {
        s32 port;
        u32 channel;
        route(static_cast<s8>(m.peek8(c + kIndex)), port, channel);
        const u32 surround = m.peek8(c + kSurround);
        if (static_cast<s32>(m.peek32(gcax_midi::kOutputConfig + 0x78u)) == 2 && static_cast<s8>(surround) != 0)
            sound(manager, port, [&](gcax_midi::SoundObject& o) { o.surround(static_cast<s32>(surround), port, channel); });
        else {
            const u32 pan = m.peek8(c + kPan);
            sound(manager, port, [&](gcax_midi::SoundObject& o) { o.pan(static_cast<s32>(pan), port, channel); });
        }
    }

    u32 flags(u32 c) noexcept { return m.peek32(c + kFlags); }
    void set_flags(u32 c, u32 value) noexcept { m.write32(c + kFlags, value); }

    // UpdateChan 0x801D8D2C. `manager` = GetSoundManager()'s result (every vt call goes through it).
    void update_channel(u32 manager, u32 c) noexcept {
        ++report().channel_updates;
        s32 port;
        u32 ch;
        const s32 index = static_cast<s8>(m.peek8(c + kIndex));
        route(index, port, ch);
        const u32 entry_flags = flags(c);
        if (entry_flags & kStartPending) {
            m.write32(c + kPreviousDistance, kSingleZero);
            if (index < static_cast<s32>(kFirstSpecial)) {
                const s32 bank = m.peek8(c + kBank), program = m.peek8(c + kProgram);
                sound(manager, port, [&](gcax_midi::SoundObject& o) { o.set_bank_program(port, ch, bank, program); });
            }
            if (flags(c) & kPositional) {
                m.write8(c + kVolume, static_cast<u8>(se_math::pos_volume(cpu, c)));
                if (flags(c) & kPositionalAtStart) {
                    set_flags(c, flags(c) & ~kPositional);
                    set_flags(c, flags(c) & ~kPositionalAtStart);
                }
            } else if (flags(c) & kOneShotVolume) {
                m.write8(c + kVolume, m.peek8(c + kVolumeOffset));
                set_flags(c, flags(c) & ~kOneShotVolume);
            } else {
                m.write8(c + kVolume, m.peek8(c + kVolumeOffset));
            }
            const s32 volume = scaled_volume(m.peek32(c + kRow), m.peek8(c + kVolume));
            sound(manager, port, [&](gcax_midi::SoundObject& o) { o.volume(volume, port, ch); });
            if (flags(c) & kPositionalPan) {
                s8 surround = static_cast<s8>(m.peek8(c + kSurround));
                const s32 pan = se_math::pos_pan(cpu, c, &surround);
                m.write8(c + kSurround, static_cast<u8>(surround));
                m.write8(c + kPan, static_cast<u8>(pan));
                send_pan(manager, c);
            }
            if (flags(c) & kPanOnce) {
                set_flags(c, flags(c) & ~kPanOnce);
                send_pan(manager, c);
            }
            if (flags(c) & kDoppler) m.write16(c + kBend, static_cast<u16>(se_math::doppler_bend(cpu, c)));
            if (flags(c) & kBendOnce) set_flags(c, flags(c) & ~kBendOnce);
            const s32 bend = static_cast<s16>(m.peek16(c + kBend));
            sound(manager, port, [&](gcax_midi::SoundObject& o) { o.bend(bend, port, ch); });
            const u32 priority = m.peek8(c + kPriority);
            sound(manager, port, [&](gcax_midi::SoundObject& o) { o.note_on(priority, port, ch); });
            set_flags(c, flags(c) & ~kStartPending);
            m.write8(c + kNoteMarks, static_cast<u8>(m.peek8(c + kNoteMarks) | 0x80u));  // K(-0x38D0) = 0x80
            m.write16(c + kAge, static_cast<u16>(m.peek16(c + kAge) + 1u));
            return;
        }
        if (static_cast<s16>(m.peek16(c + kFramesLeft)) != 0) {
            if (entry_flags & kPositional) {
                const s32 n = static_cast<s8>(se_math::pos_volume(cpu, c));
                if (static_cast<s8>(m.peek8(c + kVolume)) != n) {
                    const s32 volume = scaled_volume(m.peek32(c + kRow), n);
                    sound(manager, port, [&](gcax_midi::SoundObject& o) { o.volume(volume, port, ch); });
                    m.write8(c + kVolume, static_cast<u8>(n));
                }
                if ((m.peek8(m.peek32(c + kRow) + kRowFlags) & 0x10u) && static_cast<s8>(m.peek8(c + kVolume)) < -110)
                    m.write16(c + kFramesLeft, 1u);
            }
            if (flags(c) & kOneShotVolume) {  // never set in shipped code
                const s32 n = static_cast<s8>(m.peek8(c + kVolumeOffset));
                if (static_cast<s8>(m.peek8(c + kVolume)) != n) {
                    const s32 volume = scaled_volume(m.peek32(c + kRow), n);
                    sound(manager, port, [&](gcax_midi::SoundObject& o) { o.volume(volume, port, ch); });
                    m.write8(c + kVolume, static_cast<u8>(n));
                }
                set_flags(c, flags(c) & ~kOneShotVolume);
            }
            if (flags(c) & kPositionalPan) {
                s8 surround = 0;
                const s32 pan = static_cast<s8>(se_math::pos_pan(cpu, c, &surround));
                if (static_cast<s8>(m.peek8(c + kPan)) != pan || static_cast<s8>(m.peek8(c + kSurround)) != surround) {
                    send_pan(manager, c);  // the OLD values: a one-frame lag
                    m.write8(c + kPan, static_cast<u8>(pan));
                    m.write8(c + kSurround, static_cast<u8>(surround));
                }
            }
            if (flags(c) & kPanOnce) {
                const s32 pan = m.peek8(c + kPan);
                sound(manager, port, [&](gcax_midi::SoundObject& o) { o.pan(pan, port, ch); });
                set_flags(c, flags(c) & ~kPanOnce);
            }
            if (flags(c) & kDoppler) {
                const s32 b = se_math::doppler_bend(cpu, c);
                const s32 old = static_cast<s16>(m.peek16(c + kBend));
                if (old != static_cast<s16>(b)) {
                    sound(manager, port, [&](gcax_midi::SoundObject& o) { o.bend(old, port, ch); });  // OLD value
                    m.write16(c + kBend, static_cast<u16>(b));
                }
            }
            if (flags(c) & kBendOnce) {  // never set in shipped code
                const s32 bend = static_cast<s16>(m.peek16(c + kBend));
                sound(manager, port, [&](gcax_midi::SoundObject& o) { o.bend(bend, port, ch); });
                set_flags(c, flags(c) & ~kBendOnce);
            }
            m.write16(c + kFramesLeft, static_cast<u16>(m.peek16(c + kFramesLeft) - 1u));
            m.write16(c + kAge, static_cast<u16>(m.peek16(c + kAge) + 1u));
        }
        if (static_cast<s16>(m.peek16(c + kFramesLeft)) != 0) return;
        const u32 f = flags(c);
        if (f & kRetrigger) {
            ++report().retriggered;
            set_flags(c, f | kStartPending);
            m.write16(c + kFramesLeft, m.peek16(c + kReload));
            m.write8(c + kNoteMarks, 0x1Fu);
            set_flags(c, flags(c) & ~kRetrigger);
            set_flags(c, flags(c) | kStopAtExpiry);
        } else if (f & kStopAtExpiry) {
            ++report().expired;
            if (m.peek8(c + kPan) != 0u || m.peek8(c + kSurround) != 0u) {
                sound(manager, port, [&](gcax_midi::SoundObject& o) { o.reset_pan(port, ch); });
                m.write8(c + kPan, 0u);
                m.write8(c + kSurround, 0u);
            }
            if (m.peek16(c + kBend) != 0u) {
                sound(manager, port, [&](gcax_midi::SoundObject& o) { o.bend(0, port, ch); });
                m.write16(c + kBend, 0u);
            }
            sound(manager, port, [&](gcax_midi::SoundObject& o) { o.note_off(port, ch); });
            m.write32(c + kOwner, 0u);
            m.write16(c + kId, 0u);
            m.write16(c + kFramesLeft, 0u);
            m.write32(c + kFlags, 0u);
            m.write16(c + kAge, 0u);
            m.write32(c + kRow, 0u);
        }
    }

    // Alloc 0x801D5A18 -> channel index or -1.
    s32 allocate(u32 manager, u32 row, u32 priority) noexcept {
        if (row == 0u) return -1;
        priority &= 0xFFu;
        const u32 channels = m.peek32(manager + kChannelArray);
        const u32 new_id = m.peek32(row + 4u) & 0xFFFFu;
        s32 pick = -1;
        if (m.peek8(row + kRowFlags) & 2u) {
            // The special ring: five probes from the cursor over 0x26..0x2A. The victim is the last channel probed
            // with a priority <= the new one (the equal-priority age bookkeeping never runs: r9 is never set).
            ++report().special_ring;
            s32 i = static_cast<s8>(m.peek8(manager + kSpecialCursor));
            s32 lower = -1;
            for (s32 probes = 0x2B; probes > 0x26; --probes) {
                const u32 c = channels + static_cast<u32>(i) * kChannelBytes;
                if (m.peek32(c + kRow) == 0u) {
                    pick = i;
                    break;
                }
                if (m.peek8(c + kPriority) <= priority) lower = i;
                i = static_cast<s8>(i + 1);
                if (i >= static_cast<s32>(kSpecialEnd)) i = static_cast<s32>(kFirstSpecial);
            }
            if (pick < 0) {
                if (lower > 0) pick = lower;
                else return -1;
            }
            s32 cursor = static_cast<s8>(pick + 1);
            if (cursor >= static_cast<s32>(kSpecialEnd)) cursor = static_cast<s32>(kFirstSpecial);
            m.write8(manager + kSpecialCursor, static_cast<u8>(cursor));
            return pick;  // no reset pan and no note off on this ring
        }
        // The normal ring: up to 32 probes from the cursor; a free channel (id 0) ends the scan, else the last
        // lower-priority channel, else the first equal-priority one with the strictly greatest age.
        s32 i = static_cast<s8>(m.peek8(manager + kNormalCursor));
        s32 lower = -1, equal = -1;
        u32 equal_age = 0u;
        for (s32 probes = static_cast<s32>(kNormalCount); pick < 0 && probes > 0; --probes) {
            const u32 c = channels + static_cast<u32>(i) * kChannelBytes;
            if (m.peek16(c + kId) == 0u) {
                pick = i;
            } else {
                const u32 p = m.peek8(c + kPriority);
                if (p < priority) {
                    lower = i;
                } else if (lower < 0 && p == priority) {
                    const u32 age = m.peek16(c + kAge);
                    if (equal < 0 || age > equal_age) {
                        equal = i;
                        equal_age = age;
                    }
                }
            }
            i = static_cast<s8>(i + 1);
            if (i >= static_cast<s32>(kNormalCount)) i = 0;
        }
        if (pick < 0) pick = lower >= 0 ? lower : equal;
        s32 cursor = static_cast<s8>(pick + 1);
        if (cursor >= static_cast<s32>(kNormalCount)) cursor = 0;
        m.write8(manager + kNormalCursor, static_cast<u8>(cursor));
        if (pick < 0) return pick;
        const u32 c = channels + static_cast<u32>(pick) * kChannelBytes;
        const s32 mode = static_cast<s32>(m.peek32(manager + kPauseMode));
        if (mode == 2) {
            m.write8(c + kVolume, 0x81u);  // -127
        } else if (mode == 1) {
            // A paused game hands a non-exempt channel to an exempt sound silenced.
            if (!pause_allowed(m, manager, m.peek16(c + kId)) && pause_allowed(m, manager, new_id))
                m.write8(c + kVolume, 0x81u);
        }
        s32 port;
        u32 ch;
        route(pick, port, ch);
        if (m.peek8(c + kPan) != 0u || m.peek8(c + kSurround) != 0u) {
            m.write8(c + kPan, 0u);
            m.write8(c + kSurround, 0u);
            sound(manager, port, [&](gcax_midi::SoundObject& o) { o.reset_pan(port, ch); });
        }
        sound(manager, port, [&](gcax_midi::SoundObject& o) { o.note_off(port, ch); });  // even a free channel
        return pick;
    }

    // Start 0x801D9B2C. +6 (the last volume) is deliberately not reset, as the original.
    void start(u32 c, u32 id, u32 row, u32 priority) noexcept {
        m.write32(c + kRow, row);
        m.write8(c + kBank, m.peek8(row + 8u));
        m.write8(c + kProgram, m.peek8(row + 9u));
        m.write8(c + kPriority, static_cast<u8>(priority));
        m.write8(c + kPan, 0u);
        m.write8(c + kSurround, 0u);
        m.write16(c + kBend, 0u);
        m.write8(c + kVolumeCeiling, 0u);
        m.write8(c + kVolumeOffset, 0u);
        m.write32(c + kFlags, 0u);
        const u32 duration = m.peek8(row + kRowDuration);
        const u16 frames = static_cast<u16>(duration != 0u ? duration : kDefaultDuration);
        m.write16(c + kReload, frames);
        m.write16(c + kFramesLeft, frames);
        m.write16(c + kAge, 0u);
        m.write16(c + kId, static_cast<u16>(id));
        m.write32(c + kOwner, 0u);
        for (u32 i = 0; i < 12u; i += 4u) m.write32(c + kPosition + i, kSingleZero);
        m.write32(c + kMaxDistance, double_to_single_bits_flush(se_math::max_distance(cpu, row)));
        m.write32(c + kPreviousDistance, kSingleZero);
    }

    // PlaySE 0x801D5F14 -> channel or 0.
    u32 play(u32 manager, u32 id, u32 boost, u32 position) noexcept {
        ++report().requests;
        if (!request_allowed(m, manager, id)) {
            ++report().gated;
            return 0u;
        }
        const u32 tables = m.peek32(manager + kTables);
        if (tables == 0u) return 0u;
        const u32 row = sound_tables::lookup(m, tables, id, services.tables);
        if (row == 0u) return 0u;
        if ((m.peek8(row + kRowFlags) & 0x10u) && position != 0u) {
            const double limit = bits_to_double(se_math::max_distance(cpu, row));
            const double distance = bits_to_double(se_math::listener_distance(cpu, position));
            if (distance > limit) {
                ++report().culled;
                return 0u;  // the row's start counter stays consumed
            }
        }
        const u32 base = m.peek8(row + kRowPriority);
        boost &= 0xFFu;
        const u32 priority = base == 10u ? 0u : ((base > boost ? base : boost) + 1u) & 0xFFu;
        const s32 index = allocate(manager, row, priority);
        if (index < 0) {
            ++report().no_channel;
            return 0u;
        }
        const u32 c = m.peek32(manager + kChannelArray) + static_cast<u32>(index) * kChannelBytes;
        start(c, id, row, priority);
        ++report().started;
        return c;
    }

    // FindOwned 0x801D5E04: the request gate, then the first channel with this owner and id.
    u32 find_owned(u32 manager, u32 id, u32 owner) noexcept {
        if (!request_allowed(m, manager, id)) return 0u;
        const u32 channels = m.peek32(manager + kChannelArray);
        for (u32 i = 0; i < kChannels; ++i) {
            const u32 c = channels + i * kChannelBytes;
            if (m.peek32(c + kOwner) == owner && m.peek16(c + kId) == (id & 0xFFFFu)) return c;
        }
        return 0u;
    }

    // RowFlags 0x801D5628: request flag 0x40 takes flags 2 / 1 from the row's flags bits 0 / 2.
    void row_flags(u32 c, u32& request) noexcept {
        if ((request & 0x40u) == 0u) return;
        const u32 row = m.peek32(c + kRow);
        if (row != 0u && (m.peek8(row + kRowFlags) & 1u)) request = (request | 2u) & 0xFFFFu;
        if (row != 0u && (m.peek8(row + kRowFlags) & 4u)) request = (request | 1u) & 0xFFFFu;
    }

    // KeepPos 0x801D990C.
    void keep_position(u32 c, u32 position) noexcept {
        if ((flags(c) & kPositional) && position != 0u) copy_vec3(m, c + kPosition, position);
    }

    // OutOfRange 0x801D99A8.
    bool out_of_range(u32 c) noexcept {
        const u32 row = m.peek32(c + kRow);
        if (row == 0u || (flags(c) & kPositional) == 0u) return false;
        if ((m.peek8(row + kRowFlags) & 0x10u) == 0u) return false;
        const double limit = bits_to_double(se_math::max_distance(cpu, row));
        return bits_to_double(se_math::listener_distance(cpu, c + kPosition)) > limit;
    }

    u16 duration_or_default(u32 c, s32 duration) noexcept {
        if (duration != 0) return static_cast<u16>(duration);
        const u32 d = m.peek8(m.peek32(c + kRow) + kRowDuration);
        return static_cast<u16>(d != 0u ? d : kDefaultDuration);
    }

    // Refresh 0x801D9944 (keep-alive).
    void refresh(u32 c, u32 request, s32 duration) noexcept {
        const u16 d = duration_or_default(c, duration);
        if (request & 4u) {
            m.write16(c + kReload, d);
            set_flags(c, flags(c) | kRetrigger);
        }
        if (request & 2u) {
            m.write16(c + kFramesLeft, d);
            set_flags(c, flags(c) | kRefreshed);
        }
    }

    // SetPos 0x801D9A28.
    void set_position(u32 c, u32 request, u32 position) noexcept {
        if ((request & 1u) == 0u || position == 0u) return;
        copy_vec3(m, c + kPosition, position);
        set_flags(c, flags(c) | kPositional);
        if ((m.peek8(m.peek32(c + kRow) + kRowFlags) & 8u) == 0u) set_flags(c, flags(c) | kPositionalPan);
        if (request & 0x20u) set_flags(c, flags(c) | kDoppler);
    }

    // SetDur 0x801D9AB4.
    void set_duration(u32 c, u32 request, s32 duration) noexcept {
        const u16 d = duration_or_default(c, duration);
        if (request & 4u) {
            m.write16(c + kFramesLeft, d);
            m.write16(c + kReload, d);
            set_flags(c, flags(c) | kRetrigger);
            return;
        }
        m.write16(c + kFramesLeft, d);
        if (request & 2u) set_flags(c, flags(c) | kRefreshed);
    }

    void arm(u32 c) noexcept { m.write32(c + kFlags, kStopAtExpiry | kStartPending); }  // Arm 0x801D9B20
};

Context context(Cpu& cpu, Services& services) noexcept { return Context{cpu, cpu.guest_memory(), services}; }

}  // namespace

// PlaySimple 0x801D59AC(mgr, id, s8 volume, owner) -> 1/0.
void serve_play_simple(Cpu& cpu, Services& services) noexcept {
    Context x = context(cpu, services);
    const u32 manager = cpu.gpr[3], id = cpu.gpr[4], volume = cpu.gpr[5], owner = cpu.gpr[6];
    u32 result = 0u;
    if ((id & 0xFFFFu) != 0xFFFFu) {
        const u32 c = x.play(manager, id, 0u, 0u);
        if (c != 0u) {
            x.arm(c);
            x.m.write8(c + kVolume, static_cast<u8>(volume));
            x.m.write32(c + kOwner, owner);
            result = 1u;
        }
    }
    cpu.gpr[3] = result;
}

// PlayFlags 0x801D5894(mgr, id, Vec3* position, owner, u16 flags) -> 1/0.
void serve_play_flags(Cpu& cpu, Services& services) noexcept {
    Context x = context(cpu, services);
    const u32 manager = cpu.gpr[3], id = cpu.gpr[4], position = cpu.gpr[5], owner = cpu.gpr[6];
    u32 request = cpu.gpr[7] & 0xFFFFu;
    u32 result = 0u;
    if ((id & 0xFFFFu) != 0xFFFFu) {
        const u32 kept = owner != 0u ? x.find_owned(manager, id & 0xFFFFu, owner) : 0u;
        if (kept != 0u) {
            ++x.report().kept_alive;
            x.row_flags(kept, request);
            x.keep_position(kept, position);
            if (!x.out_of_range(kept)) x.refresh(kept, request, 0);
            result = 1u;
        } else {
            const u32 c = x.play(manager, id, 0u, position);
            if (c != 0u) {
                x.row_flags(c, request);
                x.arm(c);
                x.set_position(c, request, position);
                x.set_duration(c, request, 0);
                x.m.write32(c + kOwner, owner);
                result = 1u;
            }
        }
    }
    cpu.gpr[3] = result;
}

// Play3D 0x801D5764(mgr, id, Vec3* position, owner, u16 flags, s8 volume, s16 duration) -> 1/0.
void serve_play_3d(Cpu& cpu, Services& services) noexcept {
    Context x = context(cpu, services);
    const u32 manager = cpu.gpr[3], id = cpu.gpr[4], position = cpu.gpr[5], owner = cpu.gpr[6];
    const u32 request = cpu.gpr[7] & 0xFFFFu, volume = cpu.gpr[8];
    const s32 duration = static_cast<s16>(cpu.gpr[9]);
    u32 result = 0u;
    if ((id & 0xFFFFu) != 0xFFFFu) {
        const u32 kept = owner != 0u ? x.find_owned(manager, id & 0xFFFFu, owner) : 0u;
        if (kept != 0u) {
            ++x.report().kept_alive;
            x.keep_position(kept, position);
            x.m.write8(kept + kVolume, static_cast<u8>(volume));
            if (!x.out_of_range(kept)) x.refresh(kept, request, duration);
            result = 1u;
        } else {
            const u32 c = x.play(manager, id, (request & 2u) != 0u ? 1u : 0u, position);
            if (c != 0u) {
                x.arm(c);
                x.set_position(c, request, position);
                x.set_duration(c, request, duration);
                x.m.write8(c + kVolume, static_cast<u8>(volume));
                x.m.write32(c + kOwner, owner);
                result = 1u;
            }
        }
    }
    cpu.gpr[3] = result;
}

// PlayRing 0x801D56A0(mgr) -> 1/0: the ring pickup sound, alternating left / right, at most every 1/30 s.
void serve_play_ring(Cpu& cpu, Services& services) noexcept {
    Context x = context(cpu, services);
    GuestMemory& m = x.m;
    const u32 manager = cpu.gpr[3];
    u32 result = 0u;
    if (static_cast<s8>(m.peek8(kRingGuard)) == 0) {
        m.write32(kRingToggle, 1u);
        m.write8(kRingGuard, 1u);
    }
    if (!(bits_to_double(single(m.peek32(manager + kRingCooldown))) > 0.0)) {
        const u32 c = x.play(manager, kRingId, 0u, 0u);
        if (c != 0u) {
            ++x.report().rings;
            m.write32(c + kFlags, kPanOnce | kStopAtExpiry | kStartPending);
            m.write8(c + kVolumeCeiling, 0u);
            m.write8(c + kVolumeOffset, 0u);
            const u32 toggle = m.peek32(kRingToggle);
            m.write8(c + kPan, m.peek8(kRingPan + toggle));
            m.write32(kRingToggle, toggle == 0u ? 1u : 0u);
            m.write32(manager + kRingCooldown, kRingCooldownBits);
            result = 1u;
        }
    }
    cpu.gpr[3] = result;
}

// GateClearIfZero 0x801D5190 / GateSetIfZero 0x801D51C0 (mgr, s32 x) and GateClear 0x801D51F0 / GateSet 0x801D5218
// (mgr): the request gate, mgr + 0x14 bit 1.
void serve_gate(Cpu& cpu, u32 entry) noexcept {
    GuestMemory& m = cpu.guest_memory();
    const u32 gates = cpu.gpr[3] + kGates;
    const bool zero = cpu.gpr[4] == 0u;
    constexpr u32 bit = 1u << kRequestGate;
    if (entry == kGateSet || (entry == kGateSetIfZero && zero)) m.write32(gates, m.peek32(gates) | bit);
    else if (entry == kGateClear || (entry == kGateClearIfZero && zero)) m.write32(gates, m.peek32(gates) & ~bit);
}

// FrameSE 0x801D6270(mgr): the tables' per-frame start counters, then every active channel.
void serve_frame(Cpu& cpu, Services& services) noexcept {
    Context x = context(cpu, services);
    GuestMemory& m = x.m;
    const u32 manager = cpu.gpr[3];
    ++x.report().frames;
    const u32 tables = m.peek32(manager + kTables);
    if (tables != 0u) {
        // ResetFrameCounts 0x801D87EC: +0x10 of every row of the eight slots.
        for (u32 slot = 0; slot < 8u; ++slot) {
            const u32 s = tables + slot * 0x10u;
            for (s32 i = 0; i < static_cast<s32>(m.peek32(s)); ++i)
                m.write32(m.peek32(s + 4u) + static_cast<u32>(i) * sound_tables::kRowBytes + sound_tables::kRowLive, 0u);
        }
    }
    if (static_cast<s32>(m.peek32(manager + kPauseMode)) == 2) return;
    const u32 sound_manager = m.peek32(kManagerPointer);
    u32 c = m.peek32(manager + kChannelArray);
    for (u32 i = 0; i < kChannels; ++i, c += kChannelBytes) {
        if (m.peek32(c + kRow) == 0u) continue;
        if (static_cast<s32>(m.peek32(manager + kPauseMode)) == 1 && !pause_allowed(m, manager, m.peek16(c + kId)))
            continue;
        if ((m.peek32(manager + kGates) >> kRequestGate) & 1u) {
            if (!request_allowed(m, manager, m.peek16(c + kId))) {
                const s32 left = static_cast<s16>(m.peek16(c + kFramesLeft));
                if (left > 10 || left < 0) m.write16(c + kFramesLeft, 10u);
            }
        }
        x.update_channel(sound_manager, c);
    }
    // vt+0x14 of the sound object is 0x801D6430 `blr`: nothing to do.
}

}  // namespace gekko::se_manager
