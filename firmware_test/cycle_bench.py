#!/usr/bin/env python3
"""
OpenEar cycle bench: how long does one audio block take on the Cortex-M4F?

Compiles the firmware's real DSP (firmware/dsp/openear_dsp.c) and the audio
interrupt's sample conversion (firmware/audio_convert.h) with the firmware's
own compiler flags, runs one block in the Unicorn Cortex-M4 emulator, counts
every instruction executed and converts them to cycles with the Cortex-M4
timing table (ARM DDI 0439B ch. 3, FPv4 ch. 7). That cycle figure is an
ESTIMATE (no wait states, no cache misses, no interrupts): treat it as
+-25 %. The real number on the board is printed by 'stat'.

Fails if a block would take more than LIMIT_PCT of its real-time budget at
100 MHz, leaving room for the estimate's error and for Bluetooth interrupts.

Needs: arm-none-eabi-gcc on PATH (or under ~/.local/xPacks), pip install
unicorn capstone.   Run: python3 firmware_test/cycle_bench.py [--baseline]
"""
import glob, os, subprocess, sys, tempfile, collections
from unicorn import Uc, UcError, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS, UC_HOOK_CODE, UC_HOOK_MEM_WRITE
from unicorn.arm_const import UC_ARM_REG_SP, UC_CPU_ARM_CORTEX_M4
from capstone import Cs, CS_ARCH_ARM, CS_MODE_THUMB, CS_MODE_MCLASS

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
FW = os.path.join(ROOT, "firmware")
LIMIT_PCT = 45.0
CLOCK_HZ = 100e6
FLAGS = ["-mcpu=cortex-m4", "-mthumb", "-mfloat-abi=softfp", "-mfpu=fpv4-sp-d16", "-O2",
         "-fsingle-precision-constant", "-ffp-contract=off", "-ffunction-sections", "-fdata-sections",
         "-fno-isolate-erroneous-paths-dereference", "-Wall", "-Werror"]

def find_gcc():
    for d in [""] + glob.glob(os.path.expanduser("~/.local/xPacks/@xpack-dev-tools/arm-none-eabi-gcc/*/.content/bin/")):
        exe = os.path.join(d, "arm-none-eabi-gcc") if d else "arm-none-eabi-gcc"
        try:
            subprocess.run([exe, "--version"], capture_output=True, check=True)
            return exe
        except (OSError, subprocess.CalledProcessError):
            pass
    sys.exit("arm-none-eabi-gcc not found")

def tool(gcc, name):
    d, b = os.path.split(gcc)
    return os.path.join(d, b[: -len("gcc")] + name)

def build(gcc, block, baseline, out):
    dsp = [os.path.join(ROOT, "dsp_test", "reference", "ref_dsp.c")] if baseline else \
          [os.path.join(FW, "dsp", "openear_dsp.c"), os.path.join(FW, "recorder.c")]
    inc = ["-I" + os.path.join(HERE, "stubs"), "-I" + os.path.join(FW, "dsp"), "-I" + FW,
           "-I" + os.path.join(ROOT, "dsp_test", "reference"), "-DOE_REC=1"]
    defs = ["-DN=%d" % block] + (["-DUSE_REF"] if baseline else [])
    elf = os.path.join(out, "bench.elf")
    # the shipped 0.2.2 was built with GCC's default fused multiply-add
    flags = [f for f in FLAGS if not (baseline and f == "-ffp-contract=off")]
    subprocess.run([gcc] + flags + inc + defs + ["--specs=nano.specs", "-nostartfiles", "-T", os.path.join(HERE, "cycle_bench", "link.ld"),
                    os.path.join(HERE, "cycle_bench", "bench.c")] + dsp + ["-lm", "-lc", "-o", elf], check=True)
    subprocess.run([tool(gcc, "objcopy"), "-O", "binary", elf, elf + ".bin"], check=True)
    nm = subprocess.run([tool(gcc, "nm"), elf], capture_output=True, text=True, check=True).stdout
    syms = {l.split()[2]: int(l.split()[0], 16) for l in nm.splitlines() if len(l.split()) == 3}
    return elf + ".bin", syms

BRANCHES = {"b", "bl", "blx", "bx", "cbz", "cbnz", "beq", "bne", "bcs", "bcc", "bhs", "blo", "bmi", "bpl",
            "bvs", "bvc", "bhi", "bls", "bge", "blt", "bgt", "ble", "bal"}
LOADS = {"ldr", "ldrh", "ldrsh", "ldrb", "ldrsb", "vldr"}

