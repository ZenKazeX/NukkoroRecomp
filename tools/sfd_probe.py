#!/usr/bin/env python3
"""Empirical parser for Sofdec .sfd containers (CRI MPEG program streams).

Static byte-level analysis only. No decoding, no ffmpeg, no game execution.

Parses, from the raw bytes:
  - MPEG-1/2 program-stream pack (0x000001BA) and system (0x000001BB) headers
  - every PES packet and its stream id, PTS/DTS, payload extent
  - MPEG video sequence header (0x000001B3) and, if present, sequence extension
    (0x000001B5 with extension id 1 -- its presence is what makes a stream MPEG-2)
  - picture start codes (0x00000100) so the exact coded frame count is a count,
    not an estimate
  - CRI private_stream_2 (0x000001BF) payloads: the "SofdecStream" file header
    and the "CRITAGS" text metadata block
  - the CRI ADX header (0x8000 magic, "(c)CRI" signature) carried at the head of
    the audio elementary stream
  - pack size / interleave statistics for streaming-read characterisation

Usage:  python sfd_probe.py <file.sfd> [...]      -> JSON on stdout
        python sfd_probe.py --table <file.sfd>... -> human table on stdout
"""
import os, sys, struct, json, collections

# ISO/IEC 11172-2 Table 2-3 -- MPEG-1 aspect_ratio_information is a *pel* aspect
# ratio (height/width of a pel), NOT a display aspect ratio.  There is no 16:9
# display flag in MPEG-1 video at all; that only exists in MPEG-2 Table 6-3.
ASPECT_MPEG1 = {
    0: "forbidden",
    1: "1.0000  square pel",
    2: "0.6735",
    3: "0.7031  (16:9 625-line)",
    4: "0.7615",
    5: "0.8055",
    6: "0.8437  (16:9 525-line)",
    7: "0.8935",
    8: "0.9157  (4:3 625-line / PAL)",
    9: "0.9815",
    10: "1.0255",
    11: "1.0695",
    12: "1.0950  (4:3 525-line / NTSC)",
    13: "1.1575",
    14: "1.2015",
    15: "reserved",
}
# ISO/IEC 13818-2 Table 6-3 -- MPEG-2 aspect_ratio_information IS a display AR.
ASPECT_MPEG2 = {
    0: "forbidden", 1: "1:1 square pixels", 2: "4:3", 3: "16:9", 4: "2.21:1",
}
FRAMERATE = {
    0: None, 1: 24000 / 1001, 2: 24.0, 3: 25.0, 4: 30000 / 1001, 5: 30.0,
    6: 50.0, 7: 60000 / 1001, 8: 60.0,
}
FRAMERATE_NAME = {
    0: "forbidden", 1: "23.976 (24000/1001)", 2: "24", 3: "25",
    4: "29.97 (30000/1001)", 5: "30", 6: "50", 7: "59.94 (60000/1001)", 8: "60",
}
PICTURE_TYPE = {1: "I", 2: "P", 3: "B", 4: "D"}
ADX_ENCODING = {
    0x02: "ADX fixed-coefficient",
    0x03: "ADX standard (linear prediction)",
    0x04: "ADX exponential scale",
    0x10: "AHX (MPEG-2 audio)",
    0x11: "AHX",
}


def stream_name(sid):
    if sid == 0xBA: return "pack_start"
    if sid == 0xBB: return "system_header"
    if sid == 0xBC: return "program_stream_map"
    if sid == 0xBD: return "private_stream_1"
    if sid == 0xBE: return "padding_stream"
    if sid == 0xBF: return "private_stream_2"
    if sid == 0xB9: return "program_end"
    if 0xC0 <= sid <= 0xDF: return "audio_%d" % (sid - 0xC0)
    if 0xE0 <= sid <= 0xEF: return "video_%d" % (sid - 0xE0)
    return "0x%02X" % sid


