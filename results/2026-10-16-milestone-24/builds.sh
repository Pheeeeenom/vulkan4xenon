#!/bin/bash
# Builds each break set of breaks.py in a copy of the layer, one .xex a set: builds.sh a b c d e f. The layer and
# xsdk at LAYER and XSDK are only read, xsdk built first; the copies and titles go to S, where make clean doesn't reach.
LAYER=${LAYER:-/mnt/z/vulkan4xenon}
XSDK=${XSDK:-/mnt/z/Xbox-360-SDK}
S=${S:-/mnt/c/Users/Mena/AppData/Local/Temp/claude/Z--Xbox-360-SDK/5f2bc020-db5c-4043-b258-abaf74d81117/scratchpad}
HERE=$(cd "$(dirname "$0")" && pwd)
for set in "$@"; do
	copy="$S/m24_broken_$set"
	rm -rf "$copy" && mkdir -p "$copy" || exit 1
	(cd "$LAYER" && tar --exclude=./build --exclude=./.git -cf - .) | (cd "$copy" && tar -xf -) || exit 1
	python3 "$HERE/breaks.py" "$set" "$copy" || exit 1
	make -C "$copy" XSDK="$XSDK" -j32 build/vulkan4xenon_test.xex >"$S/m24_broken_$set.txt" 2>&1 &&
		cp "$copy/build/vulkan4xenon_test.xex" "$S/v4x_m24_broken_$set.xex"
	grep -E "error:" "$S/m24_broken_$set.txt"
	rm -rf "$copy"
done
ls -la --time-style=+%T "$S"/v4x_m24_broken_*.xex
