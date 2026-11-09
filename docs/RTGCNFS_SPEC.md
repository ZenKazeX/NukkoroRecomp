# RtGCNFS specification (RenderWare GameCube file system device), PAL main.dol

Read-only reverse engineering of 0x8047B818..0x8047C770 plus the RtFSManager layer 0x8047A5E8..0x8047B818.
Everything below was read from the disassembly (`tools/doldis.py`). The struct names come from the RW 3.7 SDK header
`RW37SDK/Library D3D9 Hdr Files/rtfsyst.h`. Its `RtFile` / `RtFileSystem` layouts match every offset used here.
Anything not proven is marked **UNCERTAIN**.

Conventions:
- `file` is an `RtFile*` (0x90-byte record) and `fs` is an `RtFileSystem*` (0x64-byte object).
- `G` = the word at 0x2A40(r13) = 0x805EEF40. Its DOL initial value is 1.
- `ERRCB(code)` means: `if (fs+0x5C != 0) (*(fs+0x5C))(r3 = code, r4 = *(fs+0x60))`. The device never writes
  fs+0x60 (see section 3).
- `ENG` = `*(r13+0x615C)`, the RwEngineInstance. Its slots:
  - +0x108 malloc(size, hint)
  - +0x10C free(p)
  - +0x114 calloc(n, size, hint)
  - +0xCC strcpy
  - +0xE8 strcmp (FS names)
  - +0xF0 string compare for device names (strcmp or stricmp, **UNCERTAIN** which)
  - +0xF4 strlen
- Every allocation hint is 0x401BE.
- 64-bit fields are big-endian `RwInt64`: the high word is at +0 and the low word at +4.

## 0. Identified helpers

| addr | identity | evidence |
|---|---|---|
| 0x80378A08 | OSYieldThread | OSDisableInterrupts (0x8037611C), then 0x803787B0(1) (SelectThread(TRUE)), then OSRestoreInterrupts (0x80376144). Name from the SDK pattern. |
| 0x803A1EE0 / 0x803A1EE8 / 0x803A1EEC / 0x803A1EF0 | `_savegpr_23` / `_25` / `_26` / `_27` (r11 = frame) | `stw r23,-0x24(r11)` … |
| 0x803A1F2C / 0x803A1F34 / 0x803A1F38 / 0x803A1F3C | `_restgpr_23` / `_25` / `_26` / `_27` | `lwz r23,-0x24(r11)` … |
| 0x803731CC | OSReport (variadic, `crclr cr1eq` at call sites) | |
| 0x800054F4 | memmove-style copy (dst r3, src r4, n r5). It copies backwards when src < dst. | |
| 0x8037611C / 0x80376130 | OSDisableInterrupts / OSRestoreInterrupts | used in pairs |
| 0x803ADC08 strlen, 0x803ADB0C strncpy, 0x803AD710 strstr | MSL (calling pattern) | **UNCERTAIN**: names inferred only from how they are called |
| 0x8047A6E8 | `_rtGetFileNameFromDeviceName(name)` | section 5 |
| 0x8047A76C | `_rtFSBuildFileName(buf, len, drive, path, sep)` | section 5 |
| 0x8047AA18 | `rtFSTriggerFileOustandingOpCallback(file)` | section 5 |
| 0x8047ACC4 | `RtFSManagerGetFileSystemFromName(fsName)` | section 5 |
| 0x8047A5E8 | `_rtFSInit(fs, maxNbFiles, fsName, deviceName)` | section 5 |
| 0x8047A840 | `_rtFSFileExists(fs, name)` | section 5 |
| 0x8047A898 | `_rtFSIsEOF(file)` | section 5 |

String table at 0x8056EE40 + 0x34. The read function uses r31 = 0x8056EE40 for both the jump table and these strings:
- +0x34 = 0x8056EE74: "Unaligned async read failed\n"
- +0x54 = 0x8056EE94: "Buffer offset not multiple of 32 %p\n"
- +0x7C = 0x8056EEBC: "File pointer offset not multiple of 32 %d\n"
- +0xA8 = 0x8056EEE8: "Num bytes to read not multiple of 32 %d\n"
- +0xD4 = 0x8056EF14: "Error: Unaligned async read failed\n"

r13+0x2A44 = 0x805EEF44 is a NUL byte in the DOL, so the device name is "". No store to it was found.

## 1. The game's init call (0x80051238)

```
fs = RtGCNFSystemInit(r3=4, r4=0x80574DA0 /*buffer pool, 4*0x800*/, r5=0x800,
                      r6=r2-0x79A0=0x805F2DE0 "dvd:" /*device name*/, r7=r2-0x7998=0x805F2DE8 "dvd" /*fs name*/)
if (!fs) return 0
0x8032E254()                                  // UNCERTAIN purpose; see DISC_SYSTEM_MAP.md
*(fs+0x5C) = 0x8032D3D4                       // 0x8005127C, the drive-error callback; fs+0x60 is NOT written
if (!RtFSManagerRegister(fs) /*0x8047ABFC*/) return 0
RtFSManagerSetDefaultFileSystem(fs)           // 0x8047B1B8: *(r13+0x60F0) = fs
0x8032D150(); return 1
```

