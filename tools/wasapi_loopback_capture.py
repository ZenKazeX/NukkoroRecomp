"""Bounded WASAPI default-render-endpoint loopback recorder (never a microphone).

Only the Python standard library is required. Native engine bytes are preserved;
silence-flagged packets are materialized as zero samples. Packet timestamps and
flags accompany the streamed WAV/raw recording in JSON, including partial runs.
"""

from __future__ import annotations

import argparse
import ctypes as C
import json
import math
import os
from pathlib import Path
import struct
import time
import uuid


PCM_GUID = "00000001-0000-0010-8000-00aa00389b71"
FLOAT_GUID = "00000003-0000-0010-8000-00aa00389b71"


def parse_format(blob: bytes) -> dict:
    if len(blob) < 18:
        raise ValueError("WAVEFORMATEX is shorter than 18 bytes")
    tag, channels, rate, byte_rate, align, bits, extra = struct.unpack_from("<HHIIHHH", blob)
    if len(blob) != 18 + extra or not channels or not rate or not align:
        raise ValueError("Invalid WAVEFORMATEX length or dimensions")
    kind = {1: "pcm", 3: "float"}.get(tag, "unknown")
    valid_bits, channel_mask, subtype = bits, None, None
    if tag == 0xFFFE:
        if extra < 22:
            raise ValueError("Truncated WAVEFORMATEXTENSIBLE")
        valid_bits, channel_mask = struct.unpack_from("<HI", blob, 18)
        subtype = str(uuid.UUID(bytes_le=blob[24:40]))
        kind = {PCM_GUID: "pcm", FLOAT_GUID: "float"}.get(subtype, "unknown")
    supported = ((kind == "pcm" and bits in (8, 16, 24, 32)) or
                 (kind == "float" and bits in (32, 64)))
    if supported and (align != channels * (bits // 8) or byte_rate != rate * align or
                      not 0 < valid_bits <= bits):
        raise ValueError("Invalid PCM/float frame stride, rate or valid-bit count")
    return dict(tag=tag, channels=channels, sample_rate=rate, byte_rate=byte_rate,
                block_align=align, bits_per_sample=bits, valid_bits=valid_bits,
                channel_mask=channel_mask, subtype=subtype, kind=kind,
                standard_wave=supported, raw_format_hex=blob.hex())


def wave_header(blob: bytes, frames: int) -> bytes:
    fmt = parse_format(blob)
    if not fmt["standard_wave"] or frames < 0:
        raise ValueError("Format/length cannot be written as a standard WAV")
    chunk = b"fmt " + struct.pack("<I", len(blob)) + blob
    if len(blob) & 1:
        chunk += b"\0"
    if fmt["kind"] == "float":
        chunk += b"fact" + struct.pack("<II", 4, frames)
    data_size = frames * fmt["block_align"]
    riff_size = 4 + len(chunk) + 8 + data_size + (data_size & 1)
    if riff_size > 0xFFFFFFFF:
        raise ValueError("RIFF recording exceeds 32-bit chunk length")
    return b"RIFF" + struct.pack("<I", riff_size) + b"WAVE" + chunk + b"data" + struct.pack("<I", data_size)


def normalized_samples(data: bytes, fmt: dict):
    """Diagnostic conversion only; recording bytes never pass through this."""
    bits = fmt["bits_per_sample"]
    if len(data) % fmt["block_align"]:
        raise ValueError("Partial audio frame")
    if not fmt["standard_wave"]:
        return
    if fmt["kind"] == "float":
        for (value,) in struct.iter_unpack("<f" if bits == 32 else "<d", data):
            yield value
    elif fmt["kind"] == "pcm":
        if bits == 8:
            for value in data:
                yield (value - 128) / 128.0
        elif bits in (16, 32):
            for (value,) in struct.iter_unpack("<h" if bits == 16 else "<i", data):
                yield value / float(1 << (bits - 1))
        elif bits == 24:
            for offset in range(0, len(data), 3):
                yield int.from_bytes(data[offset:offset + 3], "little", signed=True) / 8388608.0


def silence_bytes(frames: int, fmt: dict) -> bytes:
    # Eight-bit PCM is unsigned; its mathematical silence is 0x80.
    count = frames * fmt["block_align"]
    return bytes([128]) * count if fmt["kind"] == "pcm" and fmt["bits_per_sample"] == 8 else bytes(count)


class GUID(C.Structure):
    _fields_ = [("data1", C.c_uint32), ("data2", C.c_uint16), ("data3", C.c_uint16),
                ("data4", C.c_ubyte * 8)]

    @classmethod
    def make(cls, value):
        return cls.from_buffer_copy(uuid.UUID(value).bytes_le)


def check(hr, operation):
    if hr < 0:
        raise OSError(f"{operation} failed: HRESULT 0x{hr & 0xFFFFFFFF:08X}")
    return hr


def com_call(obj, index, arguments, *values):
    table = C.cast(obj, C.POINTER(C.POINTER(C.c_void_p))).contents
    call = C.WINFUNCTYPE(C.c_int32, C.c_void_p, *arguments)(table[index])
    return call(obj, *values)


def release(obj):
    if obj:
        table = C.cast(obj, C.POINTER(C.POINTER(C.c_void_p))).contents
        C.WINFUNCTYPE(C.c_uint32, C.c_void_p)(table[2])(obj)


class Loopback:
    def __init__(self):
        if os.name != "nt":
            raise OSError("WASAPI loopback requires Windows")
        self.ole = C.WinDLL("ole32")
        self.kernel = C.WinDLL("kernel32", use_last_error=True)
        self.enum = self.device = self.client = self.capture = None
        self.event = None
        self.com_owned = self.started = False
        self.ole.CoInitializeEx.argtypes = [C.c_void_p, C.c_uint32]
        self.ole.CoInitializeEx.restype = C.c_int32
        self.ole.CoCreateInstance.argtypes = [C.POINTER(GUID), C.c_void_p, C.c_uint32,
                                            C.POINTER(GUID), C.POINTER(C.c_void_p)]
        self.ole.CoCreateInstance.restype = C.c_int32
        self.ole.CoTaskMemFree.argtypes = [C.c_void_p]
        self.kernel.CreateEventW.argtypes = [C.c_void_p, C.c_int, C.c_int, C.c_wchar_p]
        self.kernel.CreateEventW.restype = C.c_void_p
        self.kernel.CloseHandle.argtypes = [C.c_void_p]
        self.kernel.WaitForSingleObject.argtypes = [C.c_void_p, C.c_uint32]
        self.kernel.WaitForSingleObject.restype = C.c_uint32

    def open(self):
        hr = self.ole.CoInitializeEx(None, 0)
        if (hr & 0xFFFFFFFF) != 0x80010106:  # pre-existing apartment is usable
            check(hr, "CoInitializeEx")
            self.com_owned = True
        clsid = GUID.make("bcde0395-e52f-467c-8e3d-c4579291692e")
        iid_enum = GUID.make("a95664d2-9614-4f35-a746-de8db63617e6")
        obj = C.c_void_p()
        check(self.ole.CoCreateInstance(C.byref(clsid), None, 23, C.byref(iid_enum), C.byref(obj)),
              "MMDeviceEnumerator")
        self.enum = obj
        obj = C.c_void_p()
        # eRender=0, eConsole=0. No capture endpoint is enumerated or activated.
        check(com_call(self.enum, 4, [C.c_int, C.c_int, C.POINTER(C.c_void_p)],
                       0, 0, C.byref(obj)), "GetDefaultAudioEndpoint(eRender)")
        self.device = obj
        identity = C.c_void_p()
        check(com_call(self.device, 5, [C.POINTER(C.c_void_p)], C.byref(identity)), "GetId")
        try:
            self.device_id = C.wstring_at(identity)
        finally:
            self.ole.CoTaskMemFree(identity)
        iid_client = GUID.make("1cb9ad4c-dbfa-4c32-b178-c2f568a703b2")
        obj = C.c_void_p()
        check(com_call(self.device, 3, [C.POINTER(GUID), C.c_uint32, C.c_void_p,
                                       C.POINTER(C.c_void_p)], C.byref(iid_client), 23,
                       None, C.byref(obj)), "Activate(IAudioClient)")
        self.client = obj
        fmt_ptr = C.c_void_p()
        check(com_call(self.client, 8, [C.POINTER(C.c_void_p)], C.byref(fmt_ptr)), "GetMixFormat")
        try:
            prefix = C.string_at(fmt_ptr, 18)
            self.format_blob = C.string_at(fmt_ptr, 18 + struct.unpack_from("<H", prefix, 16)[0])
            self.format = parse_format(self.format_blob)
            # Shared, loopback, event callback; use engine-selected buffer size.
            check(com_call(self.client, 3, [C.c_int, C.c_uint32, C.c_int64, C.c_int64,
                                           C.c_void_p, C.c_void_p],
                           0, 0x00020000 | 0x00040000, 0, 0, fmt_ptr, None), "Initialize(loopback)")
        finally:
            self.ole.CoTaskMemFree(fmt_ptr)
        period, minimum = C.c_int64(), C.c_int64()
        check(com_call(self.client, 9, [C.POINTER(C.c_int64), C.POINTER(C.c_int64)],
                       C.byref(period), C.byref(minimum)), "GetDevicePeriod")
        self.period_100ns, self.minimum_period_100ns = period.value, minimum.value
        size = C.c_uint32()
        check(com_call(self.client, 4, [C.POINTER(C.c_uint32)], C.byref(size)), "GetBufferSize")
        self.buffer_frames = size.value
        self.event = self.kernel.CreateEventW(None, False, False, None)
        if not self.event:
            raise C.WinError(C.get_last_error())
        check(com_call(self.client, 13, [C.c_void_p], self.event), "SetEventHandle")
        iid_capture = GUID.make("c8adbd64-e71e-48a0-a4de-185c395cd317")
        obj = C.c_void_p()
        check(com_call(self.client, 14, [C.POINTER(GUID), C.POINTER(C.c_void_p)],
                       C.byref(iid_capture), C.byref(obj)), "GetService(IAudioCaptureClient)")
        self.capture = obj

    def close(self):
        if self.started:
            com_call(self.client, 11, [])
            self.started = False
        for name in ("capture", "client", "device", "enum"):
            release(getattr(self, name))
            setattr(self, name, None)
        if self.event:
            self.kernel.CloseHandle(self.event)
            self.event = None
        if self.com_owned:
            self.ole.CoUninitialize()
            self.com_owned = False


def record(out: Path, duration: float, max_frames: int | None, packet_records: int):
    session = Loopback()
    metadata = dict(schema=1, endpoint="default eRender/eConsole loopback", microphone=False,
                    duration_bound_s=duration, user_frame_bound=max_frames, packets=[],
                    packet_records_limit=packet_records, packet_records_dropped=0,
                    delivered_frames=0, written_frames=0, truncated_frames=0, packets_total=0,
                    silent_packets=0, silent_frames=0, discontinuities=0, timestamp_errors=0,
                    device_gap_events=0, device_gap_frames=0, device_position_regressions=0,
                    event_wakes=0, event_timeouts=0, peak=0.0, clipped_samples=0,
                    nonfinite_samples=0, analyzed_samples=0, scaled_sum_squares=0.0)
    base = out.with_suffix("") if out.suffix in (".wav", ".raw", ".json") else out
    base.parent.mkdir(parents=True, exist_ok=True)
    stream, fmt_blob = None, None
    previous_end = None
    start = None
    try:
        session.open()
        fmt, fmt_blob = session.format, session.format_blob
        metadata.update(format=fmt, device_id=session.device_id,
                        device_period_100ns=session.period_100ns,
                        minimum_period_100ns=session.minimum_period_100ns,
                        engine_buffer_frames=session.buffer_frames)
        frame_bound = min(math.ceil(duration * fmt["sample_rate"]),
                          max_frames if max_frames is not None else 2**63 - 1)
        standard = fmt["standard_wave"]
        if standard:
            try:
                wave_header(fmt_blob, frame_bound)
            except ValueError:  # retain long recordings as raw rather than overflow RIFF
                standard = False
        path = Path(str(base) + (".wav" if standard else ".raw"))
        metadata.update(output=str(path.resolve()), frame_bound=frame_bound, container="wav" if standard else "raw")
        stream = path.open("wb")
        if standard:
            stream.write(wave_header(fmt_blob, 0))
        check(com_call(session.client, 10, []), "Start")
        session.started = True
        start = time.perf_counter()
        print("READY " + json.dumps(dict(endpoint="eRender/eConsole loopback",
              sample_rate=fmt["sample_rate"], channels=fmt["channels"],
              block_align=fmt["block_align"], output=str(path.resolve()))), flush=True)
        deadline = start + duration
        while metadata["written_frames"] < frame_bound and time.perf_counter() < deadline:
            size = C.c_uint32()
            check(com_call(session.capture, 5, [C.POINTER(C.c_uint32)], C.byref(size)), "GetNextPacketSize")
            if not size.value:
                wait_ms = min(100, max(1, math.ceil((deadline - time.perf_counter()) * 1000)))
                result = session.kernel.WaitForSingleObject(session.event, wait_ms)
                if result == 0:
                    metadata["event_wakes"] += 1
                elif result == 0x102:
                    metadata["event_timeouts"] += 1
                else:
                    raise OSError(f"WaitForSingleObject failed: 0x{result:08X}")
                continue
            data = C.c_void_p()
            frames, flags = C.c_uint32(), C.c_uint32()
            position, qpc = C.c_uint64(), C.c_uint64()
            check(com_call(session.capture, 3, [C.POINTER(C.c_void_p), C.POINTER(C.c_uint32),
                       C.POINTER(C.c_uint32), C.POINTER(C.c_uint64), C.POINTER(C.c_uint64)],
                       C.byref(data), C.byref(frames), C.byref(flags), C.byref(position), C.byref(qpc)), "GetBuffer")
            if not frames.value:
                continue
            try:
                kept = min(frames.value, frame_bound - metadata["written_frames"])
                count = kept * fmt["block_align"]
                raw = silence_bytes(kept, fmt) if flags.value & 2 else C.string_at(data, count)
            finally:
                # End the engine-buffer lease before file I/O or analysis.
                check(com_call(session.capture, 4, [C.c_uint32], frames.value), "ReleaseBuffer")
            stream.write(raw)
            metadata["delivered_frames"] += frames.value
            metadata["written_frames"] += kept
            metadata["truncated_frames"] += frames.value - kept
            metadata["packets_total"] += 1
            if flags.value & 2:
                metadata["silent_packets"] += 1
                metadata["silent_frames"] += frames.value
            metadata["discontinuities"] += bool(flags.value & 1)
            metadata["timestamp_errors"] += bool(flags.value & 4)
            if not flags.value & 4:
                if previous_end is not None and position.value > previous_end:
                    metadata["device_gap_events"] += 1
                    metadata["device_gap_frames"] += position.value - previous_end
                elif previous_end is not None and position.value < previous_end:
                    metadata["device_position_regressions"] += 1
                previous_end = position.value + frames.value
            else:
                previous_end = None
            if len(metadata["packets"]) < packet_records:
                metadata["packets"].append(dict(frames=frames.value, written_frames=kept,
                    flags=flags.value, device_position=position.value, qpc_100ns=qpc.value,
                    host_elapsed_s=time.perf_counter() - start))
            else:
                metadata["packet_records_dropped"] += 1
            for sample in normalized_samples(raw, fmt):
                if not math.isfinite(sample):
                    metadata["nonfinite_samples"] += 1
                    continue
                metadata["analyzed_samples"] += 1
                magnitude, peak = abs(sample), metadata["peak"]
                # Scaled sum of squares remains finite even for malformed but
                # finite float64 samples. It never changes the recorded bytes.
                if magnitude > peak:
                    metadata["scaled_sum_squares"] = metadata["scaled_sum_squares"] * (peak / magnitude)**2 + 1
                    metadata["peak"] = magnitude
                elif peak:
                    metadata["scaled_sum_squares"] += (magnitude / peak)**2
                metadata["clipped_samples"] += magnitude >= 1.0
        metadata["stop_reason"] = "frame bound" if metadata["written_frames"] == frame_bound else "duration bound"
    except BaseException as error:
        metadata["error"] = f"{type(error).__name__}: {error}"
        raise
    finally:
        session.close()
        if stream:
            if metadata.get("container") == "wav":
                if metadata["written_frames"] * metadata["format"]["block_align"] & 1:
                    stream.write(b"\0")
                stream.seek(0)
                stream.write(wave_header(fmt_blob, metadata["written_frames"]))
            stream.close()
        metadata["wall_s"] = time.perf_counter() - start if start is not None else 0
        metadata["rms"] = metadata["peak"] * math.sqrt(metadata["scaled_sum_squares"] / metadata["analyzed_samples"]) if metadata["analyzed_samples"] else 0
        Path(str(base) + ".json").write_text(json.dumps(metadata, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    return metadata


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, required=True, help="Recording basename (WAV/raw plus JSON)")
    parser.add_argument("--duration", type=float, default=30, help="Wall time bound, 0.01..600 seconds")
    parser.add_argument("--max-frames", type=int, help="Additional output frame bound")
    parser.add_argument("--packet-records", type=int, default=4096, help="Bounded timestamp records, 0..65536")
    args = parser.parse_args()
    if not math.isfinite(args.duration) or not 0.01 <= args.duration <= 600:
        parser.error("duration must be finite and between 0.01 and 600 seconds")
    if args.max_frames is not None and args.max_frames <= 0:
        parser.error("max-frames must be positive")
    if not 0 <= args.packet_records <= 65536:
        parser.error("packet-records must be between 0 and 65536")
    try:
        result = record(args.out, args.duration, args.max_frames, args.packet_records)
    except (OSError, ValueError) as error:
        parser.exit(1, str(error) + "\n")
    print(json.dumps({key: result[key] for key in ("output", "written_frames", "wall_s", "peak", "rms",
          "discontinuities", "device_gap_frames", "silent_frames")}, indent=2))


if __name__ == "__main__":
    main()
