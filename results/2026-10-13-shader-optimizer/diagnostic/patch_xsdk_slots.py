"""patch_xsdk_slots.py <xsdk> <slots>: perf step 1's experiment, more resolve rectangle slots (src/gpu.c), placed at
0x400 in the 64 KB write-back page, past the stream's fences at 0x200. Temporary; put src/gpu.c back after."""
import sys
from pathlib import Path

p = Path(sys.argv[1]) / "src/gpu.c"
slots = int(sys.argv[2])
raw = p.read_bytes().decode()
crlf = "\r\n" in raw
s = raw.replace("\r\n", "\n")


def sub(old, new):
    global s
    assert s.count(old) == 1, old
    s = s.replace(old, new)


sub("\tRECTANGLE_SLOTS = 8,\n", f"\tRECTANGLE_SLOTS = {slots},\n")
sub("\tRECTANGLE_OFFSET = 0x40,\n", "\tRECTANGLE_OFFSET = 0x400,\n")
p.write_bytes((s.replace("\n", "\r\n") if crlf else s).encode())
print("patched", slots, "slots", "crlf" if crlf else "lf")