Before this, the game calls `RtFSManagerOpen(-1)` at 0x80051784. `RtFSManagerClose` is called at 0x800518E8 and
0x80051A6C.

## 2. Per-function specification (0x8047B818..0x8047C770)

### 0x8047B818 fsGetObject(r3=fs, r4=index) -> RtFile* (device slot fs+0x14)

```
if (index <u *(fs+0x04)) return *(fs+0x58) + index*0x90
return NULL
```

### 0x8047B83C fsGetDeviceName(r3=fs) -> char* (slot fs+0x1C)

Returns r13+0x2A44 (0x805EEF44, the string ""). It reads no memory.

### 0x8047B844 fsClose(r3=fs) (slot fs+0x18)

`ENG+0x10C(*(fs+0x58))`: frees the file array only. The manager frees fs itself.

### 0x8047B874 open(r3=fs, r4=file, r5=filename, r6=access flags) -> RtFileSystemError (slot fs+0x28)

Frame 0x120. The path buffer is at sp+8, 0x100 bytes.

```
if (flags & 0xE)  /*WRITE|CREATE|APPEND*/        return 4   // RTFS_ERROR_WRITEPROTECTED
p   = _rtGetFileNameFromDeviceName(filename)     // 0x8047A6E8: text after the first ':'
dev = (*(fs+0x1C))(fs)                           // -> 0x8047B83C -> ""
if (!_rtFSBuildFileName(sp+8, 0x100, dev, p, '/')) return 5 // 0x8047A76C; RTFS_ERROR_FILENAMETOOLONG
if (DVDOpen(sp+8, file+0x40) == 0 /*0x8037AAA0*/)   return 3 // RTFS_ERROR_FILENOTFOUND
file+0x20 = 2; file+0x24 = 1                     // status READY, error NOERROR
file+0x0C = 0; file+0x08 = 0                     // position = 0
file+0x80 = 0; file+0x84 = 0                     // cache invalid, cache offset 0
file+0x04 = *(file+0x74); file+0x00 = 0          // length = DVDFileInfo.length (zero-extended)
if (flags & 0x10) { file+0x18 = 1; file+0x20 = 3 }   // async: isAsync, status BUSY
else              { file+0x18 = 0; file+0x20 = 2 }
file+0x1C = 2                                    // priority
return 1                                         // RTFS_ERROR_NOERROR
```

It never touches G or calls ERRCB.

### 0x8047B988 close(r3=file) (slot fs+0x2C)

```
DVDClose(file+0x40)     // 0x8037AB68
file+0x20 = 1           // CLOSED
```

The declared return is void. r3 is left holding DVDClose's 1.

### 0x8047B9C0 read-complete callback(r3=result, r4=DVDCommandBlock*)

`*(r4+0x2C) = r3`: the block's userData, which is `file+0x6C`. This is the only callback the device passes to DVD. It
is formed at 0x8047B9F0/0x8047B9F8.

### 0x8047B9C8 submit(r3=file, r4=buf, r5=len, r6=fileOffset, r7=ignored) -> s32 (internal)

Frame 0x20 (saves r29-r31). r7 is overwritten before use.

```
alen = (len + 0x1F) & ~0x1F;  fs = *(file+0x38)
r3 = DVDReadAsyncPrio(file+0x40, buf, alen, fileOffset, 0x8047B9C0, *(file+0x1C))   // 0x8037AF04
if (*(file+0x18) != 0) {                         // async file
    if (r3 == 0) return 0
    file+0x20 = 3;  G = 1;  return alen
}
do {                                             // sync: poll the drive
    OSYieldThread()                              // 0x80378A08, every iteration, including the first
    st = DVDGetDriveStatus()                     // 0x8037D3DC
    switch (st+1 /*unsigned; >12 -> default*/) { see table T1 }
} while (st != 0)
r3 = *(file+0x6C); if (r3 < 0) r3 = 0           // result stored by the 0x8047B9C0 callback
file+0x20 = 2;  return r3
```

0x8047BC20 is dead: its `bne` reuses cr0 from `st != 0`, which is always false there.

**Table T1** (0x8056EE40, index = drive status + 1). These are the cases of the sync loop.