def cost(m, ops):
    """Cortex-M4 TRM (ARM DDI 0439B) cycle counts; branches and load pipelining
    are handled in run() (they depend on the previous instruction)."""
    m = m.split(".")[0]
    regs = ops.count(",") + 1
    if m in ("vmla", "vmls", "vfma", "vfms", "vnmla", "vnmls"): return 3
    if m in ("vdiv", "vsqrt"): return 14
    if m in ("vldr", "vstr"): return 2
    if m in ("vpush", "vpop", "vldmia", "vstmia", "vldmdb", "vstmdb", "vldm", "vstm"): return 1 + regs
    if m.startswith("v"): return 1
    if m in ("ldr", "ldrh", "ldrsh", "ldrb", "ldrsb", "str", "strh", "strb"): return 2
    if m in ("ldrd", "strd"): return 3
    if m in ("push", "pop", "ldm", "stm", "ldmia", "stmia", "stmdb", "ldmdb"): return 1 + regs
    if m in ("udiv", "sdiv"): return 6
    if m in ("mla", "mls", "smlal", "umlal"): return 2
    return 1  # ALU, mul, umull/smull (1), not-taken branches

def run(binf, syms):
    mu = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
    mu.ctl_set_cpu_model(UC_CPU_ARM_CORTEX_M4)
    mu.mem_map(0x20000000, 2 * 1024 * 1024)
    mu.mem_write(0x20000000, open(binf, "rb").read())
    mu.reg_write(UC_ARM_REG_SP, 0x20000000 + 2 * 1024 * 1024 - 16)
    md = Cs(CS_ARCH_ARM, CS_MODE_THUMB | CS_MODE_MCLASS)
    cache, st, res = {}, {"on": False, "ins": 0, "cyc": 0, "prev": None}, {}
    def hc(uc, addr, size, _):
        if not st["on"]:
            st["prev"] = None
            return
        k = (addr, size)
        if k not in cache:
            i = next(md.disasm(bytes(uc.mem_read(addr, size)), addr), None)
            m = i.mnemonic.split(".")[0] if i else ""
            cache[k] = (cost(i.mnemonic, i.op_str) if i else 1, m in BRANCHES, m in LOADS)
        c, is_br, is_ld = cache[k]
        prev = st["prev"]
        if prev:
            p_addr, p_size, p_br, p_ld = prev
            if p_br and addr != p_addr + p_size:
                st["cyc"] += 2          # taken branch: pipeline refill (1 + P, P = 1..3)
            if p_ld and is_ld:
                c -= 1                  # back-to-back loads pipeline (N + 1)
        st["cyc"] += c
        st["ins"] += 1
        st["prev"] = (addr, size, is_br, is_ld)
    def hm(uc, access, addr, size, value, _):
        if value in (1, 3, 5):
            st.update(on=True, ins=0, cyc=0)
        elif value in (2, 4, 6):
            res[value] = (st["ins"], st["cyc"])
            st["on"] = False
    mu.hook_add(UC_HOOK_CODE, hc)
    mu.hook_add(UC_HOOK_MEM_WRITE, hm, begin=syms["marker"], end=syms["marker"] + 3)
    try:
        mu.emu_start(syms["bench_entry"] | 1, 0, count=80_000_000)
    except UcError:
        pass  # the bkpt at the end
    return res

def main():
    baseline = "--baseline" in sys.argv
    gcc = find_gcc()
    fails = 0
    print("OpenEar cycle bench (%s DSP), Cortex-M4F @ %.0f MHz, estimate +-25%%" %
          ("frozen 0.2.2" if baseline else "firmware", CLOCK_HZ / 1e6))
    for block in (64, 128):
        with tempfile.TemporaryDirectory() as d:
            binf, syms = build(gcc, block, baseline, d)
            res = run(binf, syms)
        budget = CLOCK_HZ * block / 48000.0
        keys = ((2, "mic block (fitted profile)"), (4, "hearing-test tone block"))
        if not baseline:
            keys += ((6, "mic block while recording"),)
        for key, name in keys:
            if key not in res:
                print("  FAIL  block %d %s: bench did not finish" % (block, name)); fails += 1; continue
            ins, cyc = res[key]
            pct = 100.0 * cyc / budget
            ok = pct <= LIMIT_PCT
            fails += not ok
            print("  %s  block %3d %-27s %6d instructions, ~%6d cycles = %4.1f%% of the block (limit %.0f%%)" %
                  ("PASS" if ok else "FAIL", block, name, ins, cyc, pct, LIMIT_PCT))
    print("\n%s" % ("ALL CYCLE CHECKS PASSED" if not fails else "%d CYCLE CHECK(S) FAILED" % fails))
    return 1 if fails else 0

if __name__ == "__main__":
    sys.exit(main())
