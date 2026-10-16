#!/bin/bash
# Builds vulkan4xenon_test once for each of breaks.py's breaks, or for those named, to vulkan4xenon_test_m21_<break>.xex
# in OUT, each from copies made there and deleted after; LAYER and XSDK are only read. A break of xost's is built into
# a copy of xsdk, its library and PC compiler, and the bundle is made again with that compiler; a break of the layer's
# links against XSDK's library as built.
LAYER=${LAYER:-/mnt/z/vulkan4xenon}
XSDK=${XSDK:-/mnt/z/Xbox-360-SDK}
OUT=${OUT:?OUT names a folder outside both trees for the copies and the titles}
BREAKS="$(dirname "$(readlink -f "$0")")/breaks.py"
mkdir -p "$OUT" || exit 1
for name in ${*:-$(python3 "$BREAKS" list "$XSDK")}; do
	copy="$OUT/m21-$name"
	log="$OUT/m21-$name.txt"
	rm -rf "$copy" && mkdir -p "$copy/Xbox-360-SDK" "$copy/vulkan4xenon" || exit 1
	tar -C "$LAYER" -cf - --exclude=build assets bench include src tests third_party tools Makefile |
		tar -C "$copy/vulkan4xenon" -mxf - || exit 1
	if [ "$(python3 "$BREAKS" kind "$name")" = xost ]; then
		tar -C "$XSDK" -cf - --exclude=build assets cmake imports include src tests tools xenos-toolchain Makefile |
			tar -C "$copy/Xbox-360-SDK" -mxf - || exit 1
		python3 "$BREAKS" apply "$name" "$XSDK" "$copy/vulkan4xenon" "$copy/Xbox-360-SDK" || exit 1
		(cd "$copy/Xbox-360-SDK" && make -j32 build/libxsdk.a build/host/libxsc.so) >"$log" 2>&1 &&
			(cd "$copy/vulkan4xenon" && python3 tools/build_shaders.py --xost ../Xbox-360-SDK/xenos-toolchain \
				--output tests/shaders tests/shaders/*.vert tests/shaders/*.frag) >>"$log" 2>&1 &&
			(cd "$copy/vulkan4xenon" && make -j32 build/vulkan4xenon_test.xex) >>"$log" 2>&1
	else
		python3 "$BREAKS" apply "$name" "$XSDK" "$copy/vulkan4xenon" "$copy/Xbox-360-SDK" || exit 1
		(cd "$copy/vulkan4xenon" && make -j32 XSDK="$XSDK" build/vulkan4xenon_test.xex) >"$log" 2>&1
	fi && cp "$copy/vulkan4xenon/build/vulkan4xenon_test.xex" "$OUT/vulkan4xenon_test_m21_$name.xex" ||
		echo "$name: the build failed, see $log"
	grep -E "error:" "$log"
	rm -rf "$copy"
done
ls -la --time-style=+%T "$OUT"/vulkan4xenon_test_m21_*.xex
