#!/bin/bash
# Builds the clean title and each named break set, restoring the tree after each, then the clean title again.
S=/mnt/c/Users/Mena/AppData/Local/Temp/claude/Z--Xbox-360-SDK/3ba51ff7-78d2-4449-9ad4-c903b0892394/scratchpad
cd /mnt/z/vulkan4xenon || exit 1
rm -f build/v4x_s2_*.xex
make clean >/dev/null
make -j32 >"$S/clean2.txt" 2>&1 || { grep -E "error:" -A3 "$S/clean2.txt" | head; exit 1; }
cp build/vulkan4xenon_test.xex build/v4x_s2_clean.xex
state() { git diff 2>/dev/null | sha1sum; git status --short | sha1sum; }
state >"$S/before2.sha"
for set in "$@"; do
	python3 "$S/step2_breaks.py" apply "$set" &&
		make -j32 >"$S/broken_$set.txt" 2>&1 &&
		cp build/vulkan4xenon_test.xex "build/v4x_s2_broken_$set.xex"
	grep -E "error:" "$S/broken_$set.txt"
	python3 "$S/step2_breaks.py" restore
done
state | diff - "$S/before2.sha" && echo tree-restored
make -j32 >"$S/clean2.txt" 2>&1 && echo clean-built
ls -la --time-style=+%T build/*.xex
