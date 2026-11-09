"""One host code-generation model per image that contains Gekko guest-semantics code (L0r0-01).

runtime/include/gekko/*.h holds inline guest-semantics helpers whose DEFINITION depends on the host
instruction set the including translation unit is compiled for (ppc_ops.h: cntlzw is LZCNT under
__AVX2__ and BSR otherwise; fused_multiply_add is the FMA3 instruction under __AVX2__/__FMA__ and the
C library fma otherwise), and GEKKO_NOINLINE members of Cpu (cpu.h mem_read32 and friends) that are
emitted as COMDATs by every TU that uses them. If two TUs linked into one image are compiled for
different /arch or /fp models the program is ill-formed (ODR, [basic.def.odr]/13, no diagnostic
required) and MSVC keeps whichever COMDAT it sees first: before this check existed the shipping
ShadowTheHedgehog.exe and ShadowTheHedgehogReference.exe compiled shadow_main.cpp WITHOUT
SHADOW_ARCH_FLAGS and every shard call to Cpu::mem_read32 ran the shadow_main.obj copy.

Three lanes, every one must pass:

  A  configuration: from the CMake file API codemodel, every C/C++ compile group that can include
     runtime/include/gekko, across an executable and the static/object libraries it links, has the
     same host ISA and floating-point model flags.
  B  objects: the libraries of those images (runtime, generated shards) and the own objects of the
     required shipping images carry the gekko_host_* linker directives (#pragma detect_mismatch in
     gekko/types.h); across every object of an image each key has one value, and that value is
     the one the lane A flags imply.
  C  linker: a synthetic COFF object that contradicts shadow_main.obj's gekko_host_* directives is
     rejected with LNK2038 for every key, and one that agrees is not - i.e. a future TU built with a
     different ISA cannot be linked into the image silently.
"""
import argparse
import json
import pathlib
import re
import shlex
import struct
import subprocess
import sys

GEKKO_KEYS = ("gekko_host_isa", "gekko_host_fma", "gekko_host_fp", "gekko_host_fp_contract")
LIB_TYPES = {"STATIC_LIBRARY", "OBJECT_LIBRARY"}
IMAGE_TYPES = {"EXECUTABLE", "SHARED_LIBRARY", "MODULE_LIBRARY"}


class Failure(Exception):
    pass


# ---------------------------------------------------------------- lane A: configuration
def flag_model(fragments):
    """(arch, fp_model, fp_contract) as cl.exe resolves the flag list: last /arch wins; /fp:fast
    implies contraction, /fp:precise and /fp:strict turn it off, /fp:contract turns it on."""
    arch, model, contract = "default", "precise", False
    for fragment in fragments:
        try:
            tokens = shlex.split(fragment, posix=False)
        except ValueError:
            tokens = fragment.split()
        for token in tokens:
            t = token.strip('"')
            m = re.fullmatch(r"[/-]arch:(\S+)", t, re.IGNORECASE)
            if m:
                arch = m.group(1).upper()
                continue
            m = re.fullmatch(r"[/-]fp:(\S+)", t, re.IGNORECASE)
            if m:
                v = m.group(1).lower()
                if v == "fast":
                    model, contract = "fast", True
                elif v in ("precise", "strict"):
                    model, contract = v, False
                elif v == "contract":
                    contract = True
    return arch, model, contract


def expected_directives(model):
    """The gekko_host_* values gekko/types.h must produce for these flags (None = do not compare)."""
    arch, fp, contract = model
    isa = {"default": "SSE", "SSE2": "SSE", "SSE4.2": "SSE", "AVX": "AVX", "AVX2": "AVX2",
           "AVX512": "AVX512"}.get(arch)
    if isa is None:
        return None
    return {"gekko_host_isa": isa, "gekko_host_fma": "1" if isa in ("AVX2", "AVX512") else "0",
            "gekko_host_fp": fp, "gekko_host_fp_contract": "1" if contract else "0"}


