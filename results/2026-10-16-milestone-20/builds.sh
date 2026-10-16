#!/bin/bash
# Builds each break set of breaks.py in a copy of the layer, never the layer itself, and keeps each broken title as
# vulkan4xenon_test_broken_<set>.xex in OUT. Run in WSL after building xsdk: builds.sh 1a 1b ...
HERE=$(cd "$(dirname "$0")" && pwd)
LAYER=$(cd "$HERE/../.." && pwd)
XSDK=${XSDK:-$(cd "$LAYER/../Xbox-360-SDK" && pwd)}
OUT=${OUT:-$HERE/broken}
COPY="$OUT/copy"
mkdir -p "$OUT" || exit 1
for set in "$@"; do
	rm -rf "$COPY" && mkdir -p "$COPY" || exit 1
	tar -C "$LAYER" --exclude=./build --exclude=./results --exclude=./.git -cf - . | tar -C "$COPY" -xf - || exit 1
	python3 "$HERE/breaks.py" "$set" "$COPY" || exit 1
	if make -C "$COPY" XSDK="$XSDK" -j32 >"$OUT/build_$set.txt" 2>&1; then
		cp "$COPY/build/vulkan4xenon_test.xex" "$OUT/vulkan4xenon_test_broken_$set.xex"
	else
		echo "set $set didn't build:"
		grep -E "error:" "$OUT/build_$set.txt"
	fi
done
rm -rf "$COPY"
ls -la "$OUT"/*.xex
