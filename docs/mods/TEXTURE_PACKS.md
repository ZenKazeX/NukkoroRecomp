# Texture packs (Dolphin-named custom textures)

Players' existing Dolphin texture packs work unchanged: point the game at the pack folder and every file whose
name matches a game texture is drawn in its place.

    ShadowTheHedgehog.exe --texture-pack "<pack folder>" [--texture-pack "<another pack>" ...]

The flag is repeatable; when two packs provide the same texture, the one given first wins. Folders are searched
recursively and names are matched case-insensitively. Without any `--texture-pack` the feature is off and costs one
relaxed atomic load per texture import (no hashing, no lookups, no allocation).

Code: `runtime/include/gekko/texture_packs.h`, `runtime/src/gekko/texture_packs.cpp`, the lookup in
`runtime/scaffold/rw_driver/textures.cpp` (`RwDriver::build_texture`), tests in
`runtime/tests/texture_packs_tests.cpp`, the disc census tool `tools/mods/dolphin_texture_names.py`.

## API (for the mod loader)

```cpp
#include "gekko/texture_packs.h"
namespace gekko::texture_packs {
void configure_texture_packs(std::vector<std::filesystem::path> dirs_highest_priority_first);  // {} = off
void add_texture_pack(const std::filesystem::path& dir);     // append at the lowest priority (the CLI flag)
std::vector<std::filesystem::path> texture_pack_directories();
bool texture_packs_enabled() noexcept;                       // inline, one relaxed load
Stats texture_pack_stats();                                  // indexed / shadowed / ignored files, lookups,
void print_texture_pack_stats(std::ostream&);                // hits, misses, load failures, decode time, residency
}
```

`configure_texture_packs` rebuilds the index at once (directory scan only; nothing is decoded). `add_texture_pack`
(what the CLI flag calls) only records the directory; all added directories are scanned once, at the first texture
import. Logging: one line when the index is built, one `[texture-packs] loaded <file> (WxH)` per replacement decoded,
`[texture-packs] cannot use <file>: <reason>` per unusable file, and a summary at process exit; the lifted
renderer's exit line also counts `N from texture packs`.

## The naming rule, and its proof

    tex1_{W}x{H}[_m]_{texhash:016x}[_{tluthash:016x}]_{fmt}.{png|dds}

| part | meaning |
|---|---|
| `W`, `H` | the raster's width and height (not block-padded) |
| `_m` | the sampler drawing the texture has mip filtering on (a draw-time property, not part of the bytes) |
| `texhash` | XXH64, seed 0, over the **base level only**: the GX block-padded size of `W x H` in `fmt` (C4/I4/CMPR 8x8 blocks at 4 bpp, C8/I8/IA4 8x4 at 8 bpp, IA8/RGB565/RGB5A3/C14X2 4x4 at 16 bpp, RGBA8 4x4 at 32 bpp). Mip levels stored after it are not hashed. |
| `tluthash` | paletted formats only (C4 = 8, C8 = 9, C14X2 = 10): XXH64, seed 0, over the palette entries from the smallest to the largest index the base level uses, 2 bytes per entry, big-endian as stored. C4 scans both nibbles of every byte, C8 every byte, C14X2 every big-endian u16 & 0x3FFF. `_$` in its place matches any palette. |
| `fmt` | the GX texture format number in decimal (I4 0, I8 1, IA4 2, IA8 3, RGB565 4, RGB5A3 5, RGBA8 6, C4 8, C8 9, C14X2 10, CMPR 14) |

Proof: `tools/mods/dolphin_texture_names.py` walks the PAL disc's `files/` (every `.one` archive, PRS-expanded, and
every loose `.txd`), reads every platform-6 TEXTURENATIVE raster, builds the name above from the stored bytes with
the reference `xxhash` package, and compares against the pack files. On the PAL data it found 10,275 raster
records / 4,323 distinct names. Exact matches (every field equal, including both hashes) against the real packs:

| pack | tex1_ files | match the PAL disc | match the NTSC-U disc | notes |
|---|---|---|---|---|
| Shadow Android | 6 | 5 | 5 | the 128x64 C4 file matches no disc TXD of either region |
| ShdTH-Reloaded `GUPR8P/Shadow` | 6 | 5 | 5 | same names as Shadow Android (vanilla textures); the 128x128 RGBA8 file is Reloaded's own data |
| ShdTH-Reloaded `GUPR8P/high res` | 12 | 1 | 1 | `newMenu/` and one HUD file replace Reloaded's own (modified) menu textures |
| ShdTH-Reloaded `GUPR8P/normal res fix` | 3 | 1 | 1 | two files name Reloaded-only textures |
| Maria Fixed Eyes (`Dolphin/`) | 4 | 4 | 4 | |
| Regular Rouge (DDS, BC1) | 1 | 1 | 1 | |
| Subtitle Remover | 52 | 51 | 52 | The Last Way's subtitle sheet differs between PAL and NTSC-U |
| HQ GUN Soldiers (DDS, BC3, 2048x2048) | 6 | 1 | 1 | the other 5 name textures inside the mod's own replacement `.one` files; against those files all 6 match exactly |

