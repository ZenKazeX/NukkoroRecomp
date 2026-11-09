# RenderWare 3.7 PC (Direct3D 9) native texture format

Specification of the PC texture native chunk as the ORIGINAL RenderWare 3.7.0.2 D3D9 driver writes and reads it,
for the native 64-bit Direct3D 12 reimplementation (docs/RENDERWARE_RECONSTRUCTION.md, step 2).

Rules of this document: every statement cites the instruction it comes from as `function+offset` (offsets are the
`dumpbin /disasm` offsets inside that function). Statements that come from the oracle corpus and not from the code are
marked **(corpus)**. Anything not established is marked **UNKNOWN** with the reason. Statements about the Direct3D 9
runtime itself (not RenderWare code) are marked **(D3D9 runtime, external)**.

## Sources

| what | where |
|---|---|
| native texture read / write / size | `.build/rw_pc_oracle/objs/d3d9texdic.asm` (rwcore.lib `d3d9texdic.obj`): `__rwD3D9NativeTextureGetSize`, `__rwD3D9NativeTextureWrite`, `__rwD3D9NativeTextureRead` |
| raster create / lock / palette / format table | `objs/d3d9raster.asm` (+ `objs/d3d9raster.sym.txt`, `.rdata` dumped with `dumpbin /section:.rdata /rawdata:4`) |
| raster size clamp | `objs/d3d9convrt.asm`: `__rwD3D9CheckRasterSize` |
| dictionary / chunk framing | `objs/babintex.asm`: `_RwTexDictionaryStreamWrite`, `_writeNativeTexture`, `_RwTexDictionaryStreamRead` |
| core raster / texture | `objs/baraster.asm` (`_RwRasterCreate`, `_RwRasterGetNumLevels`), `objs/batextur.asm` (`_RwTextureCreate`, `_RwTextureSetName`, `_RwTextureSetMaskName`) |
| standard-function registration, adapter info | `objs/d3d9device.asm` |
| names, enums, structs | `RW37SDK/Library D3D9 Hdr Files/rwcore.h`, `rwplcore.h`, `errcom.def`, `errcore.def` |
| D3D9 constants, `D3DCAPS9` layout, COM vtable order | Windows SDK 10.0.19041.0 `shared/d3d9.h`, `d3d9types.h`, `d3d9caps.h`, `um/wingdi.h` |
| ground truth | `.build/rw_pc_oracle/corpus/*.txd` / `*.ref` (RTX 4080, seed 0x12345678, `tools/rw_pc_oracle/texcorpus.c`) |
| byte checker | `.build/rw_pc_oracle/txd_check.py` (`python -I txd_check.py`) |

All extra objects were extracted from `RW37SDK/Library D3D9 lib Files/release/rwcore.lib` into `.build/rw_pc_oracle/objs/`.

Identifications used throughout (all from headers / symbol sizes, not guessed):

- `RwRaster` (rwcore.h:1365): +0x00 parent, +0x04 cpPixels, +0x08 palette, +0x0C width, +0x10 height, +0x14 depth,
  +0x18 stride, +0x1C/+0x1E offsets, +0x20 cType, +0x21 cFlags, +0x22 privateFlags, +0x23 cFormat (= format >> 8),
  +0x28 originalWidth, +0x2C originalHeight.
- `RwTexture` (rwcore.h:2400): +0x00 raster, +0x10 name[32], +0x30 mask[32], +0x50 filterAddressing, +0x54 refCount.
- `__RwD3D9DeviceCaps` is a COMMON symbol of size 0x130 (`d3d9device.sym.txt`), = sizeof(D3DCAPS9). Offsets from
  d3d9caps.h: +0x0C Caps2, +0x3C TextureCaps, +0x44 CubeTextureFilterCaps, +0x58 MaxTextureWidth, +0x5C MaxTextureHeight.
- COM vtable offsets (counted from d3d9.h): IDirect3D9 +0x28 CheckDeviceFormat, +0x30 CheckDepthStencilMatch;
  IDirect3DDevice9 +0x5C CreateTexture, +0x64 CreateCubeTexture, +0x11C SetPaletteEntries; IDirect3DTexture9 /
  IDirect3DCubeTexture9 +0x34 GetLevelCount, +0x44 GetLevelDesc, +0x48 GetSurfaceLevel / GetCubeMapSurface,
  +0x4C LockRect, +0x50 UnlockRect; IDirect3DSurface9 +0x34 LockRect, +0x38 UnlockRect.
- Standard functions (rwplcore.h:5709-5712, registered in d3d9device.asm lines 2405-2413): 25 GetSize
  (`RwEngineInstance+0xAC`), 26 Read (`+0xB0`), 27 Write (`+0xB4`), 28 GetMipLevels (`+0xB8`).

### The D3D9 raster extension (`RwRaster + _RwD3D9RasterExtOffset`)

Offsets as the code uses them:

| ext offset | content | evidence |
|---|---|---|
| +0x00 | `IDirect3DTexture9*` or `IDirect3DCubeTexture9*` | passed as ppTexture to CreateTexture / CreateCubeTexture (`_rwD3D9CreateTextureRaster+0x10E/+0x1A2`, Read+0x171/+0x202) |
| +0x04 | palette block: 256 x 4-byte entries + u32 D3D palette number at +0x400 (free list of 0x404-byte blocks) | `_rwD3D9CreateTextureRaster+0x52..+0xC7`, `_rwD3D9RasterUnlockPalette+0x50..+0x65` |
| +0x08 | u8 alpha | `__rwD3D9RasterHasAlpha+0xC`, `_rwD3D9CreateTextureRaster+0xEF` |
| +0x09 | low nibble = cube flag, high nibble = cube face selected for lock | `__rwD3D9RasterIsCubeRaster+0xA`, `__rwD3D9CubeRasterSelectFace+0x12..+0x1E`, `__rwD3D9RasterLock+0xB6` |
| +0x0A | low nibble = Direct3D auto mip generation (D3DUSAGE_AUTOGENMIPMAP), high nibble = compressed (DXT) | `__rwD3D9RasterCreate+0x23E`, `RwD3D9RasterCreate+0x282` |
| +0x0B | u8 locked mip level (0xFF = none) | `__rwD3D9RasterCreate+0x3D`, `__rwD3D9RasterLock+0x308` |
| +0x0C | locked `IDirect3DSurface9*` | `__rwD3D9RasterLock+0x98`, `__rwD3D9RasterUnlock+0x5E` |
| +0x10 | `D3DLOCKED_RECT` (+0x10 Pitch, +0x14 pBits) | `__rwD3D9RasterLock+0xE6`, `+0x2BE`, `+0x2FA` |
| +0x18 | `D3DFORMAT` | `_rwD3D9CreateTextureRaster+0x32`, `RwD3D9RasterCreate+0x270`, Read+0x1B0 |
| +0x1C | a COM pointer, zeroed by `_rwD3D9RasterCtor+0xA`, released by `_rwD3D9RasterDtor+0x1A`; purpose **UNKNOWN** (only the camera lock path reads it, `__rwD3D9RasterLock+0x218`) |

