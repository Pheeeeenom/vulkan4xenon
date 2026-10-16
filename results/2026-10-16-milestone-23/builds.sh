#!/bin/bash
# Builds each break set of breaks.py in a copy of the layer, against the xsdk beside it, as
# vulkan4xenon_test_broken_<set>.xex in OUT, broken beside this script by default; the working tree isn't touched. Run
# in WSL after building xsdk: builds.sh [sets], all three by default.
HERE=$(cd "$(dirname "$0")" && pwd)
LAYER=$(cd "$HERE/../.." && pwd)
XSDK=${XSDK:-$(cd "$LAYER/../Xbox-360-SDK" && pwd)}
OUT=${OUT:-$HERE/broken}
mkdir -p "$OUT" || exit 1
for set in ${@:-a b c}; do
	copy="$OUT/layer_$set"
	rm -rf "$copy"
	mkdir -p "$copy"
	(cd "$LAYER" && tar --exclude=./.git --exclude=./build --exclude=./results -cf - .) | (cd "$copy" && tar -xf -) || exit 1
	python3 "$HERE/breaks.py" "$copy" "$set" || exit 1
	(cd "$copy" && make -j32 XSDK="$XSDK" build/vulkan4xenon_test.xex >"$OUT/broken_$set.txt" 2>&1) &&
		cp "$copy/build/vulkan4xenon_test.xex" "$OUT/vulkan4xenon_test_broken_$set.xex"
	grep -E "error:" "$OUT/broken_$set.txt"
	rm -rf "$copy"
done
ls -la "$OUT"/*.xex
