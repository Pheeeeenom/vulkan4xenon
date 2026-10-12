#!/usr/bin/env python3
"""Count what a Dusklight run's refused shader configs need, and the targets it draws into, from the run's log
(ROADMAP.md, milestone 11, step 1).

    count_refusals.py results/2026-10-12-milestone-11-step-1/dusklight-record-xenia.log

Reads Aurora's "no GLSL yet for config" lines, one for each config its GLSL generator refuses, with the config's hash,
its hash with every channel's light masks cleared, and all it needs (aurora-xenon:lib/vulkan4xenon/glsl.cpp,
`refuse`), and the target lines a recording build's null backend writes (aurora-xenon:lib/null/backend.cpp,
`log_target`).
"""

import re
import sys
from collections import Counter, defaultdict

ANSI = re.compile(r"\x1b\[[0-9;]*m")
REFUSED = re.compile(r"no GLSL yet for config ([0-9a-f]{16}) \(([0-9a-f]{16}) without its lights\): (.*)$")
TARGET = re.compile(r"(Copy target|Conversion target|Offscreen target|Offscreen depth target) (\d+)x(\d+), "
                    r"GX format (\w+)")
RECORDED = re.compile(r"recorded ([0-9a-f]{16}\.(?:vert|frag))")
# The layer's smallest colour attachment is one more than this on a side (src/image.c, check_create_info).
PACKED_SIDE = 16
FEATURES = ("fog", "matrices picked per vertex", "indirect texturing", "lighting")


def kind(need):
    """A need's name, without its details."""
    return need.split(" (")[0]


def show(title, counts):
    print(title)
    for name, count in counts.most_common():
        print(f"  {count:3} {name}")


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    text = ANSI.sub("", open(sys.argv[1], encoding="utf-8", errors="replace").read())
    needs = {}
    unlit = {}
    targets = set()
    recorded = set()
    for line in text.splitlines():
        if m := REFUSED.search(line):
            if m.group(1) in needs:
                print(f"config {m.group(1)} is logged twice")
            needs[m.group(1)] = m.group(3).split("; ")
            unlit[m.group(1)] = m.group(2)
        elif m := TARGET.search(line):
            targets.add((m.group(1), int(m.group(2)), int(m.group(3)), m.group(4)))
        elif m := RECORDED.search(line):
            recorded.add(m.group(1))

    print(f"refused configs: {len(needs)}\n")
    show("by the first thing each needs, in the order the generator checks:",
         Counter(kind(n[0]) for n in needs.values()))
    show("\nneeding each:", Counter(kind(need) for n in needs.values() for need in set(n)))
    show("\nby all they need:", Counter(" + ".join(sorted({kind(need) for need in n})) for n in needs.values()))

    for feature in FEATURES[:3]:
        found = Counter(need for n in needs.values() for need in n if kind(need) == feature)
        show(f"\n{feature}: {sum(found.values())} configs", found)
        if feature == "fog":
            print(f"  {sum(c for need, c in found.items() if 'range' in need)} of them adjusted by range")

    lit = {h: next(need for need in n if kind(need) == "lighting") for h, n in needs.items()
           if any(kind(need) == "lighting" for need in n)}
    groups = defaultdict(list)
    for h in lit:
        groups[unlit[h]].append(h)
    print(f"\nlighting: {len(lit)} configs, {len(groups)} with their light masks cleared")
    print(f"  {sum(unlit[h] != h for h in lit)} of them change when their masks are cleared")
    for members in (m for m in groups.values() if len(m) > 1):
        masks = " ".join(sorted(re.findall(r"lights (0x[0-9a-f]+)", " ".join(lit[h] for h in members))))
        print(f"  {len(members)} differ only in their light masks: {masks}")
    show("  lit channels:", Counter(c for need in lit.values() for c in need[len("lighting ("):-1].split(", ")))
    sums = Counter(bin(int(mask, 16)).count("1") for need in lit.values()
                   for mask in re.findall(r"lights (0x[0-9a-f]+)", need))
    print("  lights a lit channel sums: " + ", ".join(f"{k} in {v}" for k, v in sorted(sums.items())))

    other = Counter(kind(need) for n in needs.values() for need in n if kind(need) not in FEATURES)
    if other:
        show("\nother needs:", other)

    print(f"\ntargets: {len(targets)}")
    for usage, width, height, gx in sorted(targets):
        small = f", {PACKED_SIDE} texels or less on a side" if min(width, height) <= PACKED_SIDE else ""
        print(f"  {usage} {width}x{height}, GX format {gx}{small}")
    print(f"\nprograms recorded: {len(recorded)}")


if __name__ == "__main__":
    main()
