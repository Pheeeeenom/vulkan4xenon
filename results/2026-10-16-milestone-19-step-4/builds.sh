#!/bin/bash
# Builds each break set of step4_breaks.py, restoring the tree after each, then the layer clean again. The broken
# titles go to the scratchpad, where make clean doesn't reach.
S=/mnt/c/Users/Mena/AppData/Local/Temp/claude/Z--Xbox-360-SDK/3ba51ff7-78d2-4449-9ad4-c903b0892394/scratchpad
cd /mnt/z/vulkan4xenon || exit 1
state() { git diff | sha1sum; git status --short | sha1sum; }
state >"$S/before4.sha"
for set in "$@"; do
	python3 "$S/step4_breaks.py" apply "$set" || exit 1
	make -j32 >"$S/broken4_$set.txt" 2>&1 && cp build/vulkan4xenon_test.xex "$S/v4x_s4_broken_$set.xex"
	grep -E "error:" "$S/broken4_$set.txt"
	python3 "$S/step4_breaks.py" restore
done
state | diff - "$S/before4.sha" && echo tree-restored
make clean >/dev/null && make -j32 >"$S/clean4.txt" 2>&1 && echo clean-built
ls -la --time-style=+%T "$S"/v4x_s4_*.xex build/vulkan4xenon_test.xex
