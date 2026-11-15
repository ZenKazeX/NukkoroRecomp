# The native engine for data-writing codes

Branch `mods/code-engine` (from `mods/loader`). Code: `runtime/include/gekko/code_engine.h`, `code_engine_guest.h`,
`runtime/src/gekko/code_engine.cpp`; hook `NativeVideo::set_frame_hook` (`native_video.h/.cpp`); command line in
`runtime/src/shadow_main.cpp`; tests `runtime/tests/code_engine_tests.cpp` (CTest `code_engine_tests`); in-game proof
`tools/mods/codemods/engine_proof.py`.

Public wording: a static recompilation made with GekkoForge. Codes that only write data need no recompile; this engine
applies them to guest memory natively. Codes that change or run PowerPC cannot run here and are refused by name.

## 1. Where it runs

On the console a Gecko code handler runs once per frame from a hook (VI retrace, `VIWaitForRetrace`, GXDraw...).
Our equivalent is the **retrace delivery**, `NativeVideo::retrace_handler` (the native `__VIRetraceHandler`,
`runtime/src/gekko/native_video.cpp`). It runs on the game thread once per VI field, which on this port is the game's
frame period (`NativeVideo::frame_locked`). The hook sits in `kPhaseWakeup`: after the SDK's pre/post retrace callbacks and
the register commit, **just before `OSWakeupThread` wakes the sleepers of `VIWaitForRetrace`**. So the game's main thread
resumes into its next frame already seeing what the codes wrote, exactly the order of a handler hooked at the VI. That
case executes once per delivery (it stores `kPhaseTick` and returns; a re-entry after a guest call resumes at
`kPhaseTick`), so there is no double run. If the game ever needs more than one retrace per frame, the codes run once per
retrace, as on the console.

Writes go through `GuestMemory::read/write8/16/32` (the accessors of the same pages the translated code's inline accessors
address, FM dual mapping, big-endian layout applied by the accessor): a store is seen by translated code like a store the
game made. `GuestCodeMemory` (`code_engine_guest.h`) adapts it; `CodeMemory` is the engine's only memory dependency, so the
engine is unit-tested without the game.

## 2. Zero cost when off

`--code-ini` / `--code-lines` are the only way to configure the engine (the loader will call `Engine::configure`). Nothing is
installed unless `configure` leaves at least one runnable, enabled code (`Engine::active()`); `shadow_main` then calls
`NativeVideo::set_frame_hook`. Otherwise `frame_hook_` is an empty `std::function`: one untaken branch per retrace and no
other state. Unit tests assert that nothing configured, a disabled code and a refused code all leave the engine inactive.

Proof, same executable family:

- `base.exe` = `mods/loader` built in this worktree (md5 7438ce75884d), `cand.exe` = the same tree plus the engine
  (md5 834f2eb0cf6e). Westopolis tape to game tick 810,000,000 (frame 1000), 1280x720, MSAA off:
  base picture `0813d7fd6208`, candidate without codes `0813d7fd6208`: identical.
- Regression gate (`wt-mods/tools/mods/modtest.py gate`, both executables built in this worktree, never deployed; base run
  twice as the determinism control): Westopolis 316 s base/repeat/cand PCM `19fe683a6e87bbff`, state `8d30c24616360278`
  all equal: PASS; Final Haunt 120 s PCM `a886ae3c0aadfca0`, state `e075d9a8d1a5482c` all equal: PASS; behaviour trace
  (`behaviour_diff.py --exact`) VERDICT PASS.

## 3. Code types

A code = lines of two big-endian words `a b`. `ba` = base address (default 0x80000000), `po` = pointer offset (default
0x80000000). Byte 0 of `a`: bit 4 (`0x10`) = use `po` instead of `ba` (so `14` is `04` on `po`, `30` is `20` on `po`...), bit 0
of the byte (address bit 24) = the address is in 0x81xxxxxx. Address = (`ba`|`po`) + (`a` & 0x01FFFFFF). Layouts follow the
public Gecko code type documentation (WiiRd / Gecko handler) as listed in the table; they are verified here by unit
vectors and by the in-game run below, **not against a console**.

