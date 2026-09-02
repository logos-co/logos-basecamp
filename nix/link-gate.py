"""Duplicate-library link gate. Invoked by nix/link-gate.nix; see it for why."""
import collections
import os
import subprocess
import sys

root, nm, fmt = sys.argv[1], sys.argv[2], sys.argv[3]
macho = fmt == "macho"
suffix = ".dylib" if macho else ".so"
libdir = os.path.join(root, "lib")


def syms(path, which):
    if macho:
        args = ["-gU"] if which == "defined" else ["-u"]
    else:
        args = ["-D", "--" + which + "-only"]
    out = subprocess.run([nm] + args + [path], capture_output=True, text=True).stdout
    return {line.split()[-1] for line in out.splitlines() if line.strip()}


def rel(p):
    return os.path.relpath(p, root)


libs = []
for parent, _, names in os.walk(root):
    for name in names:
        path = os.path.join(parent, name)
        if not os.path.islink(path) and suffix in name:
            libs.append(path)

staged_in_lib = {os.path.basename(p) for p in libs if os.path.dirname(p) == libdir}
elsewhere = collections.defaultdict(list)
for path in libs:
    base = os.path.basename(path)
    if base in staged_in_lib and os.path.dirname(path) != libdir:
        elsewhere[base].append(path)

# A gate that measured nothing must say so rather than report success.
if not libs:
    sys.exit("VACUOUS: no shared libraries found under %s" % root)
if not elsewhere:
    sys.exit("VACUOUS: no library is staged both in lib/ and beside a module. "
             "The bundle layout changed and this gate no longer sees the "
             "ambiguity it exists to catch.")

failures = 0
examined = 0
skewed = 0
for base in sorted(elsewhere):
    in_lib = syms(os.path.join(libdir, base), "defined")
    if not in_lib:
        sys.exit("VACUOUS: %s read 0 symbols from lib/%s" % (nm, base))
    for sibling in sorted(elsewhere[base]):
        beside = syms(sibling, "defined")
        if beside == in_lib:
            continue
        skewed += 1
        # Only a consumer sitting NEXT TO a second copy is ambiguous: that is
        # the one directory where the two platforms disagree about which copy
        # wins. Everything else resolves to lib/ on both.
        for consumer in sorted(libs):
            if os.path.dirname(consumer) != os.path.dirname(sibling):
                continue
            if consumer == sibling:
                continue
            wanted = syms(consumer, "undefined")
            from_lib, from_beside = wanted & in_lib, wanted & beside
            examined += 1
            if from_lib == from_beside:
                continue
            failures += 1
            print("FAIL  %s" % rel(consumer))
            print("        imports from %s, staged twice with DIFFERENT symbols:" % base)
            print("          lib/%s satisfies %d of its imports" % (base, len(from_lib)))
            print("          %s satisfies %d" % (rel(sibling), len(from_beside)))
            for s in sorted(from_beside - from_lib):
                print("          only beside the module: %s" % s)
            for s in sorted(from_lib - from_beside):
                print("          only in lib/:           %s" % s)

print()
print("libraries staged in lib/ and beside a module: %d" % len(elsewhere))
print("of those, copies whose symbols differ:        %d" % skewed)
print("consumers examined against both copies:       %d" % examined)
sys.exit(1 if failures else 0)
