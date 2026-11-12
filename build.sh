#!/bin/sh
# Build the two small armv7 helpers (n64svc, n64boot) with the Android NDK.
# The emulator daemon is built separately (daemon/build_gl.sh) and normally
# shipped prebuilt in prebuilt/armv7/n64play_gl.
set -e
cd "$(dirname "$0")"
NDK=${NDK:-../android-ndk-r26d}
TC=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin
CCB="$TC/armv7a-linux-androideabi34-clang"
[ -x "$CCB" ] || {
  echo "NDK not found at $NDK"
  echo "download https://developer.android.com/ndk/downloads (tested: r26d) or set NDK=…"
  exit 1
}
mkdir -p out prebuilt/armv7
$CCB -O2 -Wall -Wextra -o out/n64svc  svc/n64svc.c
$CCB -O2 -Wall -Wextra -o out/n64boot boot/n64boot.c
"$TC/llvm-strip" --strip-unneeded out/n64svc out/n64boot 2>/dev/null || true
cp -f out/n64svc out/n64boot prebuilt/armv7/
ls -l out