| idx | DVD state | target | effects (in order) |
|---|---|---|---|
| 0 | -1 FATAL | 0x8047BA6C | file+0x20=4; fs+0x08=7 (FATAL); G=2; ERRCB(2) |
| 1 | 0 END | 0x8047BBD4 | file+0x20=4; ERRCB(7). The loop then exits and the tail overwrites file+0x20 with 2. |
| 2 | 1 BUSY | 0x8047BB78 | if (G==1 && file+0x20!=4) { file+0x20=3; G=1 } else { fs+0x08=1 (NOTREADY); G=8 }. The `st!=1` arm (fs+0x08=2; G=1) is unreachable here. |
| 3 | 2 WAITING | 0x8047BBD4 | as idx 1 |
| 4 | 3 COVER_CLOSED | 0x8047BBFC (default) | ERRCB(7) |
| 5 | 4 NO_DISK | 0x8047BAA4 | file+0x20=4; fs+0x08=4 (NOMEDIA); G=3; ERRCB(3) |
| 6 | 5 COVER_OPEN | 0x8047BAD8 | file+0x20=4; fs+0x08=3 (DOOROPEN); G=4; ERRCB(4) |
| 7 | 6 WRONG_DISK | 0x8047BB0C | file+0x20=4; fs+0x08=5 (WRONGMEDIA); G=5; ERRCB(5) |
| 8 | 7 MOTOR_STOPPED | 0x8047BBD4 | as idx 1 |
| 9 | 8 PAUSING | 0x8047BBD4 | as idx 1 |
| 10 | 9 IGNORED | 0x8047BBFC | ERRCB(7) |
| 11 | 10 CANCELED | 0x8047BBFC | ERRCB(7) |
| 12 | 11 RETRY | 0x8047BB40 | file+0x20=4; fs+0x08=5 (sic: WRONGMEDIA, not RETRYING=6); G=6; ERRCB(6) |
| >12 | other | 0x8047BBFC | ERRCB(7) |

All cases join at 0x8047BC18, the loop test.

### 0x8047BC60 read(r3=file, r4=buf, r5=nBytes) -> u32 (slot fs+0x30)

Frame 0x40, `_savegpr_23`.
- Registers: r23 = file, r24 = dst, r25 = n, r26 = retries, r27 = pos&31, r28 = dst&31, r29 = temp, r30 = remaining.
- r31 = 0x8056EE40, the string base.

```
file+0x14 = file+0x0C; file+0x10 = file+0x08     // initialPos = position
if (file+0x18) file+0x28 = 5                     // outstandingFileOp = READ
if (n == 0) return 0
if ((s64)length < (s64)(position + n)) {         // 64-bit signed compare
    n = length.lo - position.lo                  // low words only
    if (file+0x18) n = (n + 0x1F) & ~0x1F
}
if (n == 0) return 0
remaining = n; r28 = dst & 31; r27 = position.lo & 31; retries = 0
if (file+0x18 != 0) {                            // ASYNC
    r29 = n % 32 (signed)
    if (r28 || r27 || r29) {
        OSReport(0x8056EE74)
        if (r28) OSReport(0x8056EE94, dst)
        if (r27) OSReport(0x8056EEBC, r4 = sp+8)  // passes the ADDRESS of a stack copy of the 64-bit
                                                  // position (lfd/stfd), not the value (source bug)
        if (r29) OSReport(0x8056EEE8, remaining)
        return 0
    }
    while (remaining > 0 && retries < 1) {
        rv = submit(file, dst, remaining, file+0x0C, file+0x18)
        if (rv > 0) { position += (s64)rv; remaining = 0; retries = 0; file+0x80 = 0; file+0x84 = 0 }
        else retries++
    }
    return (remaining > 0) ? 0 : n              // file+0x20 is left as submit set it (3)
}
// SYNC
while (remaining > 0 && retries < 1) {
    OSYieldThread()                              // 0x8047BE54, once per chunk
    if ((s64)position < 0) return 0
    if (!((s64)position < (s64)length)) return 0
    if (r28 == 0 && r27 == 0 && remaining >= 32) {            // aligned direct read
        rv = submit(file, dst, remaining & ~31, position.lo, file+0x18)
        if (rv > 0) { position += rv; remaining -= rv; dst += rv; retries = 0;
                      file+0x80 = 0; file+0x84 = 0 }
        else retries++
        continue
    }
    if (file+0x18 != 0) { OSReport(0x8056EF14); return 0 }     // unreachable in the sync branch
    if (file+0x80 == 1) {                                      // serve from the cache
        c = min(remaining, file+0x88 - file+0x84)
        memmove(dst, file+0x7C + file+0x84, c)                 // 0x800054F4
        file+0x84 += c;  r28 = (r28 + c) % 32
        if (file+0x84 >= file+0x88) { file+0x80 = 0; file+0x84 = 0 }
        position += c; r27 = (r27 + c) % 32; remaining -= c; dst += c   // retries is not reset
    } else {                                                   // fill the cache
        aEnd = (length.lo + 0x1F) & ~0x1F;  aPos = position.lo - r27
        fl = file+0x88;  if (aEnd < aPos + file+0x88) fl = aEnd - aPos      // signed compare
        rv = submit(file, file+0x7C, fl, aPos, file+0x18)
        if (rv > 0) { file+0x84 = r27; file+0x80 = 1; retries = 0 } else retries++
    }
}
file+0x20 = (file+0x18 == 0) ? 2 : 3
return n - remaining
```

Notes:
- file+0x88 (the cache capacity) is never reduced to the filled length. It is safe only because `remaining` was
  clamped to length-position.
- The function never touches G, ERRCB or fs+0x08 directly. submit does.

### 0x8047C0A0 write(r3=file, ...) -> 0 (slot fs+0x34)

`file+0x24 = 2` (RTFILE_ERROR_FILESYSTEM); returns 0.

### 0x8047C0B0 setPosition(r3=file, r4=offset s32, r5=method) -> RtInt64 in r3:r4 (slot fs+0x38)

