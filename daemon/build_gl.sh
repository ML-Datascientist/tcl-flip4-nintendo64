#!/bin/sh
# Build the emulator daemon (n64play_gl) from source.
#
#   ./build_gl.sh                 fetch upstream if needed, build, install to ../prebuilt/armv7
#   NDK=/path/to/ndk ./build_gl.sh
#
# Stack: mupen64plus-libretro-nx (ARM new-dynarec, 32-bit) + GLideN64 (GLES3)
# as the RDP, HLE/cxd4 RSP selectable at runtime (N64RSP=hle default).
# Linkage: NDK clang against bionic vendor libs (libEGL/libGLESv2) — musl
# cannot load bionic vendor dependencies, which is why the software-RDP build
# used a different toolchain.
set -e
cd "$(dirname "$0")"
ROOT=$(pwd -P)
PREBUILT="$ROOT/../prebuilt/armv7"

NDK=${NDK:-$ROOT/../../android-ndk-r26d}
TC=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin
CCB="$TC/armv7a-linux-androideabi34-clang"
CXXB="$TC/armv7a-linux-androideabi34-clang++"
[ -x "$CCB" ] || {
  echo "NDK not found at $NDK"
  echo "download: https://developer.android.com/ndk/downloads  (tested: r26d)"
  echo "or: NDK=/path/to/android-ndk-r26d ./build_gl.sh"
  exit 1
}

# Upstream core tree. The port was validated against one specific snapshot of
# the develop branch, so the tarball is fetched once and kept: nx.tar.gz.sha256
# is the sha256 of the exact snapshot prebuilt/armv7/n64play_gl was built from,
# and the script refuses a different tarball (unset CHECK_UPSTREAM=1 to allow).
NX_URL=${NX_URL:-https://codeload.github.com/libretro/mupen64plus-libretro-nx/tar.gz/refs/heads/develop}
if [ ! -d nx-gl ]; then
  if [ ! -f nx.tar.gz ]; then
    curl -fL -o nx.tar.gz "$NX_URL"
    sha256sum nx.tar.gz > nx.tar.gz.sha256.fetched
  fi
  if [ -f nx.tar.gz.sha256 ] && [ "${CHECK_UPSTREAM:-1}" = 1 ]; then
    sha256sum -c nx.tar.gz.sha256 || {
      echo "nx.tar.gz does not match daemon/nx.tar.gz.sha256 (the snapshot the"
      echo "shipped binary was built from). Fetch that snapshot or set"
      echo "CHECK_UPSTREAM=0 to build from whatever NX_URL points at."
      exit 1
    }
  fi
  tar xzf nx.tar.gz && mv mupen64plus-libretro-nx-* nx-gl
fi

COMMON="HAVE_PARALLEL_RSP=0 HAVE_PARALLEL_RDP=0 HAVE_THR_AL=1 LLE=1 GLES3=1"
# GLES3 (not GLES2): the Adreno driver on this SoC only hands out ES3.2
# contexts, and GLideN64 emits "#version <runtime>0 es" — an ES2 build puts ES1
# syntax under #version 320 and the shaders fail to compile.
make -C nx-gl platform=armv7a_hardfloat $COMMON CPUFLAGS="-DARM -marm" -j8 \
     CC="$CCB" CXX="$CXXB" CC_AS="$CCB" \
     >build_core.log 2>&1 || true     # the in-tree .so link always fails (-lEGL)

NOBJ=$(find nx-gl -name '*.o' | wc -l)
echo "core objects: $NOBJ"
[ "$NOBJ" -ge 200 ] || { echo "core build incomplete — see daemon/build_core.log"; exit 1; }

COBJS="-DN64HAVE_GL -O2 -Inx-gl/libretro -Inx-gl/libretro-common/include -Inx-gl/mupen64plus-core/src -Inx-gl/custom"
$CCB $COBJS -c n64play.c  -o n64play_gl.o
$CCB $COBJS -c n64stubs.c -o n64stubs_gl.o
$CCB $COBJS -c n64gl.c    -o n64gl.o
$CXXB -static-libstdc++ -O2 -o n64play_gl \
      n64play_gl.o n64stubs_gl.o n64gl.o \
      $(find nx-gl -name '*.o') \
      -lm -lEGL -lGLESv2 -lGLESv3 -ldl -llog
"$TC/llvm-strip" --strip-unneeded n64play_gl 2>/dev/null || true
mkdir -p "$PREBUILT"
cp -f n64play_gl "$PREBUILT/n64play_gl"
ls -l "$PREBUILT/n64play_gl"
echo "done"
