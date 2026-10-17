#!/bin/bash
# Builds vulkan4xenon_test once for each break set of breaks.py, each in a copy of this layer, against the xsdk beside
# it, as vulkan4xenon_test_m25_broken_<set>.xex in OUT, broken beside this script by default: builds.sh [set ...],
# every set when none is named. The tree itself is never touched. Run in WSL, after building xsdk.
HERE=$(cd "$(dirname "$0")" && pwd)
LAYER=$(cd "$HERE/../.." && pwd)
XSDK=${XSDK:-$(cd "$LAYER/../Xbox-360-SDK" && pwd)}
OUT=${OUT:-$HERE/broken}
mkdir -p "$OUT" || exit 1
for set in ${@:-a b c}; do
	COPY="$OUT/v4x_m25_$set"
	rm -rf "$COPY" && mkdir -p "$COPY" || exit 1
	tar -C "$LAYER" --exclude=./build --exclude=./.git --exclude=./results -cf - . | tar -C "$COPY" -xf - || exit 1
	python3 "$HERE/breaks.py" "$COPY" "$set" || exit 1
	if make -C "$COPY" -j32 XSDK="$XSDK" build/vulkan4xenon_test.xex >"$OUT/v4x_m25_$set.txt" 2>&1; then
		cp "$COPY/build/vulkan4xenon_test.xex" "$OUT/vulkan4xenon_test_m25_broken_$set.xex" && echo "set $set built"
	else
		grep -E "error:" "$OUT/v4x_m25_$set.txt"
		echo "set $set failed to build"
	fi
	rm -rf "$COPY"
done
ls -la "$OUT"/vulkan4xenon_test_m25_broken_*.xex