## 1. The chunk layout

### 1.1 Framing (core, not the driver)

Written by `_RwTexDictionaryStreamWrite` / `_writeNativeTexture` (babintex.obj):

```
rwID_TEXDICTIONARY (0x16) chunk                       _RwTexDictionaryStreamWrite+0x6C
  rwID_STRUCT (0x01), length 4                         +0x8B
    u16 textureCount                                   +0xE3 (from _countTexturesInDictionary; >= 0x10000 -> write fails, +0xC7)
    u16 deviceId = rwDEVICESYSTEMGETID (0x16) result, high byte forced 0   +0xEF, +0xF4
  per texture: rwID_TEXTURENATIVE (0x15) chunk         _writeNativeTexture+0x4F
    [driver payload, section 1.2]                      _writeNativeTexture+0x78 (RwEngineInstance+0xB4 = NativeTextureWrite)
    texture plugin data (rwID_EXTENSION)               _writeNativeTexture+0x95 (_rwPluginRegistryWriteDataChunks)
  dictionary plugin data (rwID_EXTENSION)              _RwTexDictionaryStreamWrite+0x137
```

Every chunk header is written with version 0x37002 and build 0xA (`push 37002h` / `push 0Ah` before every
`__rwStreamWriteVersionedChunkHeader` call). The library id word on disk is 0x1C02000A **(corpus, all 10 files)**; the
encoding is done in `_rwStreamWriteVersionedChunkHeader` (bastream.obj), which was not disassembled, so the formula
(`((v-0x30000)&0x3FF00)<<14 | (v&0x3F)<<16 | build`) is only checked against the bytes, not read from the code.

The TEXTURENATIVE chunk length = `GetSize + extension size` (`_writeNativeTexture+0x4A`: `ecx + eax + 0Ch`, where ecx
is the driver size and eax+0xC the plugin chunk size).

### 1.2 Driver payload: one rwID_STRUCT chunk

`__rwD3D9NativeTextureWrite+0x37` writes the struct header: type 1, length = GetSize - 12, version 0x37002, build 0xA.
All fields little-endian, no padding between them:

| offset in payload | size | field | written by | read by |
|---|---|---|---|---|
| 0x00 | 4 | platform id = **9** (`rwID_PCD3D9`, rwplcore.h:2063) | Write+0x83 | Read+0x5E: must be 9 |
| 0x04 | 4 | filter / addressing word (1.3) | Write+0x4E..+0x8B | Read+0x5B5..+0x5EC |
| 0x08 | 32 | texture name, copied whole from `RwTexture.name` | Write+0x8F (`rep movs` 8 dwords from tex+0x10) | Read+0x5EF (`RwTextureSetName`) |
| 0x28 | 32 | mask name, copied whole from `RwTexture.mask` | Write+0xA4 (8 dwords from tex+0x30) | Read+0x5FA (`RwTextureSetMaskName`) |
| 0x48 | 4 | raster format = `RwRaster.cFormat << 8` (rwRASTERFORMAT* bits 8-15 only, upper and lower bytes 0) | Write+0xDF..+0xE9 | Read+0xA8, +0x258, +0x351 |
| 0x4C | 4 | D3DFORMAT = ext+0x18 | Write+0x140, +0x14B | Read+0x8F, +0x40B |
| 0x50 | 2 | width = raster width | Write+0xCF | Read+0xCB, +0x41A |
| 0x52 | 2 | height = raster height | Write+0xDB | Read+0xB0, +0x42F |
| 0x54 | 1 | depth = raster depth | Write+0xED | Read+0xAC (only passed to RwRasterCreate) |
| 0x55 | 1 | number of levels = `RwRasterGetNumLevels(raster)` | Write+0xF4 | Read+0xFA, +0x4FE |
| 0x56 | 1 | raster type (cType) | Write+0xFD | Read+0xA4 / +0x355 (low byte of RwRasterCreate flags) |
| 0x57 | 1 | flags (1.4) | Write+0x104..+0x134 | Read+0x82 (`test ah`), +0x10B, +0x3E9 |
| 0x58 | 0, 128 or 1024 | palette (1.5) | Write+0x166..+0x1F3 | Read+0x444..+0x4C1 |
| ... | | per face, per level: u32 size + size bytes (1.6) | Write+0x20D..+0x362 | Read+0x4E8..+0x598 |

The struct chunk's own length (returned by `RwStreamFindChunk`, Read+0x1B) is **not** used by the reader: nothing
after Read+0x2B reads it, and the reader never skips remaining bytes. The third argument of the read function is not
referenced either.

#### Name / mask bytes

The writer copies all 32 bytes, including whatever follows the terminating NUL. `_RwTextureCreate+0x42/+0x45` clears
only name[0] and mask[0], so a texture whose mask was never set carries stale bytes from a reused free-list entry:
**(corpus)** 52 of 148 mask fields contain e.g. `00 'ask_31' 00...` after the NUL. Readers must stop at the first NUL.
The reader hands the 32-byte buffers to `RwTextureSetName` / `RwTextureSetMaskName`, which copy with n = 0x20
(`_RwTextureSetName+0x13`, engine string function at `RwEngineInstance+0xD0`) and, if `strlen >= 32`
(`+0x27..+0x30`), raise `E_RW_STRING_TRUNCATION` (0x8000001E, pluginID 1) and force name[31] = 0 (`+0x62`). A
32-byte name with no NUL therefore makes `strlen` run past the buffer (name into mask; mask into the reader's stack
frame) - hazard, not a feature.