| Type | Meaning | State |
|---|---|---|
| `00` / `02` / `04` | 8-bit fill (`YYYYYYZZ`: ZZ, Y+1 times), 16-bit fill (`YYYYZZZZ`), 32-bit write | runs |
| `06` | string, `b` bytes, ceil(b/8) data lines | runs |
| `08` | serial: `08______ X` + `TNNNZZZZ V`: N+1 writes, address +Z bytes, value +V; T = 0/1/2 (8/16/32 bit) | runs |
| `20 22 24 26` | 32-bit if equal / not equal / greater / less (unsigned) | runs |
| `28 2A 2C 2E` | 16-bit; `MMMMXXXX`: (value & ~MMMM) compared with XXXX | runs |
| low bit of `a` on a conditional | apply one endif first | runs |
| `E0` / `E2` | terminator (clears every open if) / `E2T000VV`: VV endifs, T=1 then else (invert the innermost). `XXXXYYYY`: ba = XXXX0000, po = YYYY0000 when non-zero | runs |
| `F0` | end of codes | runs |
| `40 42 44` / `48 4A 4C` (+ `0x10` = source po) | load / set / store ba, and po: `40TYZ00N X`: ba = [(Y: ba or po) + (Z: grN) + X], T=1 adds instead of sets; `42` the same without the load; `44` stores ba at that address | runs |
| `80 82 84 86 88 8A 8C` (+`0x10`) | Gecko registers gr0-gr15: set (`80SY000N`), load 8/16/32 (`82UY000N`), store with repeat (`84T?YYYN`), arithmetic with a value or a register (add, mul, or, and, xor, slw, srw, rol, asr, fadds, fmuls; forms register/memory), memory copy 1 and 2 | runs |
| `46` / `4E` | ba / po = address of the next code line | refused: the code list is not in guest memory (Unsupported) |
| `60`-`68` repeat, goto, gosub; `A0`-`B6` register ifs; `A8`-`AE` counter ifs; `CC`/`CE`/`DE` | | refused (Unsupported): layout not in hand, never guessed |
| `C0` `C2` `C6` `D2` `D6` `F2` `F4` `F6` | execute / insert PowerPC, write a branch | **NeedsRecompile**: "needs a recompiled variant" |
| any write whose static target is inside the DOL's code (`Layout`) | patches an instruction | **NeedsRecompile** |

Semantics worth knowing: ba and po are reset to 0x80000000 at the start of every code run (every retrace, every code);
the 16 Gecko registers persist across frames (frame counters work) and are shared by all codes; an if inside a false if is
not evaluated and stays false (also across its else); a read of unmapped memory makes a condition false and a register 0; a
write whose target is not committed guest memory is skipped and counted (`unmapped_writes`, first bad address kept), never
fatal; a write into the code ranges at run time (a computed pointer) is blocked (`code_writes_blocked`).

Classification (`analyze(Code, Layout)`) is static and returns `Runs`, `NeedsRecompile`, `Unsupported` or `Malformed` with
the offending line; `Engine::configure` installs only `Runs` and enabled codes, and returns one verdict per input code so the
loader can show the user why a code does not run. `pal_layout()` carries the PAL DOL's two text sections.

## 4. API

```cpp
namespace gekko::codes {
struct Line { u32 a, b; };
struct Code { std::string name; std::vector<Line> lines; bool enabled; };
Layout pal_layout();                                   // PAL DOL text ranges
Verdict analyze(const Code&, const Layout&);
IniResult parse_ini(const std::string& text);          // Dolphin [Gecko] / [Gecko_Enabled] -> codes, enabled flags
bool parse_lines(const std::string& text, std::vector<Line>&, std::string& error);
class Engine { Engine(CodeMemory&);
  std::vector<Verdict> configure(const std::vector<Code>&, const Layout&);
  bool active() const; void run_frame(); const Stats& stats() const; std::string report_json() const; };
}
```

Loader hand-off: `shadow::mods` collects the enabled mods' codes (as `Code{name, lines, enabled=true}`), `main()` calls
`code_engine.configure(codes, pal_layout())`, prints each verdict, and installs the hook only if `active()`. `shadow_main`
already does exactly this for the test command line:

```
--code-ini <game.ini>      [Gecko] codes; [Gecko_Enabled] ones run; also --code-enable <name> (exact or before " [") and --code-enable-all
--code-lines <file>        one code from plain "XXXXXXXX XXXXXXXX" lines (name = file stem)
--code-report <file.json>  verdicts + engine stats (frames, writes, writes_changed, conditions_true/false, unmapped, blocked)
```

All code addresses must be PAL. The NTSC -> PAL map is in `extract/address-maps/GUPE8P.map` (PAL -> NTSC, so PAL = NTSC - delta
of the segment holding it); `test_data/mods/reloaded_features.json` already has it for the DOL image. A data/bss segment in
the map gives the delta for the globals the DOL image does not hold: for 0x8056FE00-0x80584959 it is -0x10C0 (NTSC =
PAL - 0x10C0), so NTSC 0x8057D7E8 is PAL 0x8057E8A8.