def load_codemodel(build, config, cmake=None):
    reply = build / ".cmake" / "api" / "v1" / "reply"
    indexes = sorted(reply.glob("index-*.json"))
    if not indexes and cmake:
        # CMakeLists.txt writes the client query during a configure, and CMake only answers queries
        # that existed when that configure started: a fresh tree has no reply until it regenerates
        # once more. Regenerate it exactly as ZERO_CHECK would.
        subprocess.run([cmake, str(build)], capture_output=True, text=True, errors="replace", timeout=600)
        indexes = sorted(reply.glob("index-*.json"))
    if not indexes:
        raise Failure(f"no CMake file API reply under {reply}; reconfigure the build tree")
    index = json.loads(indexes[-1].read_text(encoding="utf-8"))
    try:
        codemodel_file = index["reply"]["client-gekkoforge"]["codemodel-v2"]["jsonFile"]
    except KeyError as exc:
        raise Failure(f"{indexes[-1].name} has no client-gekkoforge codemodel-v2 reply; reconfigure") from exc
    codemodel = json.loads((reply / codemodel_file).read_text(encoding="utf-8"))
    for configuration in codemodel["configurations"]:
        if configuration["name"].lower() == config.lower():
            targets = {}
            for entry in configuration["targets"]:
                data = json.loads((reply / entry["jsonFile"]).read_text(encoding="utf-8"))
                targets[data["id"]] = data
            return codemodel, targets
    raise Failure(f"configuration {config} not in the codemodel")


def gekko_groups(target, gekko_include):
    groups = []
    for group in target.get("compileGroups", []):
        if group.get("language") not in ("C", "CXX"):
            continue
        includes = {pathlib.Path(i["path"]).resolve() for i in group.get("includes", [])}
        if gekko_include in includes:
            fragments = [f["fragment"] for f in group.get("compileCommandFragments", [])]
            groups.append(flag_model(fragments))
    return groups


def closure(target, targets):
    seen, stack, out = {target["id"]}, [target], [target]
    while stack:
        for dep in stack.pop().get("dependencies", []):
            t = targets.get(dep["id"])
            if t is None or t["id"] in seen or t["type"] not in LIB_TYPES:
                continue
            seen.add(t["id"])
            out.append(t)
            stack.append(t)
    return out


def lane_a(targets, gekko_include):
    images, failures = [], []
    for target in sorted(targets.values(), key=lambda t: t["name"]):
        if target["type"] not in IMAGE_TYPES:
            continue
        members = closure(target, targets)
        per_target = {m["name"]: sorted(set(gekko_groups(m, gekko_include))) for m in members}
        per_target = {k: v for k, v in per_target.items() if v}
        if not per_target:
            continue
        models = {model for v in per_target.values() for model in v}
        if len(models) != 1:
            detail = "; ".join(f"{name}: {', '.join('/arch:%s /fp:%s%s' % (a, f, ' contract' if c else '') for a, f, c in v)}"
                               for name, v in sorted(per_target.items()))
            failures.append(f"{target['name']}: {len(models)} host code-generation models in one image -> {detail}")
            images.append((target, members, per_target, None))
            continue
        images.append((target, members, per_target, next(iter(models))))
        print(f"PASS A {target['name']}: /arch:{images[-1][3][0]} /fp:{images[-1][3][1]}"
              f"{' contract' if images[-1][3][2] else ''} across {', '.join(sorted(per_target))}")
    return images, failures


# ---------------------------------------------------------------- lane B: object directives
_DIRECTIVE = re.compile(r'/FAILIFMISMATCH:"?([^=\s"]+)=([^"\s]*)"?', re.IGNORECASE)


def directive_blocks(dumpbin, path, cache):
    """Per member (a lib has one Linker Directives block per member): {key: value} of gekko_host_*."""
    if path in cache:
        return cache[path]
    result = subprocess.run([dumpbin, "/nologo", "/directives", str(path)], capture_output=True, text=True,
                            errors="replace")
    if result.returncode != 0:
        raise Failure(f"dumpbin /directives {path} failed:\n{result.stdout}{result.stderr}")
    blocks, current = [], None
    for line in result.stdout.splitlines():
        if line.strip() == "Linker Directives":
            current = {}
            blocks.append(current)
            continue
        if current is None:
            continue
        for key, value in _DIRECTIVE.findall(line):
            if key.startswith("gekko_host_"):
                current.setdefault(key, set()).add(value)
    cache[path] = blocks
    return blocks