### 1.3 Filter / addressing word

Write+0x4E..+0x7A: `word = (fa & 0xFF) | ((fa >> 8) & 0xF) << 8 | ((fa >> 12) & 0xF) << 12` with
`fa = RwTexture.filterAddressing` - i.e. bits 0-7 filter mode, 8-11 addressing U, 12-15 addressing V
(rwcore.h:2358-2360), bits 16-31 written as 0.

Read: `RwTextureCreate` first sets filterAddressing = 0, then byte +0x50 = 1 and byte +0x51 = 0x11
(`_RwTextureCreate+0x2B/+0x36/+0x4B`); the reader then stores byte 0 = word & 0xFF (Read+0x5BD), bits 8-11 =
(word >> 8) & 0xF (Read+0x5C8..+0x5D7), bits 12-15 = (word >> 12) & 0xF (Read+0x5DE..+0x5EC). Bits 16-31 of the
file word are ignored; bits 16-31 of the texture stay 0.

### 1.4 Flags byte (payload +0x57)

| bit | meaning | Write | Read |
|---|---|---|---|
| 0x01 | alpha: ext+8 != 0 | +0x109..+0x113 | stored to ext+8 only on the compressed and cube paths (+0x1BA, +0x244, +0x349); ignored on the other two paths |
| 0x02 | cube texture: ext+9 & 0x0F | +0x118 | +0x10B (compressed), +0x24F |
| 0x04 | Direct3D auto mipmap: ext+0xA & 0x0F | +0x123 | +0x3E9: if set, the created raster must also have it, else fail |
| 0x08 | compressed (DXT): ext+0xA & 0xF0 | +0x12E | +0x86: selects the compressed path |
| 0x10-0x80 | always 0 on write (byte cleared at +0x104) | | not tested |

### 1.5 Palette

Written only when the format word has PAL4 (0x4000) or PAL8 (0x2000) (Write+0x171, +0x1B4):

- PAL4: `RwRasterLockPalette(raster, rwRASTERLOCKREAD)`, **0x80 bytes = 32 entries** (Write+0x182). Note: 32, not 16.
- PAL8: 0x400 bytes = 256 entries (Write+0x1C1).

The locked palette is the RW-side block at ext+4 (`__rwD3D9RasterLockPalette+0x4F/+0x56`), the same block later passed
to `IDirect3DDevice9::SetPaletteEntries(ext4[+0x400], ext4)` (`__rwD3D9RasterUnlockPalette+0x5A..+0x65`); each entry
is therefore a `PALETTEENTRY` (wingdi.h:1428): bytes R, G, B, flags (alpha). The u32 palette number at +0x400 is not
written. **(corpus)** no palettised dictionary exists (section 3.3), so the palette bytes are not byte-checked.

### 1.6 Level data

Write+0x1F7..+0x362:

- faces = 6 if ext+9 & 0x0F (cube) else 1 (Write+0x1F7..+0x209). Before the loop the face nibble is set to 0
  (Write+0x1FE); after each face it is incremented (Write+0x346..+0x356). Faces are therefore written in
  `D3DCUBEMAP_FACES` order 0..5 (+X, -X, +Y, -Y, +Z, -Z; d3d9types.h:1682-1687), because `__rwD3D9RasterLock+0xB6..+0xBF`
  passes `ext+9 >> 4` as the face to `GetCubeMapSurface`. Side effect: after a cube write the raster's face nibble is 6.
- per face, levels 0 .. numLevels-1 (numLevels = header byte +0x55); with Direct3D auto mipmap (ext+0xA & 0x0F) only
  level 0 (Write+0x327..+0x329).
- per level: `RwRasterLock(raster, level, rwRASTERLOCKREAD)` (Write+0x229; NULL -> write fails, +0x235),
  `GetLevelDesc(level)` (+0x248), then

  ```
  size = compressed ? (desc.Height < 4 ? Pitch : (desc.Height >> 2) * Pitch)   Write+0x24B..+0x266
                    : Pitch * desc.Height                                       Write+0x26C..+0x274
  ```

  where Pitch = ext+0x10 = the `D3DLOCKED_RECT.Pitch` of this lock (`__rwD3D9RasterLock+0xE6..+0x122` locks the D3D
  surface directly into ext+0x10). The u32 size is written (+0x287), then `size` bytes straight from the locked
  pointer in 1024-byte pieces plus the rest (+0x29F..+0x2FD), then unlock (+0x31C).

Consequences, all in the code above:

1. Rows are stored with the device's surface pitch, padding included. Nothing in the file says the pitch; it is
   `size / rows`. **(corpus)** on the RTX 4080 the pitch is the row size rounded up to 4 bytes: a 1x1 LUM8 level is
   stored as 4 bytes (lum8.txd, `lum8_1x1_48`: size 4, bytes `80529760`, the 3 padding bytes are the random fill the
   generator wrote over the whole pitch), 1-pixel 16-bit levels as 4, 3-pixel 16-bit levels as 8, 6-pixel LUM8 as 8;
   72 padded levels in total, 0 for 32-bit formats and 0 for DXT.
2. DXT: rows = `h >> 2` (floor), or 1 if h < 4. For a level whose height is >= 4 and not a multiple of 4 the last
   block row is **not written**. **(corpus)** `dxt1_24x40_mip_191` (dxt1.txd): level 2 (6x10) size 32 = 2 block rows of
   16 although D3D has 3; level 3 (3x5) size 8 although D3D has 2 block rows. 6 such levels in dxt1/3/5.
3. No per-row conversion, no swizzle: the bytes are the D3D9 surface memory (A8R8G8B8 etc. in D3D byte order, DXT
   blocks as D3D stores them).

### 1.7 Size (`__rwD3D9NativeTextureGetSize`)

Signature from the call at Write+0x15..+0x19: `GetSize(RwUInt32 *sizeOut, RwTexture *texture, 0)`.

- texture->raster == NULL: size = 0x54 (= 12 + 72) (GetSize+0x0D..+0x16). (The writer would dereference the NULL
  raster at Write+0xCF; there is no check.)