class Bits:
    __slots__ = ("d", "bit")
    def __init__(self, data, pos=0):
        self.d = data; self.bit = pos * 8
    def u(self, n):
        v = 0
        d = self.d
        b = self.bit
        for _ in range(n):
            v = (v << 1) | ((d[b >> 3] >> (7 - (b & 7))) & 1)
            b += 1
        self.bit = b
        return v


def parse_sequence_header(buf, off):
    """buf[off:] begins immediately after the 0x000001B3 start code."""
    b = Bits(buf, off)
    w = b.u(12); h = b.u(12); ar = b.u(4); fr = b.u(4)
    br = b.u(18)                      # bit_rate, units of 400 bit/s; all-ones = VBR
    marker = b.u(1)
    vbv = b.u(10)                     # units of 16384 bits
    constrained = b.u(1)
    load_intra = b.u(1)               # bit 63 after the start code
    if load_intra:
        b = Bits(buf, 0); b.bit = off * 8 + 63 + 1 + 512
        load_nonintra = b.u(1)
    else:
        load_nonintra = b.u(1)
    return dict(width=w, height=h, aspect_code=ar, framerate_code=fr,
                bitrate_400=br,
                bitrate_bps=(None if br == 0x3FFFF else br * 400),
                bitrate_is_all_ones=(br == 0x3FFFF),
                marker_ok=(marker == 1), vbv_buffer_size=vbv,
                vbv_bytes=vbv * 2048,
                constrained_parameters=constrained,
                load_intra_q=load_intra, load_nonintra_q=load_nonintra)


def parse_sequence_extension(buf, off):
    b = Bits(buf, off)
    if b.u(4) != 1:
        return None
    profile_level = b.u(8)
    progressive = b.u(1); chroma = b.u(2)
    hsize_ext = b.u(2); vsize_ext = b.u(2)
    bitrate_ext = b.u(12); b.u(1)
    vbv_ext = b.u(8); low_delay = b.u(1)
    fr_ext_n = b.u(2); fr_ext_d = b.u(5)
    return dict(profile_and_level=profile_level, progressive_sequence=progressive,
                chroma_format=chroma, horizontal_size_ext=hsize_ext,
                vertical_size_ext=vsize_ext, bit_rate_ext=bitrate_ext,
                vbv_buffer_size_ext=vbv_ext, low_delay=low_delay,
                frame_rate_ext_n=fr_ext_n, frame_rate_ext_d=fr_ext_d)


def parse_adx_header(buf, off):
    if len(buf) - off < 0x20: return None
    if buf[off] != 0x80 or buf[off + 1] != 0x00: return None
    co = struct.unpack_from(">H", buf, off + 2)[0]
    enc = buf[off + 4]
    blk = buf[off + 5]
    depth = buf[off + 6]
    ch = buf[off + 7]
    rate = struct.unpack_from(">I", buf, off + 8)[0]
    total = struct.unpack_from(">I", buf, off + 12)[0]
    hp = struct.unpack_from(">H", buf, off + 16)[0]
    ver = buf[off + 18]
    flg = buf[off + 19]
    sig_at = off + co - 2
    sig = bytes(buf[sig_at:sig_at + 6]) if 0 <= sig_at and sig_at + 6 <= len(buf) else b""
    bytes_per_sec = None
    if ch and blk and rate:
        bytes_per_sec = (rate / 32.0) * blk * ch   # 32 samples per 18-byte block
    return dict(magic="0x8000", copyright_offset=co,
                encoding_type="0x%02X" % enc,
                encoding_name=ADX_ENCODING.get(enc, "unknown"),
                block_size=blk, sample_bitdepth=depth, channels=ch,
                sample_rate=rate, total_samples=total,
                duration_s=(total / rate) if rate else None,
                highpass_frequency=hp, version="0x%02X" % ver,
                flags="0x%02X" % flg,
                signature=sig.decode("latin1", "replace"),
                signature_ok=(sig == b"(c)CRI"),
                nominal_bytes_per_second=bytes_per_sec,
                header_offset_in_file=off)