## 5. Proof in game with a real data code

The public preference code "No HUD" is a single 8-bit write to an NTSC bss byte (map, `RELOADED_CODE.md`: "none", bss).
Translated with the map's data delta to PAL **0x8057E8A8** (the byte `docs/STAGE_LOADING.md` calls a "play flag", G+0x148) and
run as `0057E8A8 00000000` through `--code-lines`, on the Westopolis tape (`stg0100`), headless, at game tick 810,000,000:

| run | picture | engine |
|---|---|---|
| stock `base.exe`, no code | `0813d7fd6208` (HUD: timer, score, ring counter, gauges) | none |
| `cand.exe`, no code | `0813d7fd6208` | not installed |
| code on | `3c58fc2e58ef` (HUD gone, 3D scene identical) | 1196 frames, 1196 writes, 2 changed the byte |
| `20` (equal DEADBEEF) false condition + write + `E2` | `0813d7fd6208` (= off) | 1196 conditions false, 0 writes |
| `22` (not equal DEADBEEF) true condition + write + `E2` | `3c58fc2e58ef` (= on) | 1196 conditions true, 1196 writes |

`tools/mods/codemods/engine_proof.py` reproduces the five runs and checks them (all five checks PASS; output in the
section above is its output). This also confirms the PAL translation of the bss address by its effect, and that the
retrace-time write is seen by translated code every frame (`writes_changed` is 2 of 1196: the byte already held 0 for all but
two writes, so the engine holds it there against the game setting it again).

Not captured: a data code whose effect needs a controller condition (Moonjump / Fill Meter use the pad word, `2856ED4E`
NTSC = PAL 0x8056FE0E); the conditional machinery is proven above on a memory word and by unit tests on the 16-bit mask form
those codes use.

## 6. Reloaded's 33 Gecko codes through this engine

By the earlier map (PAL addresses in `reloaded_features.json`; bss addresses translated with the segment delta above):

- **Runs as data-only: 8** - All Partners Can Homing Attack, Restore Original Escape Pod Health, Restore Original Bloom,
  Moonjump, 1-hit kill Chaos Blast, Instant Lock-On, Unlimited Wallrun, Unlimited Wallcling (writes into DOL data).
- **Runs once the bss address is translated: 10** - Mute music and cutscenes, Race Mode ON/OFF, No Subtitles, No HUD,
  Expert CC Stop, Fill Hero Meter, Fill Dark Meter, Clear Both Meters, Playable cutscenes WIP 1 (PAL addresses by the
  -0x10C0 / -0x10A8 segment deltas, e.g. 0x80590A5C, 0x80578BED, 0x805E2C5C, 0x80577788, 0x80577794).
- **NeedsRecompile: 15** - instruction patches or C2/branch codes (No Idle ChaosPowers Music, Skip Loading Events,
  Allow Resetting Timer, Restore Partner Intro, Invuln/Recovery, Restore Multiplayer / Skid / Car / Air Saucer / Knockback,
  No Ammo for Melee, SuperShadow, Jump Ball Color Switch, Shadow Dab).

Data-write codes into DOL **data** work only if the game still reads that datum from memory: a constant a native owner or a
lifted function replaced (native math, renderer, file layer...) is ignored. Check each code's effect (the way section 5 does);
the engine reports writes, not their consequences.

## 7. Limits and risks

- Retrace granularity, once per VI field. Codes cannot act between two frames, and a Gecko code that relies on being run
  from a different hook (GXDraw, per-pad-read) sees a different ordering. Frame-limiter/headless runs retrace at the game
  clock, so counts match frames.
- Layouts of 40-4C and 80-8C are from the public documentation, not from a console. Unit tests prove the implementation
  matches the documented layout; correct behaviour on real-world pointer codes should be spot-checked in game.
- `46`/`4E`, `60`-`68`, register/counter ifs and `CC/CE/DE` are refused (Unsupported), never approximated.
- The engine is not part of the save state: Gecko registers restart after a savestate load.
- Writes happen on the game thread inside the interrupt-style delivery: safe because the game thread owns all guest memory
  writes; a code must not target memory the native runtime mutates from other threads (none of the guest RAM is).
- Whether a data code is wanted in speedrun/mod contexts is the loader's policy; the engine only executes.