What the census does and does not pin down:
* palette range: proven. Four matched paletted rasters use only part of their palette (rouge.one `pl_kro1n`,
  event0916 `ev_knma3`, GunSoldierData_ark00 `en_hngsh`, number.txd `ad_hpasc1`); their pack names carry the
  hash of the used range, which differs from the hash of the whole palette. The tool also tries the whole-palette
  hash for every unmatched file; it never matches.
* base level only: not testable on this game and not needed for it. Every one of the 10,275 rasters on the PAL disc
  has exactly one level (`mips` = 1, stored size = base size), so "base level" and "all stored levels" are the same
  bytes. The implementation hashes the base level, which is Dolphin's documented behaviour.
* `_m`: none of the tested packs uses it, consistent with no raster having mips. The hash is identical either way, so
  the lookup accepts both spellings (the exact one first: RenderWare filter modes 3-6 on the drawing RwTexture
  mean `_m`).

The C++ rule (`compute_key`) is unit-tested against XXH64 reference values and hand-built C4/C8 rasters.

## In-game results (2026-11-14, lifted renderer, PAL data)

Runs: `--headless --no-audio --tv-format ntsc --boot-stage stg0100 --budget-ms 26000`, captures at render frames
900 and 1500, `--state-report` at the end. The game's input is the same in every run (no tape, no input).

| packs | lookups | hits | files decoded |
|---|---|---|---|
| Shadow Android | 342 | 3 | `pl_bw04n`, `pl_bw03n` (128x128 PNG) and the 128x64 C4 file |
| all six tested packs (Shadow Android first, then Reloaded `GUPR8P`, Maria, Rouge, Subtitle Remover, HQ GUN Soldiers) | 342 | 11 | 8 files, incl. the stg0100 subtitle sheet, Reloaded's HUD files and HQ GUN Soldiers' `en_hngsh` (BC3 DDS, 1024x1024) |
| a probe pack: Rouge's BC1 DDS (384x384) and a BC3 DDS (2048x2048) renamed to Shadow's two body textures | 342 | 2 | both DDS files (18.5 ms to decode both) |
| none | - | - | no `[texture-packs]` line at all: the import never called into the packs |

Two files the disc census could not place were hit in game: Shadow Android's `tex1_128x64_924376c246b3c129_..._8`
and Reloaded's `tex1_128x128_8f45dd68096d4665_1db8734681d4c1dc_8`. Their rasters exist in game memory but not in any
disc TXD the census walks (the second shares its palette hash with `pause0100_EN.txd`'s `hud_meter`); their source
container is not identified yet. The census numbers above are therefore a floor.

Gameplay and the no-pack path:
* The end-of-run state report (game ticks, frame counts and a digest per 64 KiB block of the guest's memory regions:
  program image, OS heap, sound banks, stream ring, motion pool) is byte-identical across all of these runs, and
  identical to the same run of the existing `agent/opt-integrate` build (built after f43c6aec, without this change).
* Without a pack, this build's captures are byte-identical to that earlier build's captures (frames 900 and 1500).

## Supported files

* `.png` (any bit depth stb_image reads; converted to RGBA8).
* `.dds`: uncompressed 32/24-bit RGB(A) with channel masks (legacy header), DX10 R8G8B8A8 / B8G8R8A8 / B8G8R8X8,
  BC1 (DXT1, with 1-bit alpha), BC2 (DXT2/3), BC3 (DXT4/5). Only the top level is read; the renderer generates the
  mip chain for every game texture, replacements included.
* Any extent up to 8192x8192. Sampling is normalised, so the game's UVs map a larger image onto the same surface.
  An extent that is not a multiple of 4 is padded by repeating the edge texels (the renderer's RGBA8 import takes
  whole 4x4 blocks).

Each file is decoded on its first hit (on the game thread, inside the texture import) and cached (1 GiB budget of
decoded texels; past it the cache is dropped and refilled on demand). A file that cannot be decoded is reported once
(`[texture-packs] cannot use ...`) and skipped from then on.

## Not supported, and why

* **BC7 / BC4 / BC5 / BC6H DDS**: refused with a message (none of the tested packs uses them). Re-save as PNG or
  BC1-BC3.
* **Custom mip levels** (`..._mip1.png` etc.): ignored; the renderer builds mips from the top level. `_arb`
  (Dolphin's "arbitrary mipmaps" marker) is accepted and treated as a plain image.
* **EFB copies** (`efb1_...`): this renderer has no EFB; render-to-texture effects are native passes.
* **Dolphin GraphicMods** (`GraphicMods/` folders, `.txt`/`.json` metadata): not read.
* **Textures that are not in the game's data**: names produced from NTSC-U-only or Reloaded-modified data, or from a
  replacement `.one` that is not installed, match nothing on the PAL data (see the table). Installing the mod's
  replacement files through the mod loader makes those names appear.
* **Movie frames** are never looked up (they are rebuilt every frame).
* No prefetch option: a replacement is loaded when the game first loads the texture (Dolphin's "Prefetch Custom
  Textures" has no equivalent yet). Large DDS files can lengthen a stage load slightly.

## Gameplay

The lookup reads the raster the game already loaded and changes only the texels handed to the renderer. Guest
memory, raster sizes and palettes are untouched, so the game's logic sees exactly what it sees without a pack.