```
switch (method) {
 case 1 /*BEGIN*/:   position = (s64)offset; file+0x80 = 0; file+0x84 = 0
 case 2 /*CURRENT*/: position += (s64)offset
                     if (file+0x80 != 0) { file+0x84 += offset
                         if (!(file+0x84 < file+0x88 && file+0x84 >= 0)) { file+0x80 = 0; file+0x84 = 0 } }
 case 3 /*END*/:     position = length + (s64)offset; file+0x80 = 0; file+0x84 = 0
 default (<1 or >=4): no change
}
file+0x20 = (file+0x18 == 0) ? 2 : 3
return (file+0x08 : file+0x0C)
```

### 0x8047C1A4 sync(r3=file, r4=block flag, ignored) -> RtFileStatus (slot fs+0x3C)

Frame 0x10. r30 = file, r31 = fs = *(file+0x38).

```
st = DVDGetDriveStatus()
switch (st+1 /*unsigned; >12 -> 0x8047C2D4*/) { table T2 }       // error cases then jump to TAIL
GENERIC (0x8047C2D4):
  if (!(G == 1 && file+0x20 != 4)) {
      if (st == 1) { fs+0x08 = 1; G = 8; goto TAIL }               // NOTREADY, skip block check
      else         { fs+0x08 = 2; G = 1 }                          // READY
  }
  bs = DVDGetCommandBlockStatus(file+0x40)       // 0x8037D390
  switch (bs+1 /*unsigned; >12 -> status 3*/) { table T3 }
TAIL (0x8047C37C):
  if (file+0x18 != 0 && file+0x20 == 2) {
      if (file+0x28 == 5) file+0x2C = file+0x0C - file+0x14       // outstandingSize = pos.lo - initialPos.lo
      rtFSTriggerFileOustandingOpCallback(file)                   // 0x8047AA18
  }
  return file+0x20
```

**Table T2** (0x8056EF6C). Each error case calls ERRCB with the value it just stored in G (it reloads G):

| idx | DVD state | target | effects |
|---|---|---|---|
| 0 | -1 FATAL | 0x8047C1E8 | file+0x20=4; G=2; ERRCB(G=2) |
| 1-4 | 0 END, 1 BUSY, 2 WAITING, 3 COVER_CLOSED | 0x8047C2D4 | GENERIC |
| 5 | 4 NO_DISK | 0x8047C218 | file+0x20=4; G=3; ERRCB(3) |
| 6 | 5 COVER_OPEN | 0x8047C248 | file+0x20=4; G=4; ERRCB(4) |
| 7 | 6 WRONG_DISK | 0x8047C274 | file+0x20=4; G=5; ERRCB(5) |
| 8-11 | 7 MOTOR_STOPPED, 8 PAUSING, 9 IGNORED, 10 CANCELED | 0x8047C2D4 | GENERIC |
| 12 | 11 RETRY | 0x8047C2A4 | file+0x20=4; G=6; ERRCB(6) |
| >12 | | 0x8047C2D4 | GENERIC |

Unlike T1, T2 never writes fs+0x08 in its error cases and never calls ERRCB(7).

**Table T3** (0x8056EF38, index = command-block state + 1):

| idx | block state | target | file+0x20 |
|---|---|---|---|
| 0 | -1 FATAL | 0x8047C374 | 3 (BUSY) |
| 1 | 0 END | 0x8047C350 | 2 (READY) |
| 2 | 1 BUSY | 0x8047C35C | 3 |
| 3-10 | 2..9 | 0x8047C374 | 3 |
| 11 | 10 CANCELED | 0x8047C368 | 2 |
| 12, >12 | 11, other | 0x8047C374 | 3 |

### 0x8047C3D4 abort(r3=file) -> RwBool (slot fs+0x40)

`return DVDCancelAsync(file+0x40, NULL)` (0x8037D4E8).

### 0x8047A898 isEOF(r3=file) -> RwBool (slot fs+0x44; RtFSManager generic, outside the range)

```
if (position == 0xFFFFFFFF_FFFFFFFF) return 1
return !((s64)position < (s64)length)
```

### 0x8047C3FC getStatus(r3=file) -> RtFileStatus (slot fs+0x48)

`if (file+0x20 != 1) sync(file, 0); return file+0x20`

### 0x8047C43C fExists(r3=fs, r4=name) -> RwBool (slot fs+0x4C)

Tail into `_rtFSFileExists(fs, name)` (0x8047A840):

```
f = _rtFSManagerFOpenOnFS(fs, name, 1 /*READ*/, 0, 0)    // 0x8047AE1C
if (f) { (*(fs+0x2C))(f) /*close*/; return 1 }
return 0
```

### 0x8047C45C RtGCNFSystemInit(r3=maxFiles, r4=bufferPool, r5=bufSize, r6=deviceName, r7=fsName) -> RtFileSystem*

Frame 0x30, `_savegpr_25`. r26 = n, r27 = pool, r28 = bufSize, r29 = deviceName, r30 = fsName.

