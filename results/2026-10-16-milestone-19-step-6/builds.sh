#!/bin/bash
# Builds each break set of step6_breaks.py, restoring the tree after each, then the layer clean again. The broken
# titles go to the scratchpad, where make clean doesn't reach.
S=/mnt/c/Users/Mena/AppData/Local/Temp/claude/Z--Xbox-360-SDK/5f2bc020-db5c-4043-b258-abaf74d81117/scratchpad
cd /mnt/z/vulkan4xenon || exit 1
state() { git diff | sha1sum; git status --short | sha1sum; }
state >"$S/before6.sha"
for set in "$@"; do
	python3 "$S/step6_breaks.py" apply "$set" || exit 1
	make -j32 >"$S/broken6_$set.txt" 2>&1 && cp build/vulkan4xenon_test.xex "$S/v4x_s6_broken_$set.xex"
	grep -E "error:" "$S/broken6_$set.txt"
	python3 "$S/step6_breaks.py" restore
done
state | diff - "$S/before6.sha" && echo tree-restored
make clean >/dev/null && make -j32 >"$S/clean6.txt" 2>&1 && echo clean-built
ls -la --time-style=+%T "$S"/v4x_s6_*.xex build/vulkan4xenon_test.xex