def target_inputs(target, build, config):
    if target["type"] == "STATIC_LIBRARY":
        return [build / a["path"] for a in target.get("artifacts", []) if a["path"].lower().endswith(".lib")]
    base = build / target["paths"]["build"]
    objs = sorted((base / f"{target['name']}.dir" / config).rglob("*.obj"))
    objs += sorted((base / "CMakeFiles" / f"{target['name']}.dir").rglob("*.obj"))
    return objs


def lane_b(images, build, config, dumpbin, require):
    failures, cache, anchors = [], {}, {}
    for image, members, per_target, model in images:
        artifact = next((build / a["path"] for a in image.get("artifacts", [])
                         if a["path"].lower().endswith((".exe", ".dll"))), None)
        if artifact is None or not artifact.exists():
            if image["name"] in require:
                failures.append(f"{image['name']}: artefact {artifact} is missing (build it first)")
            else:
                print(f"SKIP B {image['name']}: not built")
            continue
        want = expected_directives(model) if model is not None else None
        seen = {}
        for member in members:
            if member["name"] not in per_target:
                continue
            inputs = target_inputs(member, build, config)
            if not inputs:
                failures.append(f"{image['name']}: no object files found for {member['name']}")
                continue
            carrying = 0
            for path in inputs:
                for block in directive_blocks(dumpbin, path, cache):
                    if not block:
                        continue
                    missing = [k for k in GEKKO_KEYS if k not in block]
                    if missing:
                        failures.append(f"{image['name']}: {path.name} carries gekko_host_* directives but lacks {missing}")
                    carrying += 1
                    for key, values in block.items():
                        seen.setdefault(key, {}).setdefault(frozenset(values), set()).add(path.name)
                if member["name"] == image["name"] and any(block for block in cache[path]) and path.suffix == ".obj":
                    anchors.setdefault(image["name"], path)
            if carrying == 0:
                # Libraries in the closure are the core (runtime, generated shards) and a required image's own
                # objects are the shipping program TUs: those must carry the model. Any other image's own
                # TUs may use gekko/ headers that never include gekko/types.h (host_jobs.h, the shaderbake
                # shader cache), which define no guest-semantics COMDAT; their flags are still held by lane A.
                if member["type"] in LIB_TYPES or image["name"] in require:
                    failures.append(f"{image['name']}: {member['name']} was compiled with gekko headers but none of "
                                    f"its objects carries the gekko_host_* linker directives (#pragma detect_mismatch)")
                else:
                    print(f"INFO B {image['name']}: own objects carry no gekko_host_* directives "
                          f"(no TU of {member['name']} includes gekko/types.h)")
        for key in GEKKO_KEYS:
            variants = seen.get(key, {})
            flat = {v for values in variants for v in values}
            if len(flat) > 1:
                failures.append(f"{image['name']}: {key} has {sorted(flat)} across "
                                + "; ".join(f"{sorted(v)} in {sorted(n)[:4]}" for v, n in variants.items()))
            elif flat and want is not None and flat != {want[key]}:
                failures.append(f"{image['name']}: {key}={next(iter(flat))} but the compile flags require {want[key]}")
        if not [f for f in failures if f.startswith(image["name"] + ":")]:
            print(f"PASS B {image['name']}: " + ", ".join(
                f"{k}={next(iter({v for vs in seen.get(k, {}) for v in vs}), '?')}" for k in GEKKO_KEYS))
    return failures, anchors


