#!/bin/bash
# Builds each break set of breaks.py, or the sets named, into a copy of the layer's sources, not its results, against
# XSDK, the xsdk beside it by default, as vulkan4xenon_test_broken_<set>.xex in OUT, broken beside this script by
# default. The tree itself is never changed. Build xsdk first; run it in WSL.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
XSDK=${XSDK:-$(cd "$REPO/../Xbox-360-SDK" && pwd)}
OUT=${OUT:-$HERE/broken}
SETS=${*:-$(python3 "$HERE/breaks.py" --list)}
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
for set in $SETS; do
	copy=$(mktemp -d)
	cp -r "$REPO"/{src,include,tests,tools,third_party,bench,Makefile} "$copy"
	python3 "$HERE/breaks.py" "$copy" "$set"
	if make -C "$copy" -j32 XSDK="$XSDK" build/vulkan4xenon_test.xex >"$OUT/broken_$set.txt" 2>&1; then
		cp "$copy/build/vulkan4xenon_test.xex" "$OUT/vulkan4xenon_test_broken_$set.xex"
	else
		grep -E "error" "$OUT/broken_$set.txt"
	fi
	rm -rf "$copy"
done
ls -la "$OUT"/*.xex
