import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import re


RULES = {
    "legacy_region": r"\bkMem1Size\b|\bkAramSize\b|0x0?1800000|0x81800000|24\s*\*\s*1024\s*\*\s*1024",
    "legacy_address_mask": r"&\s*0x(?:0?1ffffff|3fffffff|0?0ffffff)",
    "guest_memory_access": r"\bmem_(?:read|write)\d+\(|\.mem1\(\)|\.aram\(\)|\bmem1_base_\b",
    "narrowed_host_extent_review": r"static_cast<(?:u32|uint32_t)>\([^\n]*(?:size\(|offset|bytes|address)",
    "fixed_storage_review": r"std::array<|\bk[A-Za-z]*(?:Pool|Arena|Heap|Ring|Capacity|Scratch)[A-Za-z]*\s*=",
    "native_owned_allocation": r"shadow::memory::(?:Vector|system)|\bMemorySystem::(?:allocate|reserve|map|commit)",
    "gpu_owned_allocation": r"CreateCommittedResource|createBuffer\(|createTexture\(|RenderHeapType::",
}


def audit(root):
    files = set()
    for directory in (root / "runtime", root / "tools" / "ppcrecomp"):
        for extension in ("*.h", "*.hpp", "*.cpp"):
            files.update(directory.rglob(extension))
    findings = []
    counts = Counter()
    fingerprints = {}
    patterns = {name: re.compile(pattern, re.IGNORECASE) for name, pattern in RULES.items()}
    for path in sorted(files):
        if "thirdparty" in path.parts or "build" in path.parts:
            continue
        relative = path.relative_to(root).as_posix()
        raw = path.read_bytes()
        fingerprints[relative] = hashlib.sha256(raw).hexdigest()
        for line_number, line in enumerate(raw.decode("utf-8", errors="replace").splitlines(), 1):
            matched = [name for name, pattern in patterns.items() if pattern.search(line)]
            if matched:
                counts.update(matched)
                findings.append({"file": relative, "line": line_number, "categories": matched,
                                 "evidence": line.strip(), "status": "source-review-candidate-not-liveness-proof"})
    core = [root / "runtime/include/native_memory.h", root / "runtime/src/native_memory.cpp",
            root / "runtime/include/native_execution.h", root / "runtime/src/native_execution.cpp"]
    forbidden = re.compile(r'#\s*include\s*[<"]gekko/|\b(?:Cpu|Bus|kMem1Size|MEM1|kAramSize|recomp::)\b')
    violations = []
    for path in core:
        for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
            if forbidden.search(line):
                violations.append({"file": path.relative_to(root).as_posix(), "line": number, "text": line})
    return {
        "scope": "maintained runtime and recompiler C/C++ sources, including tests; generated/vendor/reference archives excluded",
        "interpretation": "Matches identify review candidates, not proof of live dependencies; file-format fields and GPU indices can legitimately be 32-bit.",
        "whole_game_native": False,
        "known_remaining_boundaries": {
            "translated_game": "Cpu register state, 32-bit game pointers, BAT translation and MEM1 game heap remain.",
            "dma_audio": "Legacy AI/AX/DSP sources and ARAM transfers remain; native audio output buffers have separate ownership.",
            "capture": "RenderWare capture still imports guest-addressed resources; native packets, geometry and scratch are separately allocated.",
            "resource_requests": "Existing game request destinations remain validated MEM1 addresses; host files and decoded assets are native allocations.",
            "savestates": "Legacy game storage is still serialized; native allocation handles are not guest pointers or saved addresses.",
            "accounting": "Native diagnostics cover migrated buffers, not every CRT/third-party/GPU allocation; process and GPU reports remain separate.",
        },
        "native_core_dependency_violations": violations,
        "category_counts": dict(counts), "source_sha256": fingerprints, "findings": findings,
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--out", type=Path)
    args = parser.parse_args()
    result = audit(args.root.resolve())
    if args.out:
        with args.out.open("x", encoding="utf-8") as stream:
            json.dump(result, stream, indent=2)
    print(json.dumps({"counts": result["category_counts"],
                      "native_core_dependency_violations": result["native_core_dependency_violations"],
                      "whole_game_native": False}, indent=2))
    return 1 if result["native_core_dependency_violations"] else 0


if __name__ == "__main__":
    raise SystemExit(main())