- else size = 0x64 (= 12 + 72 + 16) (+0x22), + 0x80 if PAL4 (+0x32), else + 0x400 if PAL8 (+0x41).
- plus, for each level (only level 0 with Direct3D auto mipmap: +0x6D / +0xF1), 4 + level size, computed exactly as in
  1.6 but from `IDirect3D*Texture9::LockRect(…, D3DLOCK_READONLY | D3DLOCK_NO_DIRTY_UPDATE)` (flags 0x8010,
  +0x91 / +0x119) on face 0, and level count from `GetLevelCount` (+0x79 / +0xFF).
- cube: the level sum is multiplied by 6 (+0xDD..+0xE6: `ecx + 6*ebx`).

The returned size includes the 12-byte struct header (Write+0x29 subtracts 12 for the header length).
**(corpus)** 148/148 struct chunks: `length + 12 == 0x64 + palette + sum(4 + size)`.

## 2. The read path (`__rwD3D9NativeTextureRead(RwStream*, RwTexture **out, RwInt32 unused)`)

### 2.1 Validation and failure exits

`__rwD3D9NativeTextureRead` itself never calls `_rwerror` / `RwErrorSet` (its relocation list, d3d9texdic.asm
lines 1071-1109, has neither). Every failure returns FALSE; RwErrors come only from callees.

| # | check | where | result |
|---|---|---|---|
| 1 | `RwStreamFindChunk(stream, rwID_STRUCT, &len, &ver)` fails | Read+0x1B..+0x25 | FALSE (any error is set by RwStreamFindChunk: **UNKNOWN**, bastream.obj not disassembled) |
| 2 | ver < 0x35000 or ver > 0x37002 | +0x2B..+0x3F | FALSE, no error |
| 3 | read of 0x48 bytes short | +0x4D..+0x58 | FALSE |
| 4 | platform id != 9 | +0x5E | FALSE |
| 5 | read of 16 bytes short | +0x71..+0x7C | FALSE |
| 6 | compressed: `_rwD3D9CheckValidTextureFormat(d3dFormat)` false | +0x94..+0x9E | FALSE |
| 7 | `RwRasterCreate` / `RwD3D9RasterCreate` returns NULL | +0xE4, +0x292, +0x391, +0x3DA | FALSE (errors from the driver, 3.1) |
| 8 | cube and levels > 1 and `(CubeTextureFilterCaps & (MIPFPOINT|MIPFLINEAR)) == 0` | +0x116..+0x125, +0x2B9..+0x2C8 | raster destroyed, FALSE |
| 9 | `CreateCubeTexture` / `CreateTexture` HRESULT < 0 | +0x187, +0x21E, +0x334 | raster destroyed, FALSE |
| 10 | file flag 0x04 (auto mipmap) set but the raster has no D3D auto mipmap | +0x3E9..+0x3F4 | raster destroyed, FALSE |
| 11 | file format word != `raster->cFormat << 8` | +0x3FA..+0x405 | raster destroyed, FALSE |
| 12 | file D3DFORMAT != ext+0x18 | +0x40B..+0x414 | raster destroyed, FALSE |
| 13 | file width != raster width / file height != raster height | +0x41A..+0x43E | raster destroyed, FALSE |
| 14 | palette read short (0x80 / 0x400) | +0x460..+0x476, +0x49C..+0x4B2 | palette unlocked, raster destroyed, FALSE |
| 15 | a level's u32 size read short | +0x53B..+0x53E | raster unlocked + destroyed, FALSE |
| 16 | a level's data read short (`RwStreamRead != size`) | +0x55E..+0x561 | raster unlocked + destroyed, FALSE |
| 17 | `RwTextureCreate` returns NULL | +0x5B1..+0x5B3 | raster destroyed, FALSE |

Not validated: the struct chunk length; the depth byte (only an argument to RwRasterCreate, which replaces it,
`__rwD3D9RasterCreate+0x1EC`); the level size against the locked buffer (the reader reads `size` bytes straight into
the locked pointer, +0x544..+0x552 - a size larger than the D3D level overruns it); the `RwRasterLock` result (ebx is
used without a NULL test, +0x51D..+0x550); numLevels against the created raster's real level count.

Errors that callees can set on this path: `E_RW_INVRASTERFORMAT` (0x8000000D), `E_RW_DEVICEERROR` (core code 2 with
a string), `E_RW_INVRASTERLOCKREQ` (0x8000000E), `E_RW_INVRASTERUNLOCKREQ` (0x80000011), `E_RW_STRING_TRUNCATION`
(0x8000001E); see 3.1 and 1.2. The dictionary reader adds `E_RW_BADVERSION` (0x80000004) when a TEXTURENATIVE chunk's
version is outside 0x35000..0x37002 (`_RwTexDictionaryStreamRead+0xEE..+0x102`, `+0x1A4`), and rejects (FALSE, no
error) a dictionary whose device id differs from the running device when both are non-zero
(`_RwTexDictionaryStreamRead+0x84..+0xAB`).

### 2.2 Which create function

Decided only by the file header (Read+0x82..+0x3E6):

| file | path | raster creation | D3D texture |
|---|---|---|---|
| flags & 0x08 (compressed) | Read+0x8F | `RwRasterCreate(w, h, depth, type | rwRASTERDONTALLOCATE | format)` (+0xD8) | the reader itself: `CreateCubeTexture(width, levels, usage, d3dFormat, D3DPOOL_MANAGED)` if flags & 0x02 (+0x184), else `CreateTexture(width, height, levels, usage, d3dFormat, D3DPOOL_MANAGED)` (+0x219); d3dFormat from the file is stored to ext+0x18 (+0x1B0 / +0x23A), compressed nibble set (+0x1AD / +0x238), alpha = flags & 1 |
| else flags & 0x02 (cube) | Read+0x258 | `RwRasterCreate(..., type | DONTALLOCATE | format)` (+0x286) | `CreateCubeTexture(width, levels, usage, d3dFormat from file, MANAGED)` (+0x32F); alpha = flags & 1; ext+0x18 is **not** written |
| else format & 0xFFFF6FFF != 0 (a pixel format and/or PAL bits) | Read+0x362 | `RwRasterCreate(w, h, depth, type | format)` - allocating (+0x385) | created by the driver (3.1); compressed nibble cleared (+0x3A4) |
| else (format is only MIPMAP / AUTOMIPMAP bits) | Read+0x3AB | `RwD3D9RasterCreate(w, h, d3dFormat, type | (format & 0x9000))` (+0x3CE) | created by RwD3D9RasterCreate (3.2) |