```
if (RtFSManagerGetFileSystemFromName(fsName) /*0x8047ACC4*/) return NULL
fs = ENG+0x108(0x64, 0x401BE)                    // malloc, NOT zeroed; the result is not NULL-checked
fs+0x14=0x8047B818 fsGetObject   fs+0x18=0x8047B844 fsClose     fs+0x1C=0x8047B83C fsGetDeviceName
fs+0x28=0x8047B874 open          fs+0x2C=0x8047B988 close       fs+0x30=0x8047BC60 read
fs+0x34=0x8047C0A0 write         fs+0x38=0x8047C0B0 setPosition fs+0x3C=0x8047C1A4 sync
fs+0x40=0x8047C3D4 abort         fs+0x44=0x8047A898 isEOF       fs+0x48=0x8047C3FC getStatus
fs+0x4C=0x8047C43C fExists
fs+0x58 = ENG+0x114(n, 0x90, 0x401BE)            // calloc; not NULL-checked
for i in 0..n-1 (unrolled by 8 for n > 8):
    rec = fs+0x58 + i*0x90
    rec+0x38 = fs; rec+0x7C = pool + i*bufSize; rec+0x88 = bufSize; rec+0x80 = 0; rec+0x84 = 0
if (_rtFSInit(fs, n, fsName, deviceName) /*0x8047A5E8*/ != 1) {
    ENG+0x10C(fs+0x58); ENG+0x10C(fs); return NULL
}
return fs
```

All 13 table values were verified against the hi/lo pairs. For example, 0x80480000-0x3F60 = 0x8047C0A0 goes to +0x34
and -0x3F50 = 0x8047C0B0 goes to +0x38.

## 3. Field maps

### RtFile record (0x90 bytes; array at fs+0x58; calloc-zeroed)

W = writes, R = reads. Function names abbreviated: init = 0x8047C45C, open = 0x8047B874, close = 0x8047B988,
submit = 0x8047B9C8, cb = 0x8047B9C0, read = 0x8047BC60, write = 0x8047C0A0, seek = 0x8047C0B0, sync = 0x8047C1A4,
getSt = 0x8047C3FC. Manager-layer functions are named as in section 5.

| off | SDK field | writers | readers |
|---|---|---|---|
| +0x00/+0x04 | length (s64; hi is always 0) | open | read, seek(END), isEOF. **Game reads +0x04 directly** (section 5). |
| +0x08/+0x0C | position (s64) | open, read, seek | read, submit callers (lo as DVD offset), seek, sync (lo), isEOF, AA18 (lo, op SEEK) |
| +0x10/+0x14 | initialPos | read (entry) | sync (+0x14) |
| +0x18 | isAsync | _rtFSInit (= fs+0x10), open | submit, read, seek, sync, AA18, FOpenOnFS, RwFseek, RwFgets |
| +0x1C | priority (DVD prio) | open (2), FOpenOnFS (2) | submit (r8) |
| +0x20 | status (1 CLOSED, 2 READY, 3 BUSY, 4 ERROR, 5 OPENING) | open, close, submit, T1/T2/T3, read, seek, _rtFSInit (1), FOpenOnFS (5, or 1 on fail), RwFclose (1), RwFseek/RwFgets (3 if async) | submit (BUSY case), sync, getSt, FOpenOnFS (free-slot scan), AA18 (cb arg) |
| +0x24 | error (1 NOERROR, 2 FILESYSTEM) | open (1), write (2) | none in RW |
| +0x28 | outstandingFileOp (1 NONE, 2 OPEN, 4 SEEK, 5 READ) | read (5 if async), FOpenOnFS (2), RwFseek/RwFgets (4 if async), AA18 (1) | sync, AA18 |
| +0x2C | outstandingSize | sync | AA18 |
| +0x30 / +0x34 | outstandingCB / CBData | FOpenOnFS | AA18 |
| +0x38 | fileSystem | init | submit, sync, RwFclose/RwFread/RwFwrite/RwFseek/RwFgets/RwFeof |
| +0x3C | pad (8-byte alignment of the DVDFileInfo) | none | none |
| +0x40..+0x6F | DVDCommandBlock (DVDFileInfo.cb) | DVD SDK/native | DVD |
| +0x4C | cb.state | DVDOpen (native: 0), DVDReadAsyncPrio | DVDGetCommandBlockStatus (sync), DVDCancelAsync |
| +0x6C | cb.userData = read result | cb (0x8047B9C0) | submit (sync path) |
| +0x70 | startAddr (native: asset token) | DVDOpen | DVD |
| +0x74 | length | DVDOpen | open |
| +0x78 | DVDFileInfo.callback | DVD (native: 0, then the read cb) | DVD |
| +0x7C | cache buffer pointer (pool + i*bufSize) | init | read |
| +0x80 | cache valid (0/1) | init, open, read, seek | read, seek |
| +0x84 | cache read offset | init, open, read, seek | read, seek |
| +0x88 | cache capacity (bufSize, 0x800) | init | read, seek |
| +0x8C | pad | none | none |

### RtFileSystem object (0x64 bytes; malloc, not zeroed)

