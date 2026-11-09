
#!/usr/bin/env python3
"""Exercise actual ppcrecomp output stability on a small synthetic PPC DOL."""
import argparse
import csv
import hashlib
import io
from pathlib import Path
import struct
import subprocess
import tempfile

def gap_sweep_roots(generator,root):
    """decode-cfg-3: a speculative gap-sweep root must never reject a function
    that is valid from its declared entry. It runs WITH the gap sweep."""
    out=root/"gap-sweep";out.mkdir()
    base=0x80003100
    words=[
        0x38600001,0x4E800020,0x38600005,0x4E800020,  # 0x00 C: li;blr; recovered routine li;blr
        0x38600001,0x4E800020,0x4D657472,0x4D657472,  # 0x10 A: li;blr; "MetrMetr" (19/569)
        0x38600002,0x4E800020,0x38630001,0x38630001,  # 0x20 B: li;blr; addi;addi -> runs off .text
    ]
    dol=bytearray(0x100+4*len(words))
    for offset,value in [(0,0x100),(0x48,base),(0x90,4*len(words)),(0xe0,base)]:
        struct.pack_into(">I",dol,offset,value)
    struct.pack_into(f">{len(words)}I",dol,0x100,*words)
    image=root/"gap-sweep.dol";image.write_bytes(dol)
    functions=root/"gap-sweep.csv"
    stream=io.StringIO(newline="")
    writer=csv.writer(stream)
    writer.writerow(["addr_hex","addr","name","size","ins","tags","strings","callees"])
    # B declares an extent past the end of the only executable section.
    for address,size,name in [(base,8,"C"),(base+0x10,8,"A"),(base+0x20,0x14,"B")]:
        writer.writerow([hex(address),address,name,size,size//4,"","",""])
    functions.write_text(stream.getvalue(),encoding="utf-8")
    result=subprocess.run([str(generator.resolve()),"--dol",str(image),"--functions",str(functions),
        "--out",str(out),"--report",str(out/"recomp_report.txt"),"--shards","1"],
        capture_output=True,text=True)
    assert result.returncode==0,result.stdout+result.stderr
    report=(out/"recomp_report.txt").read_text()
    rejected=report.split("rejected function entries",1)[1] if "rejected function entries" in report else ""
    # A: "Metr" after its blr used to fail emission of the whole routine
    # ("group-19 extended 569"); B: the swept addi;addi used to fall off .text
    # ("code outside an executable section"). Both roots are dropped instead.
    assert "0x80003110" not in rejected and "0x80003120" not in rejected,report
    assert "gap sweep: 2 speculative roots dropped" in result.stderr,result.stderr
    table=(out/"recomp_table.cpp").read_text()
    for accepted in ("{0x80003110u, 0x80003118u, &fn_80003110_at}",
                     "{0x80003120u, 0x80003128u, &fn_80003120_at}",
                     # control: a root that decodes is still recovered
                     "{0x80003100u, 0x80003110u, &fn_80003100_at}",
                     "&fn_80003100_e80003108"):
        assert accepted in table,accepted+"\n"+table
    # The dropped data word is a row of its own, rejected from its own entry
    # like any data row the function table carries.
    assert "0x80003118  group-19 extended 569" in rejected,report

def bctr_loop_lowering(generator,root):
    """branch-sys-1: a bctr whose targets are this function's labels - including
    the entries of rows it absorbed - must be lowered to gotos, never to a call.
    Whether a host compiler turns the old call into a tail jump depends on the
    function (MSVC does for small bodies, not for the >4,000-instruction jump-table
    decoders), so the lowering itself is checked here; native_fallback_tests
    runs the same loop shape and measures the host stack."""
    out=root/"bctr-loop";out.mkdir()
    base=0x80003100;case1=base+0x18
    words=[0x38600000,0x38800000,0x3D000000|(case1>>16),0x61080000|(case1&0xFFFF),
           0x2C030000,0x41820010,0x38840001,0x38840002,0x48000004,
           0x7C032840,0x40800018,0x38630001,0x5466177A,0x7CE83214,
           0x7CE903A6,0x4E800420,0x4E800020,0x4E800020]
    dol=bytearray(0x100+4*len(words))
    for offset,value in [(0,0x100),(0x48,base),(0x90,4*len(words)),(0xe0,base)]:
        struct.pack_into(">I",dol,offset,value)
    struct.pack_into(f">{len(words)}I",dol,0x100,*words)
    image=root/"bctr-loop.dol";image.write_bytes(dol)
    functions=root/"bctr-loop.csv"
    stream=io.StringIO(newline="")
    writer=csv.writer(stream)
    writer.writerow(["addr_hex","addr","name","size","ins","tags","strings","callees"])
    for address,size,name in [(base,0x44,"loop"),(case1,12,"case1"),(case1+4,8,"case2"),(base+0x44,4,"after")]:
        writer.writerow([hex(address),address,name,size,size//4,"","",""])
    functions.write_text(stream.getvalue(),encoding="utf-8")
    result=subprocess.run([str(generator.resolve()),"--dol",str(image),"--functions",str(functions),
        "--out",str(out),"--shards","1","--no-gap-sweep","--no-wide-entries"],capture_output=True,text=True)
    assert result.returncode==0,result.stdout+result.stderr
    text=(out/"recomp_000.cpp").read_text()
    for row in ("80003118","8000311C"):
        assert f"static void fn_{row}_body(Cpu& c, u32 entry) {{ fn_80003100_at(c, entry); }}" in text,row+" not absorbed\n"+text
    hot=text.split("static void fn_80003100_body(Cpu& c, u32 entry) {",1)[1].split("static void fn_80003100_body_at(Cpu& c, u32 entry) {",1)[0]
    for case in ("80003118","8000311C"):
        assert f"\nL_{case}:\n" in hot,"absorbed case entry is not a hot label: "+case+"\n"+hot
        assert f"case 0x{case}u: if (c.try_hle(0x{case}u)) return; goto L_{case};" in hot,case+"\n"+hot
    assert "recomp::jump_indirect(c, tmp); return;" not in text,"bctr still lowered to a call\n"+text
    assert "c.leave_to_dispatcher(tmp, FallbackReason::Yield, 0x8000313Cu); return; }" in hot,"backward bctr does not poll\n"+hot

def shared_indirect_lowering(generator,root):
    """Adding computed-jump sites must not replicate the local dispatch table."""
    for sites in (2, 12):
        out=root/f"shared-bctr-{sites}";out.mkdir()
        base=0x80003100
        # Conditional beqctr at each site; the not-taken path reaches the next.
        # Each explicit branch targets the return, supplying a known local label.
        words=[]
        for i in range(sites):
            words += [0x4D820420, 0x40820000 | ((sites*8-(i*8+4)) & 0xfffc)]
        words.append(0x4E800020)
        dol=bytearray(0x100+4*len(words))
        for offset,value in [(0,0x100),(0x48,base),(0x90,4*len(words)),(0xe0,base)]:
            struct.pack_into(">I",dol,offset,value)
        struct.pack_into(f">{len(words)}I",dol,0x100,*words)
        image=out/"input.dol";image.write_bytes(dol)
        functions=out/"functions.csv"
        functions.write_text(f"addr_hex,name,size\n{base:x},shared,{len(words)*4}\n")
        result=subprocess.run([str(generator.resolve()),"--dol",str(image),"--functions",str(functions),
            "--out",str(out),"--shards","1","--no-gap-sweep","--no-wide-entries"],capture_output=True,text=True)
        assert result.returncode==0,result.stdout+result.stderr
        text=(out/"recomp_000.cpp").read_text()
        hot=text.split("static void fn_80003100_body(Cpu& c, u32 entry) {",1)[1].split("static void fn_80003100_body_at(Cpu& c, u32 entry) {",1)[0]
        assert hot.count("gekko_indirect_dispatch:") == 1, hot
        assert hot.count("goto gekko_indirect_dispatch;") == sites, hot
        assert hot.count("switch (tmp)") == 2, "dispatch tables grew with site count"
        assert "if (tmp <= gekko_indirect_site)" in hot, hot
        assert "FallbackReason::Yield, gekko_indirect_site" in hot, hot
        for i in range(sites):
            assert f"gekko_indirect_site = 0x{base+i*8:X}u;" in hot, hot
        assert "default: recomp::jump_indirect(c, tmp); return;" in hot, hot

def interrupt_recognition_lowering(generator,root):
    """Audit batch b5. branch-sys-3: mtmsr checks for a deliverable interrupt
    before the write (resuming AT the mtmsr) and after it (resuming at
    mtmsr + 4); nothing is retired around it (native time, 2026-10-05: there is
    no instruction count). branch-sys-2: a backward branch to another record
    publishes pc and polls needs_dispatch like an in-body back edge; the
    end-of-record fall-through does not."""
    import re
    out=root/"interrupt-recognition";out.mkdir()
    base=0x80003100
    words=[0x7C6000A6,0x5464045E,0x7C800124,0x4E800020,  # mfmsr r3; rlwinm r4,r3,0,17,15; mtmsr r4; blr
           0x38600000,0x38630001,                         # row 2: li r3,0; head: addi r3,r3,1
           0x7C032800,0x4180FFF8,0x4E800020]              # row 3: cmpw r3,r5; blt head; blr
    dol=bytearray(0x100+4*len(words))
    for offset,value in [(0,0x100),(0x48,base),(0x90,4*len(words)),(0xe0,base)]:
        struct.pack_into(">I",dol,offset,value)
    struct.pack_into(f">{len(words)}I",dol,0x100,*words)
    image=root/"interrupt-recognition.dol";image.write_bytes(dol)
    functions=root/"interrupt-recognition.csv"
    stream=io.StringIO(newline="")
    writer=csv.writer(stream)
    writer.writerow(["addr_hex","addr","name","size","ins","tags","strings","callees"])
    for address,size,name in [(base,16,"mtmsr"),(base+0x10,8,"loop_head"),(base+0x18,12,"loop_tail")]:
        writer.writerow([hex(address),address,name,size,size//4,"","",""])
    functions.write_text(stream.getvalue(),encoding="utf-8")
    result=subprocess.run([str(generator.resolve()),"--dol",str(image),"--functions",str(functions),
        "--out",str(out),"--shards","1","--no-gap-sweep","--no-wide-entries"],capture_output=True,text=True)
    assert result.returncode==0,result.stdout+result.stderr
    text=(out/"recomp_000.cpp").read_text()
    hot=text.split("static void fn_80003100_body(Cpu& c, u32 entry) {",1)[1].split("static void fn_80003100_body_at(Cpu& c, u32 entry) {",1)[0]
    mtmsr=("    if (c.interrupt_pending()) { c.leave_to_dispatcher(0x80003108u, FallbackReason::Yield, 0x80003108u); return; }\n"
           "    c.set_msr(c.gpr[4]);\n"  # C2 (2026-09-28): MSR writes re-select the host page table
           "    if (c.interrupt_pending()) { c.leave_to_dispatcher(0x8000310Cu, FallbackReason::Yield, 0x80003108u); return; }\n")
    assert mtmsr in hot,"mtmsr does not check for a deliverable interrupt on both sides\n"+hot
    assert "retire_block" not in text and "unretire" not in text,"the instruction clock is emitted again\n"+text
    # D1 (2026-09-22). This used to require the back edge to be a CALL into the
    # other record:
    #
    #   c.pc = ...; if (c.needs_dispatch()) {...} fn_80003110_e80003114(c);
    #
    # That shape was the defect, not the contract. A record is one C++ function
    # and goto is function-scoped, so a loop whose back edge crossed a record
    # boundary became mutual recursion - one host frame per guest iteration,
    # measured to STATUS_STACK_OVERFLOW at ~340,000 iterations of the real
    # fn_803D946C cycle. The loop-extent merge now covers both records with one
    # translation, so the SAME back edge is an in-record goto.
    #
    # What branch-sys-2 actually established is preserved verbatim and is what
    # this asserts: the back edge publishes pc and polls needs_dispatch, so a
    # spinning machine has a guest address to report and a deliverable interrupt
    # is taken once per iteration (recognition point R3). The transfer that
    # follows must NOT be a recursive call.
    back_edge=re.compile(r"c\.pc = 0x80003114u;\s*if \(c\.needs_dispatch\(\)\) \{ c\.leave_to_dispatcher\(0x80003114u, "
                         r"FallbackReason::Yield, 0x8000311Cu\); return; \}\s*goto L_80003114;")
    assert back_edge.search(text),"the backward branch does not publish pc and poll\n"+text
    assert "fn_80003110_e80003114(c);" not in text,\
        "the loop back edge is still a recursive cross-record CALL: a guest loop has O(1)\n"\
        "architected state and must run in O(1) host stack\n"+text
    assert "c.leave_to_dispatcher(0x80003118u, FallbackReason::Yield" not in text,"the end-of-record fall-through must not poll\n"+text

def wait_point_emission(generator,root):
    """Native time (2026-10-05): --wait-points puts the game clock's wait on
    exactly the listed taken back edges - Cpu::idle_wait for a proven polling
    loop, Cpu::poll_point(site) for a loop the runtime checks for a fixed
    point - before the R3 poll, and nowhere else. A stale site (not a backward
    branch to its head) and an unknown class are refused."""
    work=root/"wait-points";work.mkdir()
    base=0x80003100
    words=[0x80030000,0x28000000,0x4182FFF8,0x4E800020,   # poll: lwz r0,0(r3); cmplwi r0,0; beq -8; blr
           0x4BFFFFF1,0x2C030000,0x4182FFF8,0x4E800020]   # progress: bl poll; cmpwi r3,0; beq -8; blr
    dol=bytearray(0x100+4*len(words))
    for offset,value in [(0,0x100),(0x48,base),(0x90,4*len(words)),(0xe0,base)]:
        struct.pack_into(">I",dol,offset,value)
    struct.pack_into(f">{len(words)}I",dol,0x100,*words)
    image=work/"wait.dol";image.write_bytes(dol)
    functions=work/"wait.csv"
    stream=io.StringIO(newline="")
    writer=csv.writer(stream)
    writer.writerow(["addr_hex","addr","name","size","ins","tags","strings","callees"])
    for address,name in [(base,"poll"),(base+0x10,"progress")]:
        writer.writerow([hex(address),address,name,16,4,"","",""])
    functions.write_text(stream.getvalue(),encoding="utf-8")
    def generate(name,listed,ok=True):
        out=work/name;out.mkdir()
        extra=[]
        if listed is not None:
            points=work/(name+".txt");points.write_text("# test\n"+listed,encoding="utf-8")
            extra=["--wait-points",str(points)]
        result=subprocess.run([str(generator.resolve()),"--dol",str(image),"--functions",str(functions),
            "--out",str(out),"--report",str(out/"recomp_report.txt"),"--shards","1","--no-gap-sweep",
            "--no-wide-entries",*extra],capture_output=True,text=True)
        assert (result.returncode==0)==ok,result.stdout+result.stderr
        return out,result
    out,_=generate("listed","0x80003108 0x80003100 poll\n0x80003118 0x80003110 progress\n")
    import re
    text=(out/"recomp_000.cpp").read_text()
    poll=re.compile(r"c\.pc = 0x80003100u;\s*c\.idle_wait\(\);\s*if \(c\.needs_dispatch\(\)\) \{ "
                    r"c\.leave_to_dispatcher\(0x80003100u, FallbackReason::Yield, 0x80003108u\); return; \}")
    assert poll.search(text),"the proven polling loop does not wait before its R3 poll\n"+text
    progress=re.compile(r"c\.pc = 0x80003110u;\s*c\.poll_point\(0x80003118u\);\s*if \(c\.needs_dispatch\(\)\) \{ "
                        r"c\.leave_to_dispatcher\(0x80003110u, FallbackReason::Yield, 0x80003118u\); return; \}")
    assert progress.search(text),"the progress loop does not check for a fixed point before its R3 poll\n"+text
    report=(out/"recomp_report.txt").read_text()
    assert "wait points          : 2 of 2 listed back edges emitted (1 fixed-point checks)" in report,report
    out,_=generate("unlisted",None)
    text=(out/"recomp_000.cpp").read_text()
    assert "c.idle_wait();" not in text and "c.poll_point(" not in text,"a wait without a list\n"+text
    _,result=generate("stale","0x8000310C 0x80003100 poll\n",ok=False)
    assert "is not a backward branch to" in result.stderr,result.stderr
    _,result=generate("bogus","0x80003108 0x80003100 bogus\n",ok=False)
    assert "unknown class" in result.stderr,result.stderr

def platform_site_emission(generator,root):
    """Stage 2 (VI native, 2026-10-06): --platform-sites lowers exactly the
    listed lhz/sth, each pinned by its DOL word, to Cpu::platform_read16 /
    platform_write16 at the effective address the generator computes for it
    (rA = 0 is a literal 0), in every emitted copy (hot and cold body), and
    reports it; unlisted loads and stores keep the bus. A stale list is refused
    before any source is written: a site no emitted function translates (in no
    function, pruned, a native owner), a word the DOL does not hold, an
    instruction that is not a D-form lhz/sth (lwz, lhzu), an unaligned or
    non-text site, a bad kind, a duplicate and a malformed line. Stage 6: the
    kind `dead` (an sth whose register has no consumer) emits the effective
    address and nothing else; a dead read is refused."""
    work=root/"platform-sites";work.mkdir()
    base=0x80003100
    words=[0x3C60CC00,0xA003206E,0xA083206C,0xA0A02000,0x4E800020,  # reads: lis r3,0xCC00; lhz r0,0x206e(r3); lhz r4,0x206c(r3); lhz r5,0x2000(0); blr
           0x38000000,0xB0030002,0xB0830004,0x4E800020,             # writes 0x80003114: li r0,0; sth r0,2(r3); sth r4,4(r3); blr
           0x80630000,0xA4E30002,0x4E800020,                        # other 0x80003124: lwz r3,0(r3); lhzu r7,2(r3); blr
           0xA0C3206E]                                              # 0x80003130 lhz r6,0x206e(r3): in no function
    dol=bytearray(0x100+4*len(words))
    for offset,value in [(0,0x100),(0x48,base),(0x90,4*len(words)),(0xe0,base)]:
        struct.pack_into(">I",dol,offset,value)
    struct.pack_into(f">{len(words)}I",dol,0x100,*words)
    image=work/"platform.dol";image.write_bytes(dol)
    functions=work/"platform.csv"
    stream=io.StringIO(newline="")
    writer=csv.writer(stream)
    writer.writerow(["addr_hex","addr","name","size","ins","tags","strings","callees"])
    for address,size,name in [(base,0x14,"reads"),(base+0x14,0x10,"writes"),(base+0x24,0x0C,"other")]:
        writer.writerow([hex(address),address,name,size,size//4,"","",""])
    functions.write_text(stream.getvalue(),encoding="utf-8")
    roots=work/"roots.txt";roots.write_text("# none: only the entry (reads) is live\n",encoding="utf-8")
    owners=work/"owners.txt";owners.write_text("0x80003114 writes\n",encoding="utf-8")
    def generate(name,listed,extra=(),ok=True):
        out=work/name;out.mkdir()
        options=[]
        if listed is not None:
            sites=work/(name+".txt");sites.write_text("# test\n\n"+listed,encoding="utf-8")
            options=["--platform-sites",str(sites)]
        result=subprocess.run([str(generator.resolve()),"--dol",str(image),"--functions",str(functions),
            "--out",str(out),"--report",str(out/"recomp_report.txt"),"--shards","1","--no-gap-sweep",
            "--no-wide-entries",*options,*extra],capture_output=True,text=True)
        assert (result.returncode==0)==ok,result.stdout+result.stderr
        if not ok:
            written=sorted(p.name for p in out.iterdir() if p.name.startswith("recomp_") and p.name!="recomp_pruned.csv")
            assert not written,"a refused list still wrote generated output: "+str(written)
        return out,result
    listed=("0x80003104 0xA003206E video_encoder_status lhz r0,0x206e(r3), a note: (free text)\n"
            "  0x8000310C 0xA0A02000 video_clock_select lhz r5,0x2000(0): rA = 0 is a literal 0\n"
            "# a comment between sites\n"
            "0x80003118 0xB0030002 video_display_control\n")
    out,_=generate("listed",listed)
    text=(out/"recomp_000.cpp").read_text()
    # The generator's own effective address, then the runtime instead of the
    # bus - in the hot body and in the cold body.
    for lowered in ("    ea = c.gpr[3] + 0x206Eu;\n    c.gpr[0] = static_cast<u32>(c.platform_read16(0x80003104u, ea));\n",
                    "    ea = 0u + 0x2000u;\n    c.gpr[5] = static_cast<u32>(c.platform_read16(0x8000310Cu, ea));\n",
                    "    ea = c.gpr[3] + 0x2u;\n    c.platform_write16(0x80003118u, ea, static_cast<u16>(c.gpr[0]));\n"):
        assert text.count(lowered)==2,"a listed site is not lowered in both bodies:\n"+lowered+"\n"+text
    assert text.count("c.platform_read16(")==4 and text.count("c.platform_write16(")==2,text
    for bus in ("c.gpr[0] = c.mem_read16(ea);","c.gpr[5] = c.mem_read16(ea);",
                "c.mem_write16(ea, static_cast<u16>(c.gpr[0]));"):
        assert bus not in text,"a listed site still reaches the bus: "+bus+"\n"+text
    # Unlisted lhz/sth (and lhzu) are untouched.
    for bus in ("    ea = c.gpr[3] + 0x206Cu;\n    c.gpr[4] = c.mem_read16(ea);\n",
                "    ea = c.gpr[3] + 0x4u;\n    c.mem_write16(ea, static_cast<u16>(c.gpr[4]));\n",
                "    ea = c.gpr[3] + 0x2u;\n    c.gpr[7] = c.mem_read16(ea);\n    c.gpr[3] = ea;\n"):
        assert text.count(bus)==2,"an unlisted access changed:\n"+bus+"\n"+text
    report=(out/"recomp_report.txt").read_text()
    assert "  platform sites       : 3 of 3 listed sites emitted (6 emissions; 0 dead stores emit nothing)\n" in report,report
    # The table the interpreter applies (Cpu::set_platform_site_table): every
    # listed site, sorted, with its dead flag.
    table=(out/"recomp_table.cpp").read_text()
    assert ("const PlatformSiteRecord kPlatformSites[] = {\n"
            "    {0x80003104u, 0xA003206Eu, false},\n"
            "    {0x8000310Cu, 0xA0A02000u, false},\n"
            "    {0x80003118u, 0xB0030002u, false},\n"
            "};\nconst std::size_t kPlatformSiteCount = 3;\n") in table,table
    # A dead store: its effective address, then nothing - no bus, no runtime call.
    out,_=generate("dead","0x8000311C 0xB0830004 dead the register has no consumer\n")
    text=(out/"recomp_000.cpp").read_text()
    assert text.count("    ea = c.gpr[3] + 0x4u;\n")==2,"the dead store's address is not computed in both bodies\n"+text
    assert "c.mem_write16(ea, static_cast<u16>(c.gpr[4]));" not in text,"a dead store still reaches the bus\n"+text
    assert "platform_write16(0x8000311Cu" not in text and "platform_read16" not in text,text
    assert text.count("c.mem_write16(ea, static_cast<u16>(c.gpr[0]));")==2,"the unlisted neighbour changed\n"+text
    report=(out/"recomp_report.txt").read_text()
    assert "  platform sites       : 1 of 1 listed sites emitted (2 emissions; 2 dead stores emit nothing)\n" in report,report
    assert "    {0x8000311Cu, 0xB0830004u, true},\n" in (out/"recomp_table.cpp").read_text(),"the dead flag is in the table"
    # A value site (B2 one time base): a listed li loads the native platform's
    # value instead of its console constant; lis (addis) is never one.
    out,_=generate("value","0x80003114 0x38000000 display_field_us the console field constant\n")
    text=(out/"recomp_000.cpp").read_text()
    # The function's first instruction: only the main body holds it (the
    # mid-entry copy starts at the next instruction).
    assert text.count("    c.gpr[0] = c.platform_value(0x80003114u);\n")==1,"the li is not lowered\n"+text
    assert "c.gpr[0] = 0u + 0x0u;" not in text,"a listed li still loads its constant\n"+text
    assert "    {0x80003114u, 0x38000000u, false},\n" in (out/"recomp_table.cpp").read_text(),"the li is in the table"
    generate("lis","0x80003100 0x3C60CC00 display_field_us\n",ok=False)
    out,_=generate("unlisted",None)
    text=(out/"recomp_000.cpp").read_text()
    assert "platform_read16" not in text and "platform_write16" not in text,"a lowering without a list\n"+text
    assert text.count("c.gpr[0] = c.mem_read16(ea);")==2 and text.count("c.mem_write16(ea, static_cast<u16>(c.gpr[0]));")==2,text
    assert "platform sites" not in (out/"recomp_report.txt").read_text()
    for name,sites,extra,message in (
            ("in-no-function","0x80003130 0xA0C3206E video_encoder_status\n",(),
             ("stale platform site 0x80003130","no emitted function translates it")),
            ("pruned","0x80003118 0xB0030002 video_display_control\n",("--prune-roots",str(roots)),
             ("stale platform site 0x80003118","its function is pruned as unreachable")),
            ("owned","0x80003118 0xB0030002 video_display_control\n",("--native-owners",str(owners)),
             ("stale platform site 0x80003118","its function is a native owner")),
            ("word","0x80003104 0xA003206F video_encoder_status\n",(),
             "platform site 0x80003104: the DOL word is 0xA003206E, the list says 0xA003206F"),
            ("lwz","0x80003124 0x80630000 video_encoder_status\n",(),
             "platform site 0x80003124 holds 0x80630000 (primary opcode 32), not lhz, sth or li"),
            ("lhzu","0x80003128 0xA4E30002 video_encoder_status\n",(),
             "platform site 0x80003128 holds 0xA4E30002 (primary opcode 41), not lhz, sth or li"),
            ("unaligned","0x80003106 0x206EA083 video_encoder_status\n",(),
             "platform site 0x80003106 is not an aligned instruction of the DOL text"),
            ("outside","0x80004000 0xA003206E video_encoder_status\n",(),
             "platform site 0x80004000 is not an aligned instruction of the DOL text"),
            ("kind","0x80003104 0xA003206E Video-Status\n",(),"has an invalid kind 'Video-Status'"),
            ("twice","0x80003104 0xA003206E video_encoder_status\n0x80003104 0xA003206E video_encoder_status\n",(),
             "platform site 0x80003104 is listed twice"),
            ("malformed","0x80003104 0xA003206E\n",(),'expected "<site hex> <instruction word hex> <kind> [note]"'),
            ("dead-read","0x80003104 0xA003206E dead\n",(),
             "platform site 0x80003104 is listed dead but holds 0xA003206E: only a store (sth) can be dead")):
        _,result=generate(name,sites,extra,ok=False)
        for expected in ((message,) if isinstance(message,str) else message):
            assert expected in result.stderr,name+": "+result.stderr

def liveness_dol(root,name,words,data_words,entry=0x80003100):
    """A one-text-section DOL at 0x80003100 plus one data section at 0x80004000."""
    base=0x80003100
    text=4*len(words);data=4*len(data_words)
    dol=bytearray(0x100+text+data)
    for offset,value in [(0x00,0x100),(0x48,base),(0x90,text),
                         (0x1C,0x100+text),(0x64,0x80004000),(0xAC,data),(0xE0,entry)]:
        struct.pack_into(">I",dol,offset,value)
    struct.pack_into(f">{len(words)}I",dol,0x100,*words)
    if data_words:
        struct.pack_into(f">{len(data_words)}I",dol,0x100+text,*data_words)
    image=root/(name+".dol");image.write_bytes(dol)
    return image

def liveness_table(root,name,starts):
    functions=root/(name+".csv")
    stream=io.StringIO(newline="")
    writer=csv.writer(stream)
    writer.writerow(["addr_hex","addr","name","size","ins","tags","strings","callees"])
    for address in starts:
        writer.writerow([hex(address),address,"r_"+hex(address)[2:],8,2,"","",""])
    functions.write_text(stream.getvalue(),encoding="utf-8")
    return functions

def run_generator(generator,image,functions,out,extra,ok=True):
    result=subprocess.run([str(generator.resolve()),"--dol",str(image),"--functions",str(functions),
        "--out",str(out),"--report",str(out/"recomp_report.txt"),"--shards","1","--no-gap-sweep",*extra],
        capture_output=True,text=True)
    assert (result.returncode==0)==ok,result.stdout+result.stderr
    return result

def liveness_words():
    BLR=0x4E800020
    return [0x48000011,BLR,0,0,          # 0x3100 A: bl 0x3110 ; blr   (the entry)
            0x48000041,BLR,0,0,          # 0x3110 B: bl 0x3150 ; blr   (called by A)
            0x38600002,BLR,0,0,          # 0x3120 C: li ; blr          (unreferenced)
            0x38600003,BLR,0,0,          # 0x3130 D: li ; blr          (a data word names it)
            0x38600004,BLR,0,0,          # 0x3140 E: li ; blr          (the roots file names it)
            0x38600005,BLR,0,0]          # 0x3150 F: li ; blr          (called only by B)

def liveness_pruning(generator,root):
    """F: an unreferenced row is pruned; call, data-word and roots-file references keep rows."""
    work=root/"liveness";work.mkdir()
    image=liveness_dol(work,"live",liveness_words(),[0x80003130])
    starts=[0x80003100+16*i for i in range(6)]
    functions=liveness_table(work,"live",starts)
    roots=work/"roots.txt";roots.write_text("# test roots\n0x80003140\n",encoding="utf-8")
    plain=work/"plain";plain.mkdir()
    run_generator(generator,image,functions,plain,[])
    pruned=work/"pruned";pruned.mkdir()
    result=run_generator(generator,image,functions,pruned,["--prune-roots",str(roots)])
    assert "unreachable pruning: 1 rows" in result.stderr,result.stderr
    listed=(pruned/"recomp_pruned.csv").read_text().splitlines()
    assert listed[0]=="address,name,reachable_instructions" and listed[1:]==["0x80003120,r_80003120,2"],listed
    table=(pruned/"recomp_table.cpp").read_text()
    assert "&fn_80003120" not in table,table
    for kept in ("0x80003100","0x80003110","0x80003130","0x80003140","0x80003150"):
        assert "{"+kept+"u," in table,kept
    # Kept rows are emitted exactly as without pruning.
    def bodies(text):
        out={};current=None
        for line in text.splitlines():
            if line.startswith("// 0x"):
                current=line.split()[1];out[current]=[]
            if current: out[current].append(line)
        return out
    a=bodies((plain/"recomp_000.cpp").read_text());b=bodies((pruned/"recomp_000.cpp").read_text())
    assert set(a)-set(b)=={"0x80003120"},sorted(set(a)-set(b))
    for name,lines in b.items():
        assert lines==a[name] or all(x==y or "EndOfFunction" in y for x,y in zip(a[name],lines)),name

def math_lowering_case(generator,root,kind):
    """A native API call removes the software callee and its exclusive child."""
    work=root/kind;work.mkdir()
    image=liveness_dol(work,"math",liveness_words(),[0x80003130])
    functions=liveness_table(work,"math",[0x80003100+16*i for i in range(6)])
    roots=work/"roots.txt";roots.write_text("0x80003140\n",encoding="utf-8")
    sites=work/"sites.txt"
    sites.write_text(f"site 0x80003100 0x48000011 {kind}\n",encoding="utf-8")
    out=work/"out";out.mkdir()
    args=["--prune-roots",str(roots),"--lift-sites",str(sites)]
    run_generator(generator,image,functions,out,args)
    table=(out/"recomp_table.cpp").read_text()
    decls=(out/"recomp_decls.h").read_text()
    shard=(out/"recomp_000.cpp").read_text()
    if kind=="scalar_square_root":
        assert '#include "math/scalar.h"' in table
        assert "shadow::math::square_root(bits_to_double(c.fpr[1].bits[0]))" in table
        assert "c.fpr[1].bits[0] = double_to_bits(result.value);" in table
        assert "if (result.domain_error) c.mem_write32(c.gpr[13] + 0x5E98u, 33u);" in table
    elif kind.startswith("msl_"):
        assert '#include "math/msl.h"' in table
        assert "const double x = bits_to_double(c.fpr[1].bits[0]);" in table
        if kind=="msl_sincosf":
            assert "c.mem_write32(c.gpr[3], float_to_bits(s));" in table
            assert "shadow::math::msl::round_to_single(co, mode)" in table
            assert "c.fpr[1].bits[0] = double_to_bits(co);" in table
        elif kind=="msl_atan2f":
            assert ("c.fpr[1].bits[0] = double_to_bits(double(shadow::math::msl::round_to_single("
                    "shadow::math::msl::atan2(x, bits_to_double(c.fpr[2].bits[0]), mode), mode)));") in table
        else:
            fn=kind[4:-1] if kind.endswith("f") else kind[4:]
            call=f"shadow::math::msl::{fn}(x)"
            if kind.endswith("f"):
                call=f"double(shadow::math::msl::round_to_single({call}, mode))"
            assert f"c.fpr[1].bits[0] = double_to_bits({call});" in table, (kind, call)
    elif kind=="affine_matrix_copy":
        assert '#include "math/matrix.h"' in table
        assert table.count("static GEKKO_NOINLINE void copy_matrix_boundary(Cpu& c)") == 1
        assert "copy_matrix_boundary(c);" in table
        assert table.count("shadow::math::copy_affine_matrix(destination, source)") == 1
        assert "memory.write_in_place(c.gpr[3], shadow::math::kAffineMatrixBytes" in table
        assert 'StopReason::ExecutionFault, "matrix copy requires two complete spans"' in table
    else:
        assert '#include "math/matrix.h"' in table
        assert table.count("static GEKKO_NOINLINE void compose_matrix_boundary(Cpu& c)") == 1
        assert "compose_matrix_boundary(c);" in table
        assert "shadow::math::compose_affine_matrix(values + li, values + ri," in table
        assert "memory.write_in_place(c.gpr[3], bytes" in table
        assert "complete aligned float spans" in table
    if kind in ("affine_matrix_copy", "affine_matrix_compose"):
        operation = "copy" if kind == "affine_matrix_copy" else "compose"
        call = f"call_matrix_{operation}"
        assert f"if ({call}(c, 0x80003100u)) return;" in shard
        assert "lift_site_80003100(c);" not in shard
        assert "c.lift_site_cut(0x80003100u)" not in shard
        assert f"bool {call}(Cpu& c, u32 site);" in decls
        assert table.count(f"GEKKO_NOINLINE bool {call}(Cpu& c, u32 site)") == 1
        body = table.split(f"GEKKO_NOINLINE bool {call}", 1)[1].split("\n}", 1)[0]
        actions = ["if (c.lift_site_cut(site)) return true;", "c.lr = site + 4u;",
                   f"{operation}_matrix_boundary(c);", "c.pc = site + 4u;",
                   "return c.exiting() && c.finish_transfers();"]
        assert all(action in body for action in actions)
        assert [body.index(action) for action in actions] == sorted(body.index(action) for action in actions)
    else:
        assert "lift_site_80003100(c);" in shard and "c.pc = 0x80003104u;" in shard
    for removed in ("fn_80003110", "fn_80003150"):
        assert removed not in decls and removed not in table and removed not in shard
    sites.write_text(f"site 0x80003100 0x48000015 {kind}\n",encoding="utf-8")
    run_generator(generator,image,functions,out,args,False)

def type_query_lowering(generator, root):
    """Reference failures own control; malformed contracts must not emit code."""
    work = root / 'type-query'; work.mkdir()
    entry, target = 0x80003100, 0x803A1AFC
    word = 0x48000001 | ((target-entry) & 0x03FFFFFC)
    image = liveness_dol(work, 'query', [word, 0x4E800020], [])
    functions = precise_table(work, 'query', [(entry, 8)])
    sites = work / 'sites.txt'
    sites.write_text(f'site 0x{entry:X} 0x{word:X} type_query 0x0 0x805E5130\n')
    out = work / 'out'; out.mkdir()
    run_generator(generator, image, functions, out, ['--lift-sites', str(sites)])
    shard = (out / 'recomp_000.cpp').read_text()
    table = (out / 'recomp_table.cpp').read_text()
    assert 'if (call_type_query(c, 0x80003100u, 0u, 0x805E5130u)) return;' in shard
    assert 'fn_803A1AFC' not in shard + table
    assert 'void lift_site_80003100(Cpu& c)' in table
    assert '{0x80003100u, LiftSiteContinuation::Prepared, &lift_site_80003100}' in table
    assert '#include "native_type_boundary.h"' in table
    assert 'recomp::call_indirect(c, c.pc, c.lr);' in table
    assert 'recomp::resume_elsewhere(c, site);' in table
    helper = table.split('GEKKO_NOINLINE bool call_type_query', 1)[1].split('const RecompiledCode::Entry', 1)[0]
    assert 'c.pc = site + 4u;' not in helper
    for i, arguments in enumerate(('', '0x10000 0x805E5130', '0x0 0x805E5131', '0x0 0x805E5130 extra', '-1 0x805E5130')):
        sites.write_text(f'site 0x{entry:X} 0x{word:X} type_query {arguments}\n')
        bad = work / f'bad-{i}'; bad.mkdir()
        result = run_generator(generator, image, functions, bad, ['--lift-sites', str(sites)], ok=False)
        assert 'type_query needs' in result.stderr
        assert not any(bad.iterdir()), 'invalid type contract wrote code'
    sites.write_text(f'site 0x{entry+4:X} 0x4E800020 type_query 0x0 0x805E5130\n')
    bad = work / 'bad-call'; bad.mkdir()
    run_generator(generator, image, functions, bad, ['--lift-sites', str(sites)], ok=False)
    assert not any(bad.iterdir())
    # Pruning must follow the remaining exception dependencies, while the old
    # cast body disappears. No data roots or explicit roots retain these rows.
    rows = [(entry, [word, 0x4E800020]), (target, [0x4E800020]),
            (0x803A1D60, [0x4E800020]), (0x803A2660, [0x4E800020])]
    dol = bytearray(256 + sum(4*len(words) for _, words in rows))
    offset = 256
    for i, (address, words) in enumerate(rows):
        struct.pack_into('>I', dol, i*4, offset)
        struct.pack_into('>I', dol, 0x48+i*4, address)
        struct.pack_into('>I', dol, 0x90+i*4, 4*len(words))
        struct.pack_into(f'>{len(words)}I', dol, offset, *words)
        offset += 4*len(words)
    struct.pack_into('>I', dol, 0xE0, entry)
    image = work / 'retirement.dol'; image.write_bytes(dol)
    functions = precise_table(work, 'retirement', [(at, 4*len(words)) for at, words in rows])
    sites.write_text(f'site 0x{entry:X} 0x{word:X} type_query 0x0 0x805E5130\n')
    roots = work / 'roots.txt'; roots.write_text('# No extra roots.\n')
    retired = work / 'retired'; retired.mkdir()
    run_generator(generator, image, functions, retired, ['--lift-sites', str(sites), '--prune-roots', str(roots)])
    compiled = (retired / 'recomp_000.cpp').read_text()
    assert 'fn_803A1AFC' not in compiled
    assert 'fn_803A2660' in compiled and 'fn_803A1D60' in compiled


def native_owner_emission(generator,root):
    """G: an owner is exactly its thunk, its exclusive callee is pruned, interior entry fails."""
    work=root/"owners";work.mkdir()
    image=liveness_dol(work,"own",liveness_words(),[0x80003130])
    starts=[0x80003100+16*i for i in range(6)]
    functions=liveness_table(work,"own",starts)
    roots=work/"roots.txt";roots.write_text("0x80003140\n",encoding="utf-8")
    owners=work/"owners.txt";owners.write_text("# owner\n0x80003110 B\n",encoding="utf-8")
    out=work/"out";out.mkdir()
    result=run_generator(generator,image,functions,out,["--prune-roots",str(roots),"--native-owners",str(owners)])
    assert "native owners: 1 rows emit only a thunk" in result.stderr,result.stderr
    shard=(out/"recomp_000.cpp").read_text()
    assert "void fn_80003110(Cpu& c) { c.serve_native_owner(0x80003110u); }" in shard,shard
    assert "fn_80003110_body" not in shard and "fn_80003110_at" not in shard
    listed=[line.split(",")[0] for line in (out/"recomp_pruned.csv").read_text().splitlines()[1:]]
    assert sorted(listed)==["0x80003120","0x80003150"],listed   # F was reachable only through B's body
    table=(out/"recomp_table.cpp").read_text()
    assert "const u32 kNativeOwners[] = {0x80003110u};" in table and "kNativeOwnerCount = 1;" in table,table
    assert "fn_80003110_at" not in table
    # A live row branching into an owner's interior refuses generation.
    words=liveness_words();words[0]=0x48000015        # A: bl 0x3114 (B's blr, an interior address)
    image=liveness_dol(work,"interior",words,[0x80003130])
    bad=work/"bad";bad.mkdir()
    result=run_generator(generator,image,functions,bad,["--prune-roots",str(roots),"--native-owners",str(owners)],ok=False)
    assert "branches into the interior of a native owner" in result.stderr,result.stderr
    # Declared as an owner ENTRY, the same address gets a thunk and a table row, and the branch is allowed.
    owners2=work/"owners2.txt";owners2.write_text("0x80003110 B\n0x80003114 entry\n",encoding="utf-8")
    good=work/"entry";good.mkdir()
    run_generator(generator,image,functions,good,["--prune-roots",str(roots),"--native-owners",str(owners2)])
    shard=(good/"recomp_000.cpp").read_text()
    assert "void fn_80003110_e80003114(Cpu& c) { c.serve_native_owner(0x80003114u); }" in shard,shard
    table=(good/"recomp_table.cpp").read_text()
    assert "{0x80003114u, 4u, &fn_80003110_e80003114" in table and "kNativeOwnerCount = 2;" in table,table
    # An owner that is not a row start is refused.
    wrong=work/"wrong.txt";wrong.write_text("0x80003114\n",encoding="utf-8")
    bad2=work/"bad2";bad2.mkdir()
    result=run_generator(generator,liveness_dol(work,"own2",liveness_words(),[0x80003130]),functions,bad2,
                         ["--native-owners",str(wrong)],ok=False)
    assert "is not the start of an accepted row" in result.stderr,result.stderr

def precise_dol(root,name,text_words,data_words,data_base=0x80010000,entry=0x80003100):
    """A text section at 0x80003100 and a data section at data_base."""
    text=4*len(text_words);data=4*len(data_words)
    dol=bytearray(0x100+text+data)
    for offset,value in [(0x00,0x100),(0x48,0x80003100),(0x90,text),
                         (0x1C,0x100+text),(0x64,data_base),(0xAC,data),(0xE0,entry)]:
        struct.pack_into(">I",dol,offset,value)
    struct.pack_into(f">{len(text_words)}I",dol,0x100,*text_words)
    if data_words: struct.pack_into(f">{len(data_words)}I",dol,0x100+text,*data_words)
    image=root/(name+".dol");image.write_bytes(dol)
    return image

def precise_table(root,name,rows):
    functions=root/(name+".csv")
    stream=io.StringIO(newline="")
    writer=csv.writer(stream)
    writer.writerow(["addr_hex","addr","name","size","ins","tags","strings","callees"])
    for address,size in rows:
        writer.writerow([hex(address),address,"r_"+hex(address)[2:],size,size//4,"","",""])
    functions.write_text(stream.getvalue(),encoding="utf-8")
    return functions

def pruned_rows(out):
    return sorted(line.split(",")[0] for line in (out/"recomp_pruned.csv").read_text().splitlines()[1:])

def precise_liveness(generator,root):
    """H: jump-table words are dispatch edges, census roots follow their constructors, hi/lo pairs form what they compute and only through a register (H5)."""
    BLR=0x4E800020;LI=0x38600001
    work=root/"precise";work.mkdir()
    # --- H1: a switch table at 0x80010000 = [C1, C2], dispatched by S's bctr at 0x80003140.
    S=[0x3C808001,0x38840000,0x7C04182E,0x7C0903A6,0x4E800420,0,0,0]   # lis/addi r4=table; lwzx; mtctr; bctr
    X=[0x3CA08001,0x38A50004,BLR,0]                                     # forms table+4
    def jt_words(a): return a+[LI,BLR,0,0]+[LI,BLR,0,0]+S+X               # A 3100 C1 3110 C2 3120 S 3130 X 3150
    rows=[(0x80003100,16),(0x80003110,8),(0x80003120,8),(0x80003130,20),(0x80003150,12)]
    functions=precise_table(work,"jt",rows)
    tables=work/"tables.txt";tables.write_text("# test\n0x80003140 0x80010000 2\n",encoding="utf-8")
    roots=work/"roots.txt";roots.write_text("# none\n",encoding="utf-8")
    def jt(name,a,data,extra):
        out=work/name;out.mkdir()
        result=run_generator(generator,precise_dol(work,name,jt_words(a),data),functions,out,["--prune-roots",str(roots),*extra])
        return pruned_rows(out),result.stderr
    dead=[BLR,0,0,0]
    listed,_=jt("jt-rooted",dead,[0x80003110,0x80003120],[])
    assert listed==["0x80003130","0x80003150"],listed                    # table words are roots without the file
    listed,err=jt("jt-dead",dead,[0x80003110,0x80003120],["--jump-tables",str(tables)])
    assert listed==["0x80003110","0x80003120","0x80003130","0x80003150"],listed
    assert "1 jump tables verified as dispatch edges, 0 left rooted" in err,err
    listed,_=jt("jt-live",[0x48000031,BLR,0,0],[0x80003110,0x80003120],["--jump-tables",str(tables)])   # A: bl S
    assert listed==["0x80003150"],listed                                 # S live: its cases follow
    listed,err=jt("jt-aliased",[0x48000051,BLR,0,0],[0x80003110,0x80003120],["--jump-tables",str(tables)])  # A: bl X
    assert listed==["0x80003130"] and "1 jump tables verified as dispatch edges" in err,(listed,err)  # X may read it
    listed,err=jt("jt-pointed",dead,[0x80003110,0x80003120,0x80010004],["--jump-tables",str(tables)])
    assert listed==["0x80003130","0x80003150"] and "0 jump tables verified as dispatch edges, 1 left rooted" in err,(listed,err)
    # --- H2: census roots. E forms D; G holds F's recorded construction site; H has neither.
    E=[0x3C608000,0x38633110,BLR,0]                                      # forms D = 0x80003110
    words=[0,BLR,0,0]+[LI,BLR,0,0]+E+[LI,BLR,0,0]+[LI,BLR,0,0]+[LI,BLR,0,0]
    rows=[(0x80003100,16),(0x80003110,8),(0x80003120,12),(0x80003130,8),(0x80003140,8),(0x80003150,8)]
    functions=precise_table(work,"census",rows)
    census=work/"census-roots.txt"
    census.write_text("0x80003110 census\n0x80003130 census 0x80003140\n0x80003150 census\n",encoding="utf-8")
    def census_case(name,a):
        out=work/name;out.mkdir()
        w=list(words);w[0]=a
        result=run_generator(generator,precise_dol(work,name,w,[]),functions,out,["--prune-roots",str(census)])
        return pruned_rows(out),result.stderr
    listed,err=census_case("census-dead",0x60000000)                    # A: nop ; blr
    assert listed==["0x80003110","0x80003120","0x80003130","0x80003140"],listed   # H (nothing forms it) stays
    assert "2 census roots conditional on their constructors, 1 unconditional" in err,err
    listed,_=census_case("census-e",0x48000021)                          # A: bl E -> D
    assert listed==["0x80003130","0x80003140"],listed
    listed,_=census_case("census-g",0x48000041)                          # A: bl G -> F
    assert listed==["0x80003110","0x80003120"],listed
    # --- H4: I forms an odd value inside J; I2 is lis+addi whose OR would be K; I3 is lis+ori forming K.
    K=0x80008000
    I=[0x3C608000,0x386331C1,BLR,0]                                      # 0x800031C1: not a code pointer
    I2=[0x3C608000,0x38638000,BLR,0]                                     # adds to 0x7FFF8000; only an OR would give K
    I3=[0x3C608000,0x60638000,BLR,0]                                     # ors to 0x80008000
    def h4_words(a):
        w=[0]*((K+8-0x80003100)//4)
        for address,code in ((0x80003100,a),(0x800031B0,I),(0x800031C0,[LI,BLR,0,0]),(0x800031D0,I2),(0x800031E0,I3),(K,[LI,BLR])):
            w[(address-0x80003100)//4:(address-0x80003100)//4+len(code)]=code
        return w
    rows=[(0x80003100,16),(0x800031B0,12),(0x800031C0,8),(0x800031D0,12),(0x800031E0,12),(K,8)]
    functions=precise_table(work,"h4",rows)
    def h4(name,a):
        out=work/name;out.mkdir()
        run_generator(generator,precise_dol(work,name,h4_words(a),[]),functions,out,["--prune-roots",str(roots)])
        return pruned_rows(out)
    # A: bl I ; bl I2 ; blr
    assert h4("h4-add",[0x480000B1,0x480000CD,BLR,0])==["0x800031C0","0x800031E0","0x80008000"]
    # A: bl I3 ; blr  - the ori forms K
    assert h4("h4-or",[0x480000E1,BLR,0,0])==["0x800031B0","0x800031C0","0x800031D0"]
    # --- H5: the halves must meet through a register. T = 0x80003300.
    D=[0x3C608000,0x38853300,BLR,0]   # lis r3 ; addi r4,r5,0x3300  - disjoint: forms nothing
    M=[0x3C608000,0x38833300,BLR,0]   # lis r3 ; addi r4,r3,0x3300  - meets: forms T
    L=[0x38603300,0x64638000,BLR,0]   # li r3,0x3300 ; oris r3,r3,0x8000 - li-then-oris: forms T
    N=[0x38803300,0x64A38000,BLR,0]   # li r4 ; oris r3,r5,0x8000   - disjoint: forms nothing
    def h5_words(a):
        w=[0]*((0x80003308-0x80003100)//4)
        for address,code in ((0x80003100,a),(0x80003200,D),(0x80003210,M),(0x80003220,L),(0x80003230,N),
                             (0x80003300,[LI,BLR])):
            w[(address-0x80003100)//4:(address-0x80003100)//4+len(code)]=code
        return w
    rows=[(0x80003100,16),(0x80003200,12),(0x80003210,12),(0x80003220,12),(0x80003230,12),(0x80003300,8)]
    functions=precise_table(work,"h5",rows)
    def h5(name,a):
        out=work/name;out.mkdir()
        run_generator(generator,precise_dol(work,name,h5_words(a),[]),functions,out,["--prune-roots",str(roots)])
        return pruned_rows(out)
    assert h5("h5-disjoint",[0x48000101,BLR,0,0])==["0x80003210","0x80003220","0x80003230","0x80003300"]
    assert h5("h5-meets",[0x48000111,BLR,0,0])==["0x80003200","0x80003220","0x80003230"]
    assert h5("h5-li-oris",[0x48000121,BLR,0,0])==["0x80003200","0x80003210","0x80003230"]
    assert h5("h5-disjoint-oris",[0x48000131,BLR,0,0])==["0x80003200","0x80003210","0x80003220","0x80003300"]

def hot_set_emission(generator,root):
    """R13: --hot-set retargets the accessor calls of exactly the listed rows' HOT bodies to
    Cpu::mem_*_hot; their cold trampolines and every other row are unchanged, an empty list is
    byte-identical to no option, and a stale or malformed list is refused before writing."""
    work=root/"hot-set";work.mkdir()
    BLR=0x4E800020
    words=[0x80830000,0x90830004,0x88A30008,0xB0A3000A,BLR,0,0,0,   # 0x3100 A: lwz;stw;lbz;sth;blr
           0x80830000,BLR,0,0]                                      # 0x3120 B: lwz;blr
    image=liveness_dol(work,"hot",words,[])
    functions=work/"hot.csv"
    stream=io.StringIO(newline="")
    writer=csv.writer(stream)
    writer.writerow(["addr_hex","addr","name","size","ins","tags","strings","callees"])
    for address,size,name in [(0x80003100,20,"A"),(0x80003120,8,"B")]:
        writer.writerow([hex(address),address,name,size,size//4,"","",""])
    functions.write_text(stream.getvalue(),encoding="utf-8")
    def bodies(text):
        out={};current=None
        for line in text.splitlines():
            if line.startswith("static void fn_") or line.startswith("void fn_"):
                current=line.split("(")[0].split()[-1];out[current]=[]
            if current: out[current].append(line)
        return {k:"\n".join(v) for k,v in out.items()}
    plain=work/"plain";plain.mkdir()
    run_generator(generator,image,functions,plain,[])
    empty_list=work/"empty.csv";empty_list.write_text("ppcrecomp-hotset-v1\n",encoding="utf-8")
    empty=work/"empty";empty.mkdir()
    run_generator(generator,image,functions,empty,["--hot-set",str(empty_list)])
    for p in plain.iterdir():
        if p.name!="recomp_report.txt":
            assert p.read_bytes()==(empty/p.name).read_bytes(),"an empty hot set changed "+p.name
    listed=work/"listed.csv";listed.write_text("ppcrecomp-hotset-v1\n# test\n0x80003100,17\n",encoding="utf-8")
    hot=work/"hot";hot.mkdir()
    run_generator(generator,image,functions,hot,["--hot-set",str(listed)])
    a=bodies((plain/"recomp_000.cpp").read_text());b=bodies((hot/"recomp_000.cpp").read_text())
    assert set(a)==set(b),(sorted(a),sorted(b))
    hot_body=b["fn_80003100_body"]
    for call in ("c.mem_read32_hot(","c.mem_write32_hot(","c.mem_read8_hot(","c.mem_write16_hot("):
        assert call in hot_body,call+"\n"+hot_body
    assert hot_body==a["fn_80003100_body"].replace("c.mem_read32(","c.mem_read32_hot(").replace(
        "c.mem_write32(","c.mem_write32_hot(").replace("c.mem_read8(","c.mem_read8_hot(").replace(
        "c.mem_write16(","c.mem_write16_hot("),hot_body
    for name,text in b.items():
        if name!="fn_80003100_body":
            assert text==a[name] and "_hot(" not in text,name+"\n"+text
    # The cold trampoline keeps the leaves (its mid-block snippets: stw, lbz, sth; the entry lwz joins hot).
    assert "c.mem_write32(" in b["fn_80003100_body_at"],b["fn_80003100_body_at"]
    report=(hot/"recomp_report.txt").read_text()
    assert "hot-set rows         : 1 of 1 listed (4 accessor sites inlined)" in report,report
    for name,body in [("noheader","0x80003100,1\n"),("interior","ppcrecomp-hotset-v1\n0x80003104,1\n"),
                      ("unknown","ppcrecomp-hotset-v1\n0x80009000,1\n"),("noweight","ppcrecomp-hotset-v1\n0x80003100\n"),
                      ("twice","ppcrecomp-hotset-v1\n0x80003100,1\n0x80003100,2\n")]:
        bad_list=work/(name+".csv");bad_list.write_text(body,encoding="utf-8")
        bad=work/("bad-"+name);bad.mkdir()
        result=run_generator(generator,image,functions,bad,["--hot-set",str(bad_list)],ok=False)
        assert "hot set" in result.stderr,result.stderr
        assert not any(bad.iterdir()),name+": a refused hot set wrote output"

def psq_site_emission(generator,root):
    """R16: --psq-sites turns exactly the listed psq_l / psq_st sites into an exact GQR guard around the
    specialised helper, with the generic helper as the else branch; unlisted sites are unchanged; a list naming a
    non-psq word, the wrong GQR index or no K is refused before writing."""
    work=root/"psq-sites";work.mkdir()
    BLR=0x4E800020
    PSQ_L,PSQ_ST=0xE0234000,0xF0234000          # psq_l / psq_st f1, 0(r3), W=0, I=4
    words=[PSQ_L,PSQ_ST,BLR,0,                  # 0x3100 A: psq_l; psq_st; blr
           PSQ_ST,BLR,0,0]                      # 0x3110 B: psq_st; blr (unlisted)
    image=liveness_dol(work,"psq",words,[])
    functions=work/"psq.csv"
    stream=io.StringIO(newline="")
    writer=csv.writer(stream)
    writer.writerow(["addr_hex","addr","name","size","ins","tags","strings","callees"])
    for address,size,name in [(0x80003100,12,"A"),(0x80003110,8,"B")]:
        writer.writerow([hex(address),address,name,size,size//4,"","",""])
    functions.write_text(stream.getvalue(),encoding="utf-8")
    plain=work/"plain";plain.mkdir()
    run_generator(generator,image,functions,plain,[])
    listed=work/"listed.txt";listed.write_text("# test\n0x80003100 4 0x00040004\n0x80003104 4 0x00040004\n",encoding="utf-8")
    spec=work/"spec";spec.mkdir()
    result=run_generator(generator,image,functions,spec,["--psq-sites",str(listed)])
    assert "psq sites: 2 of 2 listed sites specialised" in result.stderr,result.stderr
    a=(plain/"recomp_000.cpp").read_text();b=(spec/"recomp_000.cpp").read_text()
    for kind in ("load","store"):
        guarded=(f"if (c.spr[916u] == 0x40004u) ppc::psq_{kind}_k<0x40004u>(c, 1u, ea, false, 4u); "
                 f"else ppc::psq_{kind}(c, 1u, ea, false, 4u);")
        assert guarded in b,guarded+"\n"+b
        assert f"    ppc::psq_{kind}(c, 1u, ea, false, 4u);" in a
    # B's site stays the plain call, and nothing but the listed lines changed.
    assert b.replace("if (c.spr[916u] == 0x40004u) ppc::psq_load_k<0x40004u>(c, 1u, ea, false, 4u); else ","").replace(
        "if (c.spr[916u] == 0x40004u) ppc::psq_store_k<0x40004u>(c, 1u, ea, false, 4u); else ","")==a,b
    for name,body in [("notpsq","0x80003108 4 0x00040004\n"),("wronggqr","0x80003100 3 0x00040004\n"),
                      ("nok","0x80003100 4\n"),("twice","0x80003100 4 0x1\n0x80003100 4 0x2\n")]:
        bad_list=work/(name+".txt");bad_list.write_text(body,encoding="utf-8")
        bad=work/("bad-"+name);bad.mkdir()
        result=run_generator(generator,image,functions,bad,["--psq-sites",str(bad_list)],ok=False)
        assert "psq sites" in result.stderr,result.stderr
        assert not any(bad.iterdir()),name+": a refused psq list wrote output"

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument("--generator",type=Path,required=True)
    parser.add_argument("--work",type=Path,required=True)
    args=parser.parse_args()
    args.work.mkdir(parents=True,exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="recomp-output-",dir=args.work) as temporary:
        root=Path(temporary).resolve()
        root.relative_to(args.work.resolve())  # cleanup is confined to this test directory
        out=root/"generated";out.mkdir()
        dol=bytearray(0x180)
        for offset,value in [(0,0x100),(0x48,0x80003100),(0x90,0x80),(0xe0,0x80003100)]:
            struct.pack_into(">I",dol,offset,value)
        for i in range(8):
            struct.pack_into(">II",dol,0x100+i*16,0x38600000+i,0x4e800020)
        image=root/"test.dol";image.write_bytes(dol)
        functions=root/"functions.csv"
        def table(indices):
            stream=io.StringIO(newline="")
            writer=csv.writer(stream)
            writer.writerow(["addr_hex","addr","name","size","ins","tags","strings","callees"])
            for i in indices:
                address=0x80003100+i*16
                writer.writerow([hex(address),address,"test_"+str(i),8,2,"","",""])
            functions.write_text(stream.getvalue(),encoding="utf-8")
        def run(ok=True):
            result=subprocess.run([str(args.generator.resolve()),"--dol",str(image),
                "--functions",str(functions),"--out",str(out),"--report",str(out/"recomp_report.txt"),
                "--shards","2","--no-gap-sweep","--no-wide-entries"],
                capture_output=True,text=True)
            assert (result.returncode==0)==ok,result.stdout+result.stderr
            return result
        def snapshot():
            return {p.name:(hashlib.sha256(p.read_bytes()).hexdigest(),p.stat().st_mtime_ns)
                    for p in out.iterdir() if p.is_file()}
        def layout():
            return dict(csv.reader((out/"recomp_layout.csv").read_text().splitlines()[1:]))
        table([1,3,5,7]);run()
        initial=snapshot();placement=layout()
        repeated=run()
        assert snapshot()==initial,"identical generation rewrote content or timestamps"
        assert "0 written" in repeated.stderr,repeated.stderr
        table([0,1,3,5,7]);run()
        inserted=snapshot();new=layout()
        assert all(new[a]==shard for a,shard in placement.items()),"insertion moved an existing guest identity"
        changed=[name for name in ("recomp_000.cpp","recomp_001.cpp") if inserted[name][0]!=initial[name][0]]
        assert len(changed)==1,changed
        table([0,1,5,7]);run()
        reduced=layout()
        assert all(new[a]==shard for a,shard in reduced.items()),"removal moved a surviving guest identity"
        # Invalid ownership metadata must fail before rewriting any source.
        mapfile=out/"recomp_layout.csv"
        good=mapfile.read_bytes()
        mapfile.write_bytes(good+good.splitlines(keepends=True)[1])
        invalid=snapshot();run(False)
        assert snapshot()==invalid,"invalid layout changed generated output"
        mapfile.write_bytes(good)
        # Missing output is repaired even though the rest is unchanged.
        missing=out/"recomp_001.cpp"
        expected=missing.read_bytes();missing.unlink();run()
        assert missing.read_bytes()==expected
        gap_sweep_roots(args.generator,root)
        bctr_loop_lowering(args.generator,root)
        shared_indirect_lowering(args.generator,root)
        interrupt_recognition_lowering(args.generator,root)
        wait_point_emission(args.generator,root)
        platform_site_emission(args.generator,root)
        liveness_pruning(args.generator,root)
        native_owner_emission(args.generator,root)
        for kind in ("scalar_square_root", "affine_matrix_copy", "affine_matrix_compose", "msl_sin", "msl_cosf",
                     "msl_atan2f", "msl_sincosf"):
            math_lowering_case(args.generator,root,kind)
        type_query_lowering(args.generator,root)
        from test_empty_variadic_generator import check as check_empty_variadic
        check_empty_variadic(args.generator,root)
        precise_liveness(args.generator,root)
        hot_set_emission(args.generator,root)
        psq_site_emission(args.generator,root)
        print("PASS: byte/timestamp stability, stable weighted insertion/removal, invalid-layout rejection, missing-output repair, unsupported gap-sweep roots dropped, local bctr lowered to gotos, interrupt recognition at mtmsr and cross-record back edges, wait points on exactly the listed back edges (stale/unknown refused), platform sites lowered on exactly the listed lhz/sth in every copy (stale/mismatched/non-lhz-sth/invalid lists refused before writing), unreachable rows pruned with call/data/roots references kept, native owners emitted as thunks with interior entry refused, jump-table words as dispatch edges (aliased/pointed tables rooted), census roots through their constructors, hi/lo pairs forming only what they compute and only through a register, hot-set accessor inlining in exactly the listed hot bodies (empty list byte-identical, stale/malformed lists refused before writing), psq sites behind an exact GQR guard on exactly the listed sites (non-psq / wrong-GQR / incomplete lists refused)")

if __name__=="__main__":
    main()
