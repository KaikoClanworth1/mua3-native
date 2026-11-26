#!/bin/bash
# Builds the FFmpeg DLLs the port uses to play the game's H.264 movies where Media Foundation can't (Wine/Proton has
# no H.264 decoder: it substitutes a 320x240 placeholder). LGPL only: no --enable-gpl, no --enable-nonfree.
# Only the H.264 decoder + parser, the MP4 (mov) demuxer and the file protocol are built, as DLLs next to mua3.exe
# (avcodec, avformat, avutil), so the LGPL "relink" right is met by swapping the DLLs.
#
# Run in MSYS2's MINGW64 environment (needs make, perl, mingw-w64-x86_64-gcc, mingw-w64-x86_64-nasm):
#   C:\msys64\usr\bin\bash.exe -lc "MSYSTEM=MINGW64 source /etc/profile; bash '/d/Switch Recomp/MUA 3/scripts/build_ffmpeg.sh'"
# Source: the official release tarball (signature checked) unpacked in third_party/ffmpeg_src/ffmpeg-$VER.
# Output: third_party/ffmpeg_build/{bin/*.dll, include/, lib/, CONFIGURE.txt} (git-ignored; never committed).
set -euo pipefail
VER=${FFMPEG_VER:-9.1}
ROOT_WIN='D:\Switch Recomp\MUA 3'
ROOT=$(cygpath -u "$ROOT_WIN")
# FFmpeg's configure rejects paths with spaces: reach the project through a junction without them, made by the caller
# (build\steamdeck\ffmpeg_build.ps1: C:\msys64\tmp\mua3_ffmpeg -> the project; removed afterwards).
LINK=${FFMPEG_LINK:-/tmp/mua3_ffmpeg}
[ -d "$LINK/third_party" ] || { echo "no junction $LINK -> $ROOT"; exit 1; }
SRC=$LINK/third_party/ffmpeg_src/ffmpeg-$VER
OUT=$LINK/third_party/ffmpeg_build
OBJ=$LINK/third_party/ffmpeg_src/obj-$VER
[ -x "$SRC/configure" ] || { echo "no FFmpeg source at $SRC"; exit 1; }
rm -rf "$OBJ" "$OUT"
mkdir -p "$OBJ" "$OUT"
cd "$OBJ"
FLAGS=(
    --prefix="$OUT" --target-os=mingw32 --arch=x86_64
    --enable-shared --disable-static
    --disable-everything --disable-autodetect --disable-programs --disable-doc --disable-network
    --disable-avdevice --disable-avfilter --disable-swscale --disable-swresample
    --enable-w32threads
    --enable-decoder=h264 --enable-parser=h264 --enable-demuxer=mov --enable-protocol=file
    --extra-ldflags="-static-libgcc -static"
)
"$SRC/configure" "${FLAGS[@]}"
make -j"${JOBS:-4}"
make install
# Licence + exact build record for the release (LGPL: version, configure line, source link).
cp "$SRC/COPYING.LGPLv2.1" "$OUT/LICENSE-ffmpeg.txt"
{
    echo "FFmpeg $VER, unmodified, from https://ffmpeg.org/releases/ffmpeg-$VER.tar.xz"
    echo "Licensed under the GNU LGPL v2.1 or later (see LICENSE-ffmpeg.txt). Built without GPL or nonfree parts."
    echo "Source code: https://ffmpeg.org/releases/ffmpeg-$VER.tar.xz (also on request from the port's maintainer)."
    echo "Build script: scripts/build_ffmpeg.sh in https://github.com/KaikoClanworth1/mua3-native"
    echo "configure ${FLAGS[*]}" | sed "s|$OUT|<prefix>|g"
    echo "Compiler: $(gcc --version | head -1); assembler: $(nasm -v)"
} > "$OUT/FFMPEG-BUILD.txt"
ls -la "$OUT/bin"
# The DLLs must not need MinGW runtime DLLs (libgcc/libwinpthread): w32threads + -static-libgcc.
objdump -p "$OUT"/bin/*.dll | grep "DLL Name" | sort -u
if objdump -p "$OUT"/bin/*.dll | grep -qiE "DLL Name: (libgcc|libwinpthread|libstdc)"; then
    echo "FFMPEG_BUILD_FAILED: the DLLs need MinGW runtime DLLs"; exit 1
fi
echo FFMPEG_BUILD_OK
