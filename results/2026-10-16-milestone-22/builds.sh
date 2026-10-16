#!/bin/bash
# Builds each of breaks.py's sets given (a, b, c by default) into vulkan4xenon_test_m22_broken_<set>.xex, each in a
# fresh copy of xsdk and the layer, with the test's shaders built again by the copy's xost, and leaves the trees alone.
# XSDK and LAYER name the trees, WORK the folder for the copies and the titles; WORK must be on a Windows drive, as
# glslang.exe runs in it.
HERE=$(cd "$(dirname "$0")" && pwd)
XSDK=${XSDK:-/mnt/z/Xbox-360-SDK}
LAYER=${LAYER:-/mnt/z/vulkan4xenon}
WORK=${WORK:-$HERE/work}
GLSLANG=$XSDK/xenos-toolchain/tools/glslang/bin/glslang.exe
mkdir -p "$WORK/out" || exit 1
for set in ${@:-a b c}; do
	copy="$WORK/copy_$set"
	rm -rf "$copy" && mkdir -p "$copy/Xbox-360-SDK" "$copy/vulkan4xenon" || exit 1
	tar -C "$XSDK" --exclude=./.git --exclude=./build --exclude=./xenos-toolchain/reference \
		--exclude=./xenos-toolchain/tools/glslang -cf - . | tar -C "$copy/Xbox-360-SDK" -xf - || exit 1
	tar -C "$LAYER" --exclude=./.git --exclude=./build -cf - . | tar -C "$copy/vulkan4xenon" -xf - || exit 1
	python3 "$HERE/breaks.py" "$set" "$copy" || exit 1
	(cd "$copy/Xbox-360-SDK" && make -j32 >"$WORK/xsdk_$set.txt" 2>&1) || { echo "set $set: xsdk failed"; exit 1; }
	(cd "$copy/vulkan4xenon" && python3 tools/build_shaders.py --xost ../Xbox-360-SDK/xenos-toolchain \
		--glslang "$GLSLANG" --output tests/shaders tests/shaders/*.vert tests/shaders/*.frag >"$WORK/shaders_$set.txt" \
		2>&1) || { echo "set $set: shaders failed"; exit 1; }
	(cd "$copy/vulkan4xenon" && make -j32 build/vulkan4xenon_test.xex >"$WORK/layer_$set.txt" 2>&1) ||
		{ echo "set $set: layer failed"; exit 1; }
	cp "$copy/vulkan4xenon/build/vulkan4xenon_test.xex" "$WORK/out/vulkan4xenon_test_m22_broken_$set.xex"
	python3 "$HERE/changed.py" "$LAYER/tests/shaders.c" "$copy/vulkan4xenon/tests/shaders.c" "set $set" || exit 1
	rm -rf "$copy"
done
ls -la "$WORK/out"