Levels and usage on the two paths where the reader creates the texture: levels = numLevels from the file if the
created raster has rwRASTERFORMATMIPMAP (`cFormat & 0x80`, +0xF0..+0x106 / +0x29E..+0x2B4), else 1; usage =
`D3DUSAGE_AUTOGENMIPMAP` (0x400) if the auto mipmap nibble is set (+0x160..+0x176), else 0. The auto mipmap nibble is
set when `(format & 0x9000) == 0x9000` and `_rwD3D9CheckAutoMipmapGen[Cube]TextureFormat(d3dFormat)` (+0x135..+0x15D,
+0x1C8..+0x1EE, +0x2DA..+0x304) - and may already have been set inside RwRasterCreate from the RW pixel format
(`__rwD3D9RasterCreate+0x1FF..+0x244`), so it is the OR of both tests.

Code-derived finding (not exercised by the corpus, which has no cube textures): on the uncompressed cube path nothing
writes ext+0x18 - `__rwD3D9RasterCreate+0x44` zeroes it, the DONTALLOCATE raster never reaches
`_rwD3D9CreateTextureRaster` (`__rwD3D9RasterCreate+0x482..+0x487`), and Read+0x258..+0x34C does not store it - so the
check at Read+0x40B compares the file's (non-zero) D3DFORMAT with 0 and fails. As written, the original reader cannot
read back an uncompressed cube native texture. Compressed cubes store ext+0x18 (Read+0x1B0) and pass.

### 2.3 Unsupported formats: no fallback

The read function calls no conversion function (relocations, d3d9texdic.asm lines 1071-1109): no palettised to
non-palettised conversion, no DXT decompression, no format substitution. When the device refuses the format, creation
fails and the read returns FALSE (exits 6, 7, 9). `__rwD3D9RasterConvertToNonPalettized` (d3d9raster.asm 7600) exists
but its only user among the inspected libraries is rpmatfx.lib (undefined external in its symbol table); it is not on
the native texture path.

### 2.4 Filling the levels

Read+0x4C4..+0x598:

1. Palette first (if PAL4 / PAL8): `RwRasterLockPalette(raster, rwRASTERLOCKWRITE)`, read 0x80 / 0x400 bytes into it,
   unlock -> `__rwD3D9RasterUnlockPalette` uploads with `SetPaletteEntries` because the write flag (privateFlags 0x10)
   was set by the lock (`__rwD3D9RasterLockPalette+0x5B`, unlock +0x44..+0x65).
2. `rwRASTERFORMATAUTOMIPMAP` (cFormat 0x10) is removed from the raster for the duration (Read+0x4C4..+0x4D6) and
   restored after the last level (+0x59E..+0x5A4). Reason in the code: `__rwD3D9RasterUnlock+0x87..+0xBF` calls
   `RwTextureRasterGenerateMipmaps` when level 0 was locked for writing, the raster has AUTOMIPMAP and no D3D auto
   mipmap; clearing the bit keeps the stored levels instead of regenerating them.
3. faces = 6 if cube else 1; face nibble reset to 0 (+0x4D9..+0x50C).
4. level count = 1 if the created raster has D3D auto mipmap (ext+0xA & 0x0F), else numLevels from the file
   (+0x4EC..+0x508) - independent of the MIPMAP flag.
5. per level: `RwRasterLock(raster, level, rwRASTERLOCKWRITE)` (+0x51D; D3D lock flags = D3DLOCK_NOSYSLOCK 0x800,
   `__rwD3D9RasterLock+0x57`), read u32 size (+0x533), read `size` bytes directly into the locked pointer (+0x552),
   unlock (+0x567). One bulk copy: no per-row copy, no pitch handling - the file must have been written with the same
   pitch the reading device returns.
6. after each face the face nibble is incremented (+0x57C..+0x58C).

Bytes of a level the file does not cover (the truncated DXT block row, 1.6) keep whatever the new managed texture held.
**(corpus)** all 6 truncated tails read back as zeros on the RTX 4080; that the D3D9 runtime zero-fills a new
MANAGED texture is **(D3D9 runtime, external)** / not guaranteed by RenderWare code.

Cross-device hazard in the code: if the reading device has D3D auto mipmap for the format but the writing device did
not, the file holds numLevels levels while the reader reads 1 (step 4) and leaves the rest in the stream; the caller
then reads the texture's extension chunk from the wrong position (`_RwTexDictionaryStreamRead+0x135`). The opposite
case is exit 10.

### 2.5 What reaches the texture

`RwTextureCreate(raster)` (+0x5A7): raster, refCount 1; filter / addressing from the file (1.3); name and mask via
`RwTextureSetName` / `RwTextureSetMaskName` (+0x5EF / +0x5FA); `*out = texture` (+0x60E); TRUE. The raster carries the
file's format word (re-checked, exit 11) with AUTOMIPMAP restored, the file's width / height (exit 13), the D3D format
(exit 12), alpha (from the file flag on the compressed and cube paths, from the format table / RwD3D9RasterCreate
otherwise), cube / compressed / auto mipmap nibbles, and the palette. The dictionary reader adds the texture to the
dictionary after reading its extension chunk (`_RwTexDictionaryStreamRead+0x135..+0x147`). **(corpus)** the dictionary
the reader builds lists the textures in reverse file order (10/10 files; `RwTexDictionaryAddTexture` was not
disassembled, so the mechanism is not cited).

## 3. Raster-format facts from d3d9raster

### 3.1 `__rwD3D9RasterCreate` (rwSTANDARDRASTERCREATE) for normal (0) / texture (4) rasters

`_rwRasterConvTable` (d3d9raster.obj `.rdata`, offset 0, 0x58 bytes = 11 entries of {u32 D3DFORMAT, u8 depth,
u8 alpha, 2 bytes 0}, indexed by `(format >> 8) & 0xF`; used at `__rwD3D9RasterCreate+0x1A5/+0x1C8/+0x1E6`,
`_rwD3D9CreateTextureRaster+0x2B/+0xE6`):