# ---------------------------------------------------------------- lane C: linker rejection
def coff_with_directives(pairs):
    data = b" ".join(f'/FAILIFMISMATCH:"{k}={v}"'.encode("ascii") for k, v in pairs) + b" "
    header = struct.pack("<HHIIIHH", 0x8664, 1, 0, 20 + 40 + len(data), 0, 0, 0)
    # .drectve: IMAGE_SCN_LNK_INFO | IMAGE_SCN_LNK_REMOVE | IMAGE_SCN_ALIGN_1BYTES
    section = struct.pack("<8sIIIIIIHHI", b".drectve", 0, 0, len(data), 60, 0, 0, 0, 0, 0x00100A00)
    return header + section + data + struct.pack("<I", 4)


def lane_c(anchor, dumpbin, linker, work):
    failures = []
    blocks = [b for b in directive_blocks(dumpbin, anchor, {}) if b]
    if not blocks:
        return [f"lane C: {anchor} carries no gekko_host_* directives to contradict"]
    real = {k: next(iter(v)) for k, v in blocks[0].items()}
    work.mkdir(parents=True, exist_ok=True)

    def link(name, pairs):
        obj = work / f"{name}.obj"
        obj.write_bytes(coff_with_directives(pairs))
        result = subprocess.run([linker, "/NOLOGO", "/DLL", "/NOENTRY", "/NODEFAULTLIB", "/MACHINE:X64",
                                 "/FORCE:UNRESOLVED", str(obj), str(anchor), f"/OUT:{work / (name + '.dll')}"],
                                capture_output=True, text=True, errors="replace", cwd=work)
        return result.stdout + result.stderr

    agree = link("agree", sorted(real.items()))
    if "LNK2038" in agree and "gekko_host_" in agree:
        failures.append("lane C: an object with the SAME gekko_host_* values was rejected:\n" + agree[:2000])
    contradict = {k: ("contradiction" if v != "contradiction" else "other") for k, v in real.items()}
    output = link("contradict", sorted(contradict.items()))
    for key in GEKKO_KEYS:
        if key not in real:
            failures.append(f"lane C: {anchor.name} lacks {key}")
        elif not re.search(rf"LNK2038: mismatch detected for '{key}'", output):
            failures.append(f"lane C: the linker did not reject a contradicting {key} next to {anchor.name}:\n"
                            + output[:2000])
    if not failures:
        print(f"PASS C linker rejects contradicting {', '.join(GEKKO_KEYS)} next to {anchor}")
    return failures


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--build", required=True, type=pathlib.Path)
    parser.add_argument("--source", required=True, type=pathlib.Path)
    parser.add_argument("--config", default="Release")
    parser.add_argument("--dumpbin", required=True)
    parser.add_argument("--linker", required=True)
    parser.add_argument("--work", required=True, type=pathlib.Path)
    parser.add_argument("--cmake", help="regenerate the tree once if it has no file API reply yet")
    parser.add_argument("--require", action="append", default=[],
                        help="image whose artefact must exist (and anchors lane C)")
    args = parser.parse_args()
    build = args.build.resolve()
    gekko_include = (args.source / "runtime" / "include").resolve()
    failures = []
    try:
        _, targets = load_codemodel(build, args.config, args.cmake)
        images, fa = lane_a(targets, gekko_include)
        failures += fa
        names = {t["name"] for t, *_ in images}
        for required in args.require:
            if required not in names and not any(required in f for f in fa):
                failures.append(f"{required}: not found as an image containing gekko headers")
        fb, anchors = lane_b(images, build, args.config, args.dumpbin, set(args.require))
        failures += fb
        anchor_name = next((r for r in args.require if r in anchors), None)
        if anchor_name is None:
            failures.append("lane C: no required image has an object carrying gekko_host_* directives to anchor on")
        else:
            failures += lane_c(anchors[anchor_name], args.dumpbin, args.linker, args.work)
    except Failure as exc:
        failures.append(str(exc))
    if failures:
        print(f"FAIL host ISA consistency: {len(failures)} problem(s)")
        for failure in failures:
            print("  - " + failure)
        return 1
    print("PASS host ISA consistency")
    return 0


if __name__ == "__main__":
    sys.exit(main())
