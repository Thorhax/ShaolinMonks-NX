"""Names the addresses in a console profile (perf.cpp, [switch] profile = true).

profile.py <shaolinmonks ELF> <logs/profile.log> [top]: per thread, the functions the samples were in (self) and the
functions on the stack (inclusive: the function or something it called), in percent of that thread's samples.
"""
import bisect
import collections
import os
import re
import subprocess
import sys

DKA64 = os.environ.get("DKA64", os.path.join(os.environ.get("DEVKITPRO", "/opt/devkitpro"), "devkitA64"))


def symbols(elf):
    out = subprocess.run([os.path.join(DKA64, "bin/aarch64-none-elf-nm"), "-n", "-C", "--defined-only", elf],
                         capture_output=True, text=True, check=True).stdout
    addrs, names = [], []
    for line in out.splitlines():
        parts = line.split(None, 2)
        if len(parts) == 3 and parts[1] in "tTwW":
            addrs.append(int(parts[0], 16))
            names.append(re.sub(r"\((?!anonymous namespace\)).*", "", parts[2])[:90])
    return addrs, names


def main():
    elf, log = sys.argv[1], sys.argv[2]
    top = int(sys.argv[3]) if len(sys.argv) > 3 else 30
    addrs, names = symbols(elf)

    def name(a):
        i = bisect.bisect_right(addrs, a) - 1
        return names[i] if i >= 0 else "?"

    threads = {}
    current = None
    for line in open(log):
        kind, *rest = line.split()
        if kind == "thread":
            current = threads.setdefault(" ".join(rest[:-1]), {"samples": int(rest[-1]), "self": collections.Counter(),
                                                               "incl": collections.Counter()})
        elif current is not None:
            current[kind][name(int(rest[0], 16))] += int(rest[1])
    for thread, p in sorted(threads.items(), key=lambda kv: -kv[1]["samples"]):
        n = max(1, p["samples"])
        print(f"\n## {thread}: {p['samples']} samples")
        for kind in ("self", "incl"):
            print(f"  -- {kind}")
            for fn, c in p[kind].most_common(top):
                print(f"  {100 * c / n:5.1f}%  {fn}")


if __name__ == "__main__":
    main()