| index | rwRASTERFORMAT | D3DFORMAT | depth | alpha |
|---|---|---|---|---|
| 0 | DEFAULT | 0 | 0 | 0 |
| 1 | 1555 | 25 D3DFMT_A1R5G5B5 | 16 | 1 |
| 2 | 565 | 23 D3DFMT_R5G6B5 | 16 | 0 |
| 3 | 4444 | 26 D3DFMT_A4R4G4B4 | 16 | 1 |
| 4 | LUM8 | 50 D3DFMT_L8 | 8 | 0 |
| 5 | 8888 | 21 D3DFMT_A8R8G8B8 | 32 | 1 |
| 6 | 888 | 22 D3DFMT_X8R8G8B8 | 32 | 0 |
| 7 | 16 | 80 D3DFMT_D16 | 16 | 0 |
| 8 | 24 | 77 D3DFMT_D24X8 | 32 | 0 |
| 9 | 32 | 71 D3DFMT_D32 | 32 | 0 |
| 10 | 555 | 24 D3DFMT_X1R5G5B5 | 16 | 0 |

Indices 11-15 are past the table; nothing bounds-checks the index (contents there **UNKNOWN**: whatever the linker put
after `.rdata`).

Format selection (`__rwD3D9RasterCreate+0x6D..+0x1EF`):

- pixel format DEFAULT (0):
  - with PAL8: requires `_rwD3D9CheckValidTextureFormat(D3DFMT_P8)` (+0x86) else FALSE with no error (+0x92 -> +0x478);
    pixel format = 8888 if `TextureCaps & D3DPTEXTURECAPS_ALPHAPALETTE` (byte +0x3C bit 7, +0x98..+0x9F) else 888;
    depth 8.
  - else: if adapter depth (`_RwD3D9AdapterInformation+0x214`) > 16 and A8R8G8B8 valid -> 8888, depth 32
    (+0xC5..+0xE8); else A4R4G4B4 valid -> 4444, else 1555, depth 16 (+0xFF..+0x11A). (PAL4 is not tested on this
    branch.)
- explicit pixel format:
  - with PAL4: `E_RW_INVRASTERFORMAT` (0x8000000D, pluginID 1), FALSE (+0x11F..+0x153) - **always**, on every device.
  - with PAL8: P8 must be valid (+0x15C..+0x168, FALSE without error), pixel format must be 888, or 8888 only with
    ALPHAPALETTE (+0x16E..+0x18B, FALSE without error otherwise); depth 8.
  - else: table D3DFORMAT; depth formats (70..80) are checked with `_rwD3D9CheckValidZBufferTextureFormat`, others with
    `_rwD3D9CheckValidTextureFormat` (+0x1AB..+0x1DE); invalid -> FALSE without error; depth = table depth.
- AUTOMIPMAP: if not PAL8 and `(flags & 0x9000) == 0x9000` and
  `_rwD3D9CheckAutoMipmapGenTextureFormat(table[pixel format].d3dFormat)` -> ext+0xA auto mipmap nibble (+0x1F3..+0x244).
- cFormat = format >> 8 (+0x3FB). width or height 0 -> DONTALLOCATE, depth 16 if 0, TRUE (+0x5BA..+0x5D3).
- type outside 0..5 or 3 -> `E_RW_INVRASTERFORMAT` (+0x455). DONTALLOCATE set -> TRUE without a D3D texture
  (+0x482..+0x487). Else `_rwD3D9CreateTextureRaster`.

`_rwD3D9CreateTextureRaster`:

- non-palettised: ext+0x18 = table D3DFORMAT (+0x1F..+0x32). Palettised: pixel format must be 888 / 8888 else
  `E_RW_DEVICEERROR` "Palletized texture can only be rwRASTERFORMAT888 or rwRASTERFORMAT8888." (+0x3A..+0x4D); a
  palette block and palette number are allocated (+0x52..+0xC7) and ext+0x18 = 41 D3DFMT_P8 (+0xD1).
- ext+8 alpha = table alpha (+0xE6..+0xEF).
- `_rwD3D9CheckRasterSize(&width, &height, type | format)` (+0xFA) may change the raster's width / height (3.4).
- `CreateTexture(width, height, levels, usage, ext+0x18, D3DPOOL_MANAGED, &ext->texture)` (+0x1A0..+0x1CE) with
  levels = 0 (full chain) if MIPMAP else 1 (+0x1B7..+0x1C8: `(~format >> 15) & 1`), usage = AUTOGENMIPMAP if the
  nibble is set. Depth formats use `D3DUSAGE_DEPTHSTENCIL, D3DPOOL_DEFAULT, 1 level` (+0x13F..+0x166). Failure:
  palette returned to the free list, `E_RW_DEVICEERROR` "Failed to create texture." (+0x1DB..+0x2A2), FALSE.

The validity checks are all `IDirect3D9::CheckDeviceFormat(adapter, deviceType, AdapterFormat, Usage, RType, fmt)`:

| function | Usage | RType | success test |
|---|---|---|---|
| `_rwD3D9CheckValidTextureFormat` (7079) | 0 | 3 TEXTURE | `>= 0` (+0x2D) |
| `_rwD3D9CheckValidCubeTextureFormat` (7256) | 0 | 5 CUBETEXTURE | `>= 0` |
| `_rwD3D9CheckAutoMipmapGenTextureFormat` (7368) | 0x400 AUTOGENMIPMAP | 3 | Caps2 & D3DCAPS2_CANAUTOGENMIPMAP and `== 0` exactly (+0x37..+0x3C) - D3DOK_NOAUTOGEN (a success code, d3d9types.h:1627-1629) counts as "no" |
| `_rwD3D9CheckAutoMipmapGenCubeTextureFormat` (7484) | 0x400 | 5 | same |

AdapterFormat is `_RwD3D9AdapterInformation+0x210`, copied from dword +0x0C of the selected `_DisplayModes` entry
(d3d9device.asm line 159, the D3DDISPLAYMODE.Format position); +0x214 is a colour depth (32 / 16 / 0) derived from that
format through jump table `$L54260` (lines 166-179; the table bytes were not decoded).

