#!/bin/sh
# Builds a minimal static FFmpeg (only the XMA decoders) from Xenia's fork, for host/CMakeLists.txt (FFMPEG_DIR).
# Usage: tools/build_ffmpeg.sh <work dir>   (Linux; the Windows part cross-compiles with clang + mingw-w64)
set -e
W=${1:-$PWD/ffmpeg-build}
mkdir -p "$W" && cd "$W"
[ -d ffmpeg-xe ] || git clone --depth 1 https://github.com/xenia-project/FFmpeg.git ffmpeg-xe
rm -rf ff-linux ff-win && cp -r ffmpeg-xe ff-linux && cp -r ffmpeg-xe ff-win && rm -f ff-linux/config.h ff-win/config.h
SRC=.
COMMON="--disable-everything --disable-programs --disable-doc --disable-avdevice --disable-avformat --disable-swscale --disable-swresample --disable-postproc --disable-avfilter --disable-network --disable-iconv --disable-zlib --disable-bzlib --disable-lzma --disable-sdl2 --disable-xlib --disable-vaapi --disable-vdpau --disable-cuda --disable-cuvid --disable-nvenc --disable-ffnvcodec --disable-amf --disable-d3d11va --disable-dxva2 --disable-mediafoundation --disable-schannel --disable-securetransport --disable-x86asm --disable-debug --enable-static --disable-shared --enable-decoder=xmaframes,xma1,xma2,wmapro"
cd $W/ff-linux
$SRC/configure $COMMON --prefix=$W/ff-linux/out --cc=clang > cfg.log 2>&1
make -j2 > make.log 2>&1 && make install > inst.log 2>&1
echo LINUX_DONE
cd $W/ff-win
$SRC/configure $COMMON --prefix=$W/ff-win/out --enable-cross-compile --target-os=mingw32 --arch=x86_64 --cc="clang --target=x86_64-w64-mingw32" --ar=llvm-ar --ranlib=llvm-ranlib --nm=llvm-nm > cfg.log 2>&1
make -j2 > make.log 2>&1 && make install > inst.log 2>&1
echo WIN_DONE