def cstrings(payload, minlen=2):
    out = []
    cur = bytearray()
    for c in payload:
        if 0x20 <= c < 0x7F:
            cur.append(c)
        else:
            if len(cur) >= minlen:
                out.append(cur.decode("ascii"))
            cur = bytearray()
    if len(cur) >= minlen:
        out.append(cur.decode("ascii"))
    return out


def probe(path):
    size = os.path.getsize(path)
    f = open(path, "rb")
    out = dict(path=path, file_name=os.path.basename(path), size=size)

    packs = 0
    pack_offsets = []
    pes_count = collections.Counter()
    pes_bytes = collections.Counter()
    pes_sizes = collections.defaultdict(collections.Counter)
    scr_first = scr_last = None
    muxrate = None
    pack_hdr_bytes = None
    mpeg_ver = None
    system_header = None
    pack_sizes = collections.Counter()
    interleave_seq = []
    private2_blobs = []
    private1_subids = collections.Counter()
    resync_runs = []          # (offset, length) of non-startcode runs

    BUF = 1 << 22
    buf = b""
    buf_base = 0

    def at(p, n):
        nonlocal buf, buf_base
        if not (p >= buf_base and p + n <= buf_base + len(buf)):
            f.seek(p)
            buf = f.read(max(BUF, n))
            buf_base = p
        i = p - buf_base
        return buf[i:i + n]

    video_stream_ids = set()
    audio_stream_ids = set()
    video_payload_bytes = 0
    audio_payload_bytes = 0
    pts_v0 = pts_vN = pts_a0 = pts_aN = None
    dts_v0 = dts_vN = None
    adx = None
    audio_es_head = bytearray()
    ves = bytearray()
    audio_pack_indices = []
    pack_index = 0
    program_end_at = None

    pos = 0
    resync_start = None
    while pos + 4 <= size:
        sc = at(pos, 4)
        if len(sc) < 4: break
        if sc[0:3] != b"\x00\x00\x01":
            if resync_start is None: resync_start = pos
            pos += 1
            continue
        if resync_start is not None:
            if len(resync_runs) < 16:
                resync_runs.append((hex(resync_start), pos - resync_start))
            resync_start = None
        sid = sc[3]

        if sid == 0xBA:
            packs += 1
            pack_index += 1
            if len(pack_offsets) < 8: pack_offsets.append(pos)
            b5 = at(pos + 4, 10)
            if len(b5) < 10: break
            if (b5[0] & 0xC0) == 0x40:
                if mpeg_ver is None: mpeg_ver = 2
                bb = Bits(b5, 0); bb.u(2)
                s1 = bb.u(3); bb.u(1); s2 = bb.u(15); bb.u(1); s3 = bb.u(15); bb.u(1)
                bb.u(9); bb.u(1)
                scr = (s1 << 30) | (s2 << 15) | s3
                muxrate = bb.u(22); bb.u(2)
                hdrlen = 14 + (at(pos + 13, 1)[0] & 0x07)
            elif (b5[0] & 0xF0) == 0x20:
                if mpeg_ver is None: mpeg_ver = 1
                bb = Bits(b5, 0); bb.u(4)
                s1 = bb.u(3); bb.u(1); s2 = bb.u(15); bb.u(1); s3 = bb.u(15); bb.u(1)
                scr = (s1 << 30) | (s2 << 15) | s3
                bb.u(1); muxrate = bb.u(22)
                hdrlen = 12
            else:
                pos += 4; continue
            pack_hdr_bytes = hdrlen
            if scr_first is None: scr_first = scr
            scr_last = scr
            if packs > 1:
                pack_sizes[pos - pack_offsets[-1] if len(pack_offsets) < 8 else 0] += 0
            pos += hdrlen
            continue

        if sid == 0xB9:
            program_end_at = pos
            pos += 4
            continue

        lnb = at(pos + 4, 2)
        if len(lnb) < 2: break
        ln = struct.unpack(">H", lnb)[0]

        if sid == 0xBB:
            sh = at(pos + 6, ln)
            if system_header is None and len(sh) == ln:
                bb = Bits(sh, 0); bb.u(1)
                rate_bound = bb.u(22); bb.u(1)
                audio_bound = bb.u(6); fixed = bb.u(1); csps = bb.u(1)
                sal = bb.u(1); svl = bb.u(1); bb.u(1)
                video_bound = bb.u(5); prr = bb.u(1)
                streams = []
                i = 6
                while i + 2 < ln and (sh[i] & 0x80):
                    s_id = sh[i]
                    pbb = Bits(sh, i + 1); pbb.u(2)
                    scale = pbb.u(1); bound = pbb.u(13)
                    streams.append(dict(stream_id="0x%02X" % s_id, name=stream_name(s_id),
                                        buffer_bound_scale=scale, buffer_size_bound=bound,
                                        buffer_bytes=bound * (1024 if scale else 128)))
                    i += 3
                system_header = dict(header_length=ln, rate_bound=rate_bound,
                                     rate_bound_bytes_per_s=rate_bound * 50,
                                     audio_bound=audio_bound, fixed_flag=fixed,
                                     csps_flag=csps, system_audio_lock=sal,
                                     system_video_lock=svl, video_bound=video_bound,
                                     packet_rate_restriction=prr, declared_streams=streams)
            pos += 6 + ln
            continue

        pes_hdr_len = 0
        pts = dts = None
        if sid not in (0xBE, 0xBF, 0xBC):
            hb = at(pos + 6, 32)
            if len(hb) >= 3 and (hb[0] & 0xC0) == 0x80:
                pes_hdr_len = 3 + hb[2]
                if (hb[1] & 0xC0) in (0x80, 0xC0) and len(hb) >= 8:
                    p = hb[3:8]
                    pts = (((p[0] >> 1) & 7) << 30) | (p[1] << 22) | \
                          (((p[2] >> 1) & 0x7F) << 15) | (p[3] << 7) | ((p[4] >> 1) & 0x7F)
            else:
                j = 0
                while j < len(hb) and hb[j] == 0xFF: j += 1
                if j + 1 < len(hb) and (hb[j] & 0xC0) == 0x40: j += 2
                if j + 4 < len(hb) and (hb[j] & 0xF0) == 0x20:
                    p = hb[j:j + 5]
                    pts = (((p[0] >> 1) & 7) << 30) | (p[1] << 22) | \
                          (((p[2] >> 1) & 0x7F) << 15) | (p[3] << 7) | ((p[4] >> 1) & 0x7F)
                    j += 5
                elif j + 9 < len(hb) and (hb[j] & 0xF0) == 0x30:
                    p = hb[j:j + 5]
                    pts = (((p[0] >> 1) & 7) << 30) | (p[1] << 22) | \
                          (((p[2] >> 1) & 0x7F) << 15) | (p[3] << 7) | ((p[4] >> 1) & 0x7F)
                    q = hb[j + 5:j + 10]
                    dts = (((q[0] >> 1) & 7) << 30) | (q[1] << 22) | \
                          (((q[2] >> 1) & 0x7F) << 15) | (q[3] << 7) | ((q[4] >> 1) & 0x7F)
                    j += 10
                elif j < len(hb) and hb[j] == 0x0F:
                    j += 1
                pes_hdr_len = j

        payload_off = pos + 6 + pes_hdr_len
        payload_len = ln - pes_hdr_len
        nm = stream_name(sid)
        pes_count[nm] += 1
        pes_bytes[nm] += max(0, payload_len)
        pes_sizes[nm][6 + ln] += 1
        if len(interleave_seq) < 200:
            interleave_seq.append((nm, hex(pos), 6 + ln))

        if sid == 0xBF and payload_len > 0 and len(private2_blobs) < 4:
            pl = at(payload_off, payload_len)
            private2_blobs.append(dict(offset=hex(payload_off), length=payload_len,
                                       strings=cstrings(pl, 3),
                                       head_hex=pl[:48].hex()))
        elif sid == 0xBD and payload_len > 0:
            pd = at(payload_off, 1)
            if pd: private1_subids[pd[0]] += 1
            audio_payload_bytes += payload_len
        elif 0xC0 <= sid <= 0xDF:
            audio_stream_ids.add(sid)
            audio_payload_bytes += payload_len
            audio_pack_indices.append(pack_index)
            if len(audio_es_head) < 4096 and payload_len > 0:
                audio_es_head += at(payload_off, min(payload_len, 4096))
            if pts is not None:
                if pts_a0 is None: pts_a0 = pts
                pts_aN = pts
        elif 0xE0 <= sid <= 0xEF:
            video_stream_ids.add(sid)
            video_payload_bytes += payload_len
            if pts is not None:
                if pts_v0 is None: pts_v0 = pts
                pts_vN = pts
            if dts is not None:
                if dts_v0 is None: dts_v0 = dts
                dts_vN = dts
            if payload_len > 0:
                ves += at(payload_off, payload_len)

        pos += 6 + ln

    if resync_start is not None and len(resync_runs) < 16:
        resync_runs.append((hex(resync_start), size - resync_start))

    adx = parse_adx_header(bytes(audio_es_head), 0)

    # ---- video elementary stream ---------------------------------------
    v = bytes(ves)
    n = len(v)
    seq_headers = []
    seq_ext = None
    gop_count = 0
    frames = 0
    picture_types = collections.Counter()
    seq_end = 0
    i = 0
    while True:
        j = v.find(b"\x00\x00\x01", i)
        if j < 0 or j + 4 > n: break
        code = v[j + 3]
        if code == 0xB3:
            if len(seq_headers) < 3:
                try:
                    sh = parse_sequence_header(v, j + 4)
                    sh["offset_in_ves"] = j
                    seq_headers.append(sh)
                except Exception:
                    pass
            else:
                seq_headers_extra = True
        elif code == 0xB5 and seq_ext is None:
            try:
                e = parse_sequence_extension(v, j + 4)
                if e: seq_ext = e
            except Exception:
                pass
        elif code == 0xB8:
            gop_count += 1
        elif code == 0xB7:
            seq_end += 1
        elif code == 0x00:
            frames += 1
            if j + 7 < n:
                b = Bits(v, j + 4); b.u(10)
                picture_types[PICTURE_TYPE.get(b.u(3), "?")] += 1
        i = j + 3

    seq_hdr_count = v.count(b"\x00\x00\x01\xb3")

    # audio pack interleave spacing
    gaps = collections.Counter()
    for a, b2 in zip(audio_pack_indices, audio_pack_indices[1:]):
        gaps[b2 - a] += 1

    sh0 = seq_headers[0] if seq_headers else None
    fr = FRAMERATE.get(sh0["framerate_code"]) if sh0 else None
    duration_frames = (frames / fr) if (fr and frames) else None

    out.update(
        container=dict(
            pack_start_codes=packs,
            pack_header_bytes=pack_hdr_bytes,
            pack_stride_bytes=(2048 if packs and size // packs == 2048 else size // max(packs, 1)),
            sectors_2048=size / 2048.0,
            mpeg_system_version=mpeg_ver,
            scr_first=scr_first, scr_last=scr_last,
            scr_span_at_90khz_s=((scr_last - scr_first) / 90000.0)
                                if scr_first is not None else None,
            mux_rate_units_50Bps=muxrate,
            mux_rate_bytes_per_s=(muxrate * 50) if muxrate else None,
            mux_rate_bits_per_s=(muxrate * 400) if muxrate else None,
            program_end_code_at=hex(program_end_at) if program_end_at else None,
            trailing_nonstartcode_runs=resync_runs[:4],
            system_header=system_header,
        ),
        pes=dict(count=dict(pes_count), payload_bytes=dict(pes_bytes),
                 packet_size_hist={k: dict(sorted(c.items(), key=lambda kv: -kv[1])[:4])
                                   for k, c in pes_sizes.items()},
                 video_stream_ids=["0x%02X" % s for s in sorted(video_stream_ids)],
                 audio_stream_ids=["0x%02X" % s for s in sorted(audio_stream_ids)],
                 private1_subids={"0x%02X" % k: c for k, c in private1_subids.items()},
                 audio_pack_gap_hist=dict(sorted(gaps.items(), key=lambda kv: -kv[1])[:5]),
                 interleave_first24=interleave_seq[:24]),
        video=dict(
            elementary_bytes=len(v),
            sequence_header_count=seq_hdr_count,
            sequence_headers=seq_headers,
            sequence_extension=seq_ext,
            codec=("MPEG-2 Video (ISO/IEC 13818-2)" if seq_ext
                   else "MPEG-1 Video (ISO/IEC 11172-2)"),
            width=sh0["width"] if sh0 else None,
            height=sh0["height"] if sh0 else None,
            aspect_code=sh0["aspect_code"] if sh0 else None,
            aspect_meaning=(ASPECT_MPEG2 if seq_ext else ASPECT_MPEG1).get(
                sh0["aspect_code"]) if sh0 else None,
            framerate_code=sh0["framerate_code"] if sh0 else None,
            framerate=FRAMERATE_NAME.get(sh0["framerate_code"]) if sh0 else None,
            frames=frames, picture_types=dict(picture_types),
            gop_count=gop_count, sequence_end_codes=seq_end,
            duration_from_frames_s=duration_frames,
            video_bitrate_bps=(video_payload_bytes * 8 / duration_frames)
                              if duration_frames else None,
            pts_first=pts_v0, pts_last=pts_vN,
            pts_span_s=((pts_vN - pts_v0) / 90000.0) if pts_v0 is not None else None,
            dts_first=dts_v0, dts_last=dts_vN,
            payload_bytes=video_payload_bytes,
        ),
        audio=dict(
            adx_header=adx,
            payload_bytes=audio_payload_bytes,
            pts_first=pts_a0, pts_last=pts_aN,
            pts_span_s=((pts_aN - pts_a0) / 90000.0) if pts_a0 is not None else None,
            measured_bytes_per_second=(audio_payload_bytes / duration_frames)
                                      if duration_frames else None,
            head_hex=bytes(audio_es_head[:40]).hex(),
        ),
        overall=dict(
            file_bitrate_bps=(size * 8 / duration_frames) if duration_frames else None,
            file_bytes_per_second=(size / duration_frames) if duration_frames else None,
        ),
        cri=dict(private_stream_2_blobs=private2_blobs),
    )
    f.close()
    return out


def table(results):
    hdr = ("file", "MB", "codec", "WxH", "aspect", "fps", "frames", "dur s",
           "vid kbps", "aud", "kHz", "ch", "tot kbps")
    rows = []
    for r in results:
        v = r["video"]; a = r["audio"]["adx_header"] or {}
        rows.append((
            r["file_name"],
            "%.1f" % (r["size"] / 1048576.0),
            "MPEG-2" if v["sequence_extension"] else "MPEG-1",
            "%dx%d" % (v["width"], v["height"]),
            "code %d" % v["aspect_code"],
            v["framerate"],
            str(v["frames"]),
            "%.2f" % (v["duration_from_frames_s"] or 0),
            "%.0f" % ((v["video_bitrate_bps"] or 0) / 1000.0),
            a.get("encoding_name", "?").split()[0],
            "%.3f" % (a.get("sample_rate", 0) / 1000.0),
            str(a.get("channels")),
            "%.0f" % ((r["overall"]["file_bitrate_bps"] or 0) / 1000.0),
        ))
    w = [max(len(str(x[i])) for x in ([hdr] + rows)) for i in range(len(hdr))]
    def line(t): return "  ".join(str(t[i]).ljust(w[i]) for i in range(len(t)))
    print(line(hdr))
    print("  ".join("-" * x for x in w))
    for r in rows: print(line(r))


if __name__ == "__main__":
    args = sys.argv[1:]
    want_table = "--table" in args
    files = [a for a in args if not a.startswith("--")]
    res = []
    for p in files:
        sys.stderr.write("probing %s ...\n" % p); sys.stderr.flush()
        res.append(probe(p))
    if want_table:
        table(res)
    else:
        print(json.dumps({r["file_name"]: r for r in res}, indent=1, default=str))