### 3.2 `RwD3D9RasterCreate(width, height, d3dFormat, flags)`

- type normal / zbuffer -> MIPMAP / AUTOMIPMAP cleared (+0x0A..+0x14).
- `RwRasterCreate(width, height, 0, flags | rwRASTERDONTALLOCATE)` (+0x2B): format / depth defaults from 3.1.
- auto mipmap if `(flags & 0x9000) == 0x9000`, Caps2 CANAUTOGENMIPMAP and `CheckDeviceFormat(AUTOGENMIPMAP, TEXTURE,
  d3dFormat) == D3D_OK` (+0x47..+0x9E).
- camera texture (type 5): render-target checks, D3DPOOL_DEFAULT (+0xC2..+0x12F); depth formats: depth-stencil checks
  (+0x175..+0x1DE); else `CheckDeviceFormat(0, TEXTURE, d3dFormat)` must succeed, pool MANAGED (+0x1E3..+0x210).
- `CreateTexture(width, height, MIPMAP ? 0 : 1, usage, d3dFormat, pool)` (+0x133..+0x15A); failure -> raster
  destroyed, NULL (+0x165). No `_rwD3D9CheckRasterSize`: the requested size is used as is.
- then DONTALLOCATE cleared, cFormat &= 0xF0, ext+0x18 = d3dFormat (+0x258..+0x270);
  - D3DFORMAT numerically in 'DXT1'..'DXT5' (0x31545844..0x35545844): compressed nibble = 1 (+0x27D..+0x28A); DXT1:
    alpha 0, cFormat |= 2 (565), depth 16 (+0x28F..+0x29D); others: alpha 1, cFormat |= 3 (4444), depth 16
    (+0x2A9..+0x2B7).
  - else compressed nibble 0; if d3dFormat < 0x80: alpha, depth and RW pixel format from `_rwD3D9PixelFormatInfo`
    (+0x2D8..+0x2F8; a 0x200-byte COMMON array filled outside the inspected objects - contents **UNKNOWN**), and P8
    gets a palette block (+0x2FA..+0x385). d3dFormat >= 0x80 (other FOURCCs): alpha / depth not set.

### 3.3 Why the corpus has no palettised rasters

texcorpus.c makes them with `RwRasterCreate(w, h, 0, rwRASTERTYPETEXTURE | [MIPMAP] | PAL4/PAL8 | 8888/888)`.

- PAL4 + 8888 / PAL4 + 888: refused on any device by `__rwD3D9RasterCreate+0x11F..+0x153` (`E_RW_INVRASTERFORMAT`).
  This is not a device capability.
- PAL8 + 888: the only device-dependent exits are `CheckDeviceFormat(TEXTURE, D3DFMT_P8)` failing
  (`__rwD3D9RasterCreate+0x15C..+0x168`, no RwError) or `CreateTexture(D3DFMT_P8)` failing
  (`_rwD3D9CreateTextureRaster+0x1D3..+0x2A2`, E_RW_DEVICEERROR "Failed to create texture."). Which of the two fired
  on the RTX 4080 is **UNKNOWN**: the corpus run did not record `RwErrorGet` after the refusal. A probe that prints
  RwErrorGet after the failing RwRasterCreate would settle it.
- PAL8 + 8888: the same two exits, plus `TextureCaps & D3DPTEXTURECAPS_ALPHAPALETTE` (+0x184..+0x18B, no RwError).

### 3.4 `_rwD3D9CheckRasterSize(int *w, int *h, flags)` (d3d9convrt.obj)

Clamps to MaxTextureWidth / MaxTextureHeight (+0x04..+0x1D). For textures with MIPMAP: if TextureCaps & POW2, rounds
both down to a power of two (+0x23..+0x59). Otherwise: if POW2 and not NONPOW2CONDITIONAL, rounds down (+0x5B..+0x95).
If SQUAREONLY, both become the smaller (+0x97..+0xAC). Only the allocating RwRasterCreate path runs it; a change makes
the reader fail at exit 13. **(corpus)** 24x40 rasters kept 24x40 on the RTX 4080, so none of these changed anything
there (the caps values themselves were not recorded).

### 3.5 Why DXT 1x1 and 8x2 were refused

texcorpus.c uses `RwD3D9RasterCreate(w, h, DXTn, TEXTURE [| MIPMAP])`. In 3.2 the only step that depends on the size
is `CreateTexture` (`RwD3D9RasterCreate+0x15A`); its failure destroys the raster and returns NULL (+0x165..+0x174).
So the 1x1 and 8x2 refusals are `IDirect3DDevice9::CreateTexture` failures. The reason inside Direct3D 9 (DXTn
top-level width and height must be multiples of 4: 1x1 and 8x2 violate it, 4x4 / 16x64 / 64x64 / 128x32 / 256x256 /
24x40 do not) is **(D3D9 runtime, external)**, not in RenderWare code. **(corpus)** dxt1/3/5 contain exactly those 6
sizes, with and without mip levels (12 textures each).

### 3.6 Mip levels

`RwRasterGetNumLevels`: 1 without MIPMAP, else the driver's GetMipLevels (`_RwRasterGetNumLevels+0x04..+0x24`).
`__rwD3D9RasterGetMipLevels`: types 0 / 4 / 5 with a D3D texture -> `GetLevelCount()` (+0x33..+0x4A); without a texture
-> number of bits of max(w, h) (+0x52..+0x67); other types 1. With D3D auto mipmap, the count is whatever
GetLevelCount returns for an AUTOGENMIPMAP texture (**(D3D9 runtime, external)**; the corpus has no such texture).
**(corpus)** full chains: 4x4 -> 3, 8x2 -> 4, 16x64 -> 7, 24x40 -> 6, 128x32 -> 8, 256x256 -> 9.

## 4. Checked against the corpus

`python -I .build/rw_pc_oracle/txd_check.py` over all 10 dictionaries (148 textures, 509 levels). Each line is a fact
of sections 1-3 and how many textures / levels confirmed it; failures = 0 except the one noted.

