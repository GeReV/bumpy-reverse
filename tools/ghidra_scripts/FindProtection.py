# Find the copy-protection routine(s): locate protection strings, their code
# xrefs, and decompile the referencing functions.
# @category Bumpy
# Jython 2.7 compatible (no f-strings).
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

prog = currentProgram
mem = prog.getMemory()
refmgr = prog.getReferenceManager()
fm = prog.getFunctionManager()
monitor = ConsoleTaskMonitor()

targets = ["ENTER YOUR PASSWORD", "PASSWORD ERROR", "PASSWORD OK",
           "Enter the platform", "INSERT THE OTHER", "YOUR PASSWORD"]


def find_all(s):
    res = []
    b = bytearray(s.encode("latin1"))
    a = prog.getMinAddress()
    while True:
        hit = mem.findBytes(a, b, None, True, monitor)
        if hit is None:
            break
        res.append(hit)
        a = hit.add(1)
    return res


decomp = DecompInterface()
decomp.openProgram(prog)

seen = set()
func_for_string = {}

for s in targets:
    addrs = find_all(s)
    for addr in addrs:
        print("STRING %r @ %s" % (s, addr))
        refs = list(refmgr.getReferencesTo(addr))
        if not refs:
            print("   (no direct xrefs found)")
        for r in refs:
            fa = r.getFromAddress()
            f = fm.getFunctionContaining(fa)
            fname = f.getName() if f is not None else "(none)"
            print("   xref from %s  reftype=%s  func=%s" % (fa, r.getReferenceType(), fname))
            if f is not None:
                func_for_string.setdefault(f.getEntryPoint(), f)

print("")
print("==================== DECOMPILED REFERENCING FUNCTIONS ====================")
for ep, f in func_for_string.items():
    if ep in seen:
        continue
    seen.add(ep)
    res = decomp.decompileFunction(f, 90, monitor)
    print("")
    print("===== %s @ %s =====" % (f.getName(), ep))
    if res is not None and res.getDecompiledFunction() is not None:
        print(res.getDecompiledFunction().getC())
    else:
        print("   <decompilation failed>")
