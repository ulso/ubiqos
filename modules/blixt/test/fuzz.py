#!/usr/bin/env python3
"""Random programs through Blixt and GNU as, compared byte for byte.

Branches forward and back, adr, ldr from labels and pools, alignments, data
and .ltorg in random order: what exercises the passes. Run test/run.sh first,
which builds asmtest.

  test/fuzz.py [FIRST_SEED] [COUNT]
"""
import random, subprocess, sys, os
here = os.path.expanduser("~/Documents/Projects/ubiqos/modules/blixt/test")
asmtest = os.path.join(os.environ.get("TMPDIR","/tmp"), "blixt-asmtest", "asmtest")
def prog(rng):
    n = rng.randint(5, 60)
    labels = ["L%d" % i for i in range(rng.randint(2, 8))]
    lines, placed = [], []
    for i in range(n):
        r = rng.random()
        if r < 0.12 and labels:
            l = labels.pop(0); lines.append(l + ":"); placed.append(l); continue
        allL = placed + labels
        t = rng.choice(allL) if allL else None
        c = rng.random()
        if c < 0.15 and t: lines.append("    b %s" % t)
        elif c < 0.25 and t: lines.append("    b%s %s" % (rng.choice(["eq","ne","lt","gt","hi"]), t))
        elif c < 0.32 and t: lines.append("    bl %s" % t)
        elif c < 0.40 and t: lines.append("    adr r%d, %s" % (rng.randint(0,7), t))
        elif c < 0.50: lines.append("    ldr r%d, =0x%x" % (rng.randint(0,7), rng.randint(0x10000, 0xffffffff)))
        elif c < 0.55 and t: lines.append("    ldr r%d, %s" % (rng.randint(0,7), t))
        elif c < 0.62: lines.append("    .space %d" % rng.choice([1,2,3,4,6,100,500,1100,2100]))
        elif c < 0.68: lines.append("    .align %d" % rng.randint(1,3))
        elif c < 0.72: lines.append("    .ltorg")
        elif c < 0.78: lines.append("    .word 0x%x" % rng.randint(0, 0xffffffff))
        elif c < 0.82: lines.append("    .byte %d" % rng.randint(0, 255))
        else: lines.append(rng.choice(["    adds r0, r1", "    movs r2, #5", "    mov r8, r9", "    add r0, r1, r2", "    nop", "    push {r4, lr}"]))
    for l in labels: lines.append(l + ":")
    # instructions need an even address: align before each instruction run
    out, odd_risk = [], False
    for l in lines:
        if l.strip().startswith((".byte", ".space")): odd_risk = True
        elif not l.strip().startswith(".") and odd_risk:
            out.append("    .align 1"); odd_risk = False
        out.append(l)
    return "\n".join(out) + "\n"
seed0 = int(sys.argv[1]) if len(sys.argv) > 1 else 1
N = int(sys.argv[2]) if len(sys.argv) > 2 else 300
bad = gasfail = 0
for seed in range(seed0, seed0 + N):
    src = prog(random.Random(seed))
    open("f.s","w").write(src)
    open("f.gas.s","w").write(".syntax unified\n.thumb\n.text\n" + src)
    g = subprocess.run(["arm-none-eabi-as","-mcpu=cortex-m33","-mthumb","-o","f.o","f.gas.s"], capture_output=True)
    if g.returncode: gasfail += 1; continue
    subprocess.run(["arm-none-eabi-objcopy","-O","binary","-j",".text","f.o","f.bin"], check=True)
    b = subprocess.run([asmtest,"f.s","f.bin"], capture_output=True, text=True)
    if b.returncode:
        bad += 1
        if bad <= 3:
            open("fail%d.s" % seed, "w").write(src)
            print("seed", seed, b.stdout[-600:])
print("programs %d, gas refused %d, different %d" % (N, gasfail, bad))