| fact | count | example (file / texture) |
|---|---|---|
| TXD chunk 0x16, length = file - 12, library id 0x1C02000A | 10/10 | all |
| TXD struct: length 4, u16 count = textures, u16 device id = 2 (rwDEVICE_D3D9, rwplcore.h:5722) | 10/10 | dxt1.txd: `0c 00 02 00` |
| TEXTURENATIVE 0x15, struct 0x01, library id 0x1C02000A | 148/148 | dxt1.txd first native chunk at 0x1C, length 0x318 |
| platform id 9 | 148/148 | dxt1.txd 0x34: `09 00 00 00` |
| filter word = filter | U<<8 | V<<12, bits 16-31 zero | 148/148 | `dxt1_24x40_mip_191`: `02 24 00 00` = filter 2, U 4, V 2 (ref) |
| name / mask = ref, NUL terminated, 32 bytes each | 148/148 | |
| name bytes after the NUL are 0 | 148/148 | |
| mask bytes after the NUL are 0 | 96/148 - **52 carry stale bytes** (1.2) | 4444.txd: `00 'ask_31' 00 ...` |
| format word = ref RwRasterGetFormat, only bits 8-15 | 148/148 | `dxt1_24x40_mip_191`: `00 82 00 00` |
| D3DFORMAT and depth = `_rwRasterConvTable` (uncompressed) | 112/112 | `lum8_24x40_mip_63`: `32 00 00 00` (L8), depth 8 |
| DXT: FOURCC D3DFORMAT, pixel format 0x200 (DXT1) / 0x300 (DXT3, DXT5), depth 16 | 36/36 | `dxt1_24x40_mip_191`: `44 58 54 31`, depth `10` |
| alpha bit = table alpha / (DXT1 0, DXT3/5 1) | 148/148 | |
| width, height, depth, levels, type 4 = ref | 148/148 | `dxt1_24x40_mip_191`: `18 00 28 00 10 06 04 08` |
| compressed bit = ref; cube, auto-mipmap, bits 4-7 clear | 148/148 | |
| level size = pitch x h, DXT pitch x (h >> 2) or pitch if h < 4 | 509/509 | `dxt1_24x40_mip_191` sizes 480, 120, 32, 8, 8, 8 |
| level bytes = the reader's locked level (first `size` bytes) | 509/509 | `lum8_1x1_48`: size 4, `80529760` |
| padded pitch stored in the file | 72 levels | LUM8 / 16-bit levels 1-6 px wide |
| DXT level truncated by h >> 2, tail reads back 0 | 6/6 | `dxt1_24x40_mip_191` levels 2 and 3 |
| struct length + 12 = GetSize (0x64 + palette + sum(4 + size)) | 148/148 | |
| texture extension chunk 0x03 of length 0 after the struct | 148/148 | |
| native chunk length = struct + extension; dictionary extension 0x03 length 0; file fully consumed | 10/10 | |
| reader's dictionary order = file order reversed | 10/10 | |

Not checkable with this corpus: palettes (none could be created, 3.3), cube textures, Direct3D auto mipmaps
(texcorpus.c sets only rwRASTERFORMATMIPMAP), camera textures, the D3D9 pitch on other devices, and every failure exit
of 2.1. Note for extending the corpus: texcorpus.c dumps and fills 16 palette entries for PAL4, the driver stores 32.

## 5. Where the original depends on Direct3D 9 device capabilities

For a Direct3D 12 implementation these are the places where the original's result is a property of the D3D9 device,
not of the file:

1. **Surface pitch is in the file** (1.6): level sizes and bytes are D3D9 `LockRect` pitch x rows. A reader must take
   the source pitch as `size / rows` and copy `width x bpp` (or block-row bytes) per row; it must not assume pitch =
   width x bpp (RTX 4080: rows padded to 4 bytes).
2. **DXT floor rows** (1.6): files lose the last block row of levels with h >= 4 and h % 4 != 0; that region is
   whatever the new texture held (zeros on the corpus device).
3. **Format support** via `CheckDeviceFormat` against the display-mode format: P8 (palettised - refused, no
   conversion), R5G6B5 / A4R4G4B4 (needed by RwRasterCreate on the DXT1 / DXT3/5 read path, because the DXT raster is
   created with pixel format 565 / 4444), the DXT FOURCCs, every table format. Unsupported -> the read fails.
4. **ALPHAPALETTE** (TextureCaps bit 0x80) decides whether PAL8 + 8888 exists at all.
5. **Auto mipmap generation** (Caps2 CANAUTOGENMIPMAP + CheckDeviceFormat(AUTOGENMIPMAP) == S_OK): changes how many
   levels the writer stores and the reader consumes; a mismatch between writing and reading device either fails
   (exit 10) or desynchronises the stream (2.4).
6. **Cube mip filtering** (CubeTextureFilterCaps MIPFPOINT | MIPFLINEAR): without it, mipmapped cube textures fail
   (exit 8).
7. **Texture size caps** (MaxTextureWidth / Height, POW2, NONPOW2CONDITIONAL, SQUAREONLY) through
   `_rwD3D9CheckRasterSize`: a resized raster fails the read (exit 13).
8. **Display depth** (`_RwD3D9AdapterInformation+0x214`) picks the default pixel format (8888 vs 4444 / 1555) of
   DEFAULT-format rasters, and with it the auto mipmap pre-test inside RwRasterCreate.
9. **D3D9 runtime rules** outside RenderWare: DXTn creation sizes (3.5), full mip chain length for Levels = 0, the
   level count reported for AUTOGENMIPMAP textures, initial contents of a new MANAGED texture.

## UNKNOWN (summary)

- The exact chunk-header library-id encoding and RwStreamFindChunk's error behaviour (bastream.obj not disassembled;
  only the bytes 0x1C02000A are checked).
- Which of the two possible exits refused PAL8 rasters on the RTX 4080 (3.3).
- `_rwD3D9PixelFormatInfo` contents (filled outside the inspected objects); only matters for RwD3D9RasterCreate with
  non-RW D3D formats.
- The bytes after `_rwRasterConvTable` for format indices 11-15.
- The purpose of raster extension +0x1C.
- The depth values in jump table `$L54260` (d3d9device.asm) beyond "32 / 16 / 0".
- Palette bytes, cube layout and auto-mipmap files are specified from the code only; no corpus file exercises them.
