#!/bin/bash
# Builds each set of breaks.py, or those named, into a copy of the layer, with a copy of xsdk beside it where a set
# breaks xsdk, as vulkan4xenon_test_broken_<set>.xex in OUT, broken beside this script by default: builds.sh [set...].
# The repositories aren't touched; XSDK, the one beside the layer by default, is built in its own first, for the sets
# that don't copy it. Run in WSL.
HERE=$(cd "$(dirname "$0")" && pwd)
LAYER=$(cd "$HERE/../.." && pwd)
XSDK=${XSDK:-$(cd "$LAYER/../Xbox-360-SDK" && pwd)}
OUT=${OUT:-$HERE/broken}
mkdir -p "$OUT" || exit 1
OUT=$(cd "$OUT" && pwd)
SETS=${*:-$(python3 "$HERE/breaks.py" list)}
# The sources alone: no build output, history, results or the toolchain's read-only reference clones.
copy_tree() {
	mkdir -p "$2"
	tar -C "$1" --exclude=./build --exclude=./.git --exclude=./results --exclude=./local \
		--exclude=./xenos-toolchain/reference --exclude=./xenos-toolchain/tools/glslang -cf - . | tar -C "$2" -xf -
}
for set in $SETS; do
	work="$OUT/copy_$set"
	rm -rf "$work"
	copy_tree "$LAYER" "$work/vulkan4xenon"
	xsdk="$XSDK"
	if python3 "$HERE/breaks.py" touches "$set" Xbox-360-SDK; then
		copy_tree "$XSDK" "$work/Xbox-360-SDK"
		xsdk="$work/Xbox-360-SDK"
	fi
	python3 "$HERE/breaks.py" apply "$set" "$work" || exit 1
	if [ "$xsdk" != "$XSDK" ] && ! make -C "$xsdk" -j32 build/libxsdk.a >"$OUT/build_$set.txt" 2>&1; then
		echo "$set: xsdk didn't build ($OUT/build_$set.txt)"
		exit 1
	fi
	if ! make -C "$work/vulkan4xenon" -j32 XSDK="$xsdk" build/vulkan4xenon_test.xex >>"$OUT/build_$set.txt" 2>&1; then
		echo "$set: the layer didn't build ($OUT/build_$set.txt)"
		exit 1
	fi
	cp "$work/vulkan4xenon/build/vulkan4xenon_test.xex" "$OUT/vulkan4xenon_test_broken_$set.xex"
	rm -rf "$work"
	echo "$set: $OUT/vulkan4xenon_test_broken_$set.xex"
done