| off | SDK field | writers | readers |
|---|---|---|---|
| +0x00 | next (SLL) | RtFSManagerRegister | manager list walks |
| +0x04 | maxNbOpenFiles | _rtFSInit | fsGetObject, FOpenOnFS |
| +0x08 | status (1 NOTREADY, 2 READY, 3 DOOROPEN, 4 NOMEDIA, 5 WRONGMEDIA, 7 FATAL) | _rtFSInit (2), T1 cases, sync GENERIC | **nothing found** (the SDK's RtFileSystemGetStatus is not linked) |
| +0x0C | deviceName (heap copy of "dvd:") | _rtFSInit (0), SetDeviceName (0x8047A94C), RtFSManagerClose (0) | device lookup (0x8047B020, 0x8047B094, RwFopen, RwFexist), 0x8047B810 |
| +0x10 | defaultToAsync | _rtFSInit (0), 0x8047AA08 | _rtFSInit, FOpenOnFS, 0x8047AA10 |
| +0x14..+0x1C | fsGetObject, fsClose, fsGetDeviceName | init | _rtFSInit / FOpenOnFS (+0x14), RtFSManagerClose (+0x18), open (+0x1C) |
| +0x20 / +0x24 | register / unregister callbacks | _rtFSInit (0, 0) | 0x8047A8E4 (both NULL, so nothing is called) |
| +0x28..+0x4C | file function table (section 2) | init | RtFSManager (section 5) |
| +0x50..+0x54 | name[5] ("dvd") | _rtFSInit (strcpy) | RtFSManagerGetFileSystemFromName, 0x8047AD58, 0x8047A8E4 (cb arg) |
| +0x55..+0x57 | pad | none | none |
| +0x58 | files array | init | fsGetObject, fsClose, init error path |
| +0x5C | drive-error callback | **game** 0x8005127C (0x8032D3D4) | T1, T2 (ERRCB) |
| +0x60 | callback data | **never written**: uninitialised heap | T1, T2 (r4 of ERRCB; 0x8032D3D4 ignores r4) |

### G = 0x2A40(r13) (0x805EEF40, initial value 1)

Values: 1 OK, 2 fatal, 3 no disk, 4 cover open, 5 wrong disk, 6 retry, 8 drive busy.
- Stores: submit async success (1), T1 (2, 3, 4, 5, 6; BUSY case 1 or 8), T2 (2-6), sync GENERIC (1 or 8).
- Loads: T1 BUSY case (0x8047BB78), the T2 cases (they reload G as the ERRCB argument), GENERIC (0x8047C2D4).
- A whole-DOL scan found no other access, through r13+0x2A40 or through an absolute 0x805EEF40 formation.

## 4. Jump tables

T1 = 0x8056EE40 (submit), T2 = 0x8056EF6C (sync, drive status) and T3 = 0x8056EF38 (sync, block status). They are
listed with their cases in section 2. All three use `cmplwi (status+1), 0xC; bgt default`.

## 5. Outside the range: RtFSManager and the game

Address scan of the whole DOL:
- **No** lis/addi formation of any device function exists outside init 0x8047C45C. The one exception is 0x8047B9C0,
  formed inside submit.
- **No** data word holds a device or RtFS address.
- The device is reached only through the fs table, and only by these RtFSManager functions. All are direct-`bl`
  targets from the game.

| fn | identity | behaviour relevant to the device contract |
|---|---|---|
| 0x8047AAA0 | RtFSManagerOpen(max) | Writes the manager at 0x805E3B00: +4=max (game: -1), +8=0 (count), +0x14=0 (error cb), +0=0 (list), +0xC=1. Also *(r13+0x60F0)=0. No writer of +0x14 other than this was found, so manager error callbacks never fire. |
| 0x8047AAD0 | RtFSManagerClose | For each fs: fsClose (fs+0x18), free(fs+0xC), fs+0xC=0, unregister cb, free(fs). Then +0xC=0, +8=0. |
| 0x8047AB98 | _rtFSManagerIsNewFSRegistrable | max>0 && cnt+1>max → error cb(5), 0; else 1 |
| 0x8047ABFC | RtFSManagerRegister(fs) | Registrable check. If GetFileSystemFromName(fs+0x50) finds one → 0. Else register cb (fs+0x20), fs+0=head, head=fs, cnt++, return 1. |
| 0x8047ACC4 | RtFSManagerGetFileSystemFromName(name) | strcmp vs fs+0x50; not found → error cb(6), NULL |
| 0x8047AD58 | _rtFSManagerIsFSNameInUse(name) | found → error cb(7), 1; else (after error cb(6)) 0 |
| 0x8047A5E8 | _rtFSInit(fs, n, fsName, devName) | not registrable → 2; name in use → 6; strcpy(fs+0x50, fsName); fs+4=n; fs+0x10=0; for i<n: f=fsGetObject(fs,i), f+0x20=1, f+0x18=fs+0x10; fs+0xC=0; r=SetDeviceName(fs, devName); if r≠1 return r; fs+8=2; fs+0x20=fs+0x24=0; return 1 |
| 0x8047A94C | RtFileSystemSetDeviceName(fs, dev) | Device already used by another fs → 7. Else free the old fs+0xC, fs+0xC=malloc(strlen+1), strcpy, return 1. |
| 0x8047A6E8 | _rtGetFileNameFromDeviceName(s) | Returns s+i+1 for the first ':' (i < strlen), else s. |
| 0x8047A76C | _rtFSBuildFileName(buf,len,drive,path,sep) | strlen(drive)+strlen(path) > len-1 → NULL. Else strcpy(buf,drive), then append path (including its NUL) with '/' and '\\' replaced by sep. Returns buf. |
| 0x8047AA08 / 0x8047AA10 | RtFileSystemDefaultToAsync / IsDefaultedToAsync | fs+0x10 |
| 0x8047AA18 | rtFSTriggerFileOustandingOpCallback(f) | If f && f+0x18: if op (f+0x28)==1 or cb (f+0x30)==0 → f+0x28=1. Else: arg = (op==4 ? f+0x0C : f+0x2C); f+0x28=1; cb(f, arg, f+0x20, op, f+0x34). |
| 0x8047AE1C | _rtFSManagerFOpenOnFS(fs,name,acc,cb,data) | fs NULL → error cb(4), NULL. Under interrupts-off: first f=fsGetObject(i) with f+0x20==1 gets f+0x20=5. None → mgr+0x10=8, error cb(8), NULL. If (acc&0x10 \|\| f+0x18): f+0x28=2, f+0x30=cb, f+0x34=data. If fs+0x10: same, and acc\|=0x10. r=open(fs,f,name,acc); r≠1 → f+0x20=1, NULL. f+0x1C=2; return f. |
| 0x8047AFA0 | RtFSManagerFOpen | resolve fs from the name (0x8047B094), then FOpenOnFS |
| 0x8047B094 / 0x8047B020 | GetFileSystemFromFileName / _rtGetFileSystemFromDeviceName | The prefix up to and including ':' is compared (ENG+0xF0) with fs+0xC. Fallback: the default fs at r13+0x60F0. Else error cb(6). |
| 0x8047B1B8 / 0x8047B1C0 | Set/GetDefaultFileSystem | r13+0x60F0 |
| 0x8047B1C8 | RwFopen(name, mode) | Mode strings at r13+0x2A28.. "+", "b", "r", "w", "a": "r" → 1 (+'+' → \|2\|8); "w" → 6 (+'+' → \|1); "a" → 0xA (+'+' → \|1; +'b' → \|4). Then FOpenOnFS(fs,name,flags,0,0). Any write flag makes the GCN open return 4, so only plain "r"/"rb" succeeds. |
| 0x8047B3D8 | RwFclose(f) | Interrupts off; f+0x20=1; close(f) (fs+0x2C); interrupts on; return 0 |
| 0x8047B430 / 0x8047B478 | RwFread / RwFwrite(addr,size,count,f) | read/write(f, addr, size*count) / size (divwu) |
| 0x8047B4C0 | RwFseek(f,off,origin) | If f+0x18: f+0x28=4, f+0x20=3. origin 0/1/2 → setPosition(f,off,1/2/3), return 0; other → -1. |
| 0x8047B568 | RwFgets(buf,max,f) | n=read(f,buf,max-1); n==0 → NULL. Scans for '\n'; on '\n' it terminates after it and seeks CURRENT by -(n-(i+1)), with the async op/status preset as in RwFseek. '\r\n' → '\r' removed by memmove. Else buf[n]=0. Returns buf. |
| 0x8047B6A0 | RwFeof(f) | isEOF (fs+0x44) |
| 0x8047B6CC | RwFexist(name) | resolve fs (as 0x8047B094); fExists(fs,name) (fs+0x4C) |
| 0x8047B810 | RtFileSystemGetDeviceName(fs) | fs+0xC |

Slot usage:
- No RW code calls sync (+0x3C), abort (+0x40) or getStatus (+0x48) through the table. Only getStatus →
  sync is a direct bl.
- A scan found about 70 game sites with the shape `lwz r12,0x38(r3); lwz r12,0x3C/0x40/0x48/0x5C(r12); mtctr`.
  These are very likely C++ virtual calls with the vptr at +0x38, not RtFile→fileSystem slots, but this is
  **UNCERTAIN**. None was traced to an RtFile pointer.

Game callers (direct bl):
- RwFopen: 0x800444FC, 0x800514D8 (wrapper 0x800514CC), 0x80306BD4, 0x80363AE4, 0x80363B68, 0x80474A5C,
  0x80482494/A8/BC.
- RwFclose: 0x80044544, 0x80044E84, 0x80045134, 0x8004584C, 0x80306C24, 0x80363B08, 0x80363B78, 0x80474B58,
  0x804822EC.
- RwFread: 0x8004453C, 0x80045110, 0x800451BC, 0x800451E4, 0x80045218, 0x80306C1C, 0x80363B00, 0x80481DF8.
- RwFwrite: 0x80481F7C. RwFseek: 0x804821A4. RwFeof: 0x80481E0C, 0x804821B4.
- RwFgets: 0x804725A4..0x80474AA0 (11 sites).
- RwFexist: 0x80044EB4, 0x8047262C, 0x80489A70/B28/B5C.
- RtFSManagerFOpen: 0x8004547C (READ, cb 0).
- Default/async get/set: 0x800444D0-E8, 0x800445DC, 0x80306B88-BA0, 0x80306C38. Each saves the flag, sets 0 and
  restores the saved value.

Consequences:
- No file is ever opened with 0x10, and fs+0x10 stays 0, so **every file is sync** (file+0x18 = 0).
- The game reads `RtFile+0x04` (length.lo) directly after opening: 0x80044510, 0x80044E80, 0x800450F4/0x80045100/
  0x80045114, 0x80045494, 0x80306BE0.
- 0x80045150 reads +0x04 of an r27 that is very likely the RtFile (**UNCERTAIN**).
- No direct game read of file+0x20 or of the position was found, but this is **UNCERTAIN**: not exhaustively
  traced.

## 6. Reduced effective behaviour under the native DVD layer

Native DVD behaviour:
- DVDGetDriveStatus is always 0.
- DVDOpen sets cb.state=0.
- DVDReadAsyncPrio copies, sets the state to END (FATAL and result -1 if the asset copy fails), calls the callback,
  then r3=1.
- DVDGetCommandBlockStatus returns the state (3→1).
- DVDCancelAsync with cb NULL just returns 1.

All files are sync (section 5).

- **init**: unchanged, as in section 2. fs+0x5C/0x60 are left uninitialised until the game writes +0x5C.
- **fsGetObject / fsClose / fsGetDeviceName / write / setPosition / isEOF**: unchanged. They are pure memory logic.
- **open**: flags&0xE → 4. Path = name after ':', with '\\' and '/' becoming '/'. Over 255 characters → 5. Missing
  or a directory → 3. Else it fills length, position=0, cache invalid, status 2, error 1, isAsync 0, priority 2 and
  returns 1. DVDOpen also writes fi+0x70/+0x74/+0x78/+0x4C.
- **close**: status := 1.
- **submit (sync)**: exactly these observable steps:
  1. data copied into buf (alen = len rounded up to 32; at most 31 bytes past EOF, which the native accepts).
  2. file+0x6C := alen (or -1); file+0x78 := 0x8047B9C0; DVD block fields written.
  3. **one OSYieldThread**.
  4. T1 idx 1: file+0x20 := 4, then **ERRCB(7)**, i.e. 0x8032D3D4(7, garbage), which stores 0 to r13+0x5760
     and returns 1.
  5. file+0x20 := 2; return alen (0 on a failed copy).
  6. G and fs+0x08 are not touched.
- **read (sync)**:
  - initialPos := position; n clamped to length-position (0 → return 0).
  - Then per chunk: OSYieldThread, then either a 32-aligned direct submit or a cache fill/serve through the
    0x800-byte buffer at +0x7C. Each submit adds one more yield and one ERRCB(7).
  - position advances by bytes delivered; status := 2; returns n.
  - A failed copy stops after one failed submit and returns the partial count.
  - The async branch (OSReport strings, file+0x28 := 5) is dead.
- **sync** (reached only if something calls slot +0x3C / getStatus; none found in RW):
  - status 0 → GENERIC. If G≠1 or status==4: fs+0x08 := 2, G := 1.
  - DVDGetCommandBlockStatus → 0 (END) → file+0x20 := 2 (a failed read leaves -1 → 3).
  - No ERRCB. The TAIL is skipped (not async). Returns file+0x20.
- **getStatus**: sync() unless the file is CLOSED; returns status.
- **abort**: returns 1, no other effect.
- **fExists**: opens a free record via FOpenOnFS (DVDOpen hit/miss) and closes it at once. Returns 1/0. As with
  open, it returns 0 when every slot of the 4 is busy (mgr+0x10 := 8).
- **G** is never changed in practice: it stays at its DOL value 1, since the only stores reachable under status 0
  are the GENERIC `G := 1` and the async `G := 1`.
- **fs+0x08** stays 2 (set by _rtFSInit; GENERIC also writes 2).
- T1/T2 error cases (codes 2-6, status 4, G 2-6, fs+0x08 3/4/5/7, ERRCB 2-6) and the BUSY/G=8 paths are
  unreachable.

The observable residue a replacement must keep:
- the RtFile/RtFileSystem fields the manager and game read (length, position, status, cache);
- the OSYieldThread calls per chunk and per submit (thread scheduling points; **UNCERTAIN** whether any thread's
  order depends on them);
- the ERRCB(7) call per submit (only effect: r13+0x5760 := 0, which already holds 0 per DISC_SYSTEM_MAP.md);
- the native DVD block/fileinfo writes.

Correction to DISC_SYSTEM_MAP.md §2:
- the read slot is 0x8047BC60;
- 0x8047B9C8 is the internal submit and owns table 0x8056EE40;
- 0x8047C1A4 (sync) owns tables 0x8056EF6C and 0x8056EF38;
- 0x8047C1E8 is a case label, not a function;
- 0x8047C0A0 is write and 0x8047C0B0 is setPosition.
