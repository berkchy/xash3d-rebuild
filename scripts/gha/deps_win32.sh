#!/bin/bash

. scripts/lib.sh

curl -L http://libsdl.org/release/SDL2-devel-$SDL_VERSION-VC.zip -o SDL2.zip
unzip -q SDL2.zip
mv SDL2-$SDL_VERSION SDL2_VC

if [ "$GH_CPU_ARCH" = "i386" ]; then
	rustup target add i686-pc-windows-msvc
fi

curl -L https://github.com/FWGS/potential-meme/releases/download/prebuilts/mingw-w64-x86_64-pkgconf-1.2.3.0-1-any.pkg.tar.zst -o pkgconf.tar.zst
7z x pkgconf.tar.zst
7z x pkgconf.tar
rm pkgconf.tar*
mv mingw64 pkgconf

FFMPEG_ARCHIVE=$(get_ffmpeg_archive)
FFMPEG_URL=https://github.com/FWGS/FFmpeg-Builds/releases/download/latest/$FFMPEG_ARCHIVE.zip
curl -fL "$FFMPEG_URL" -o ffmpeg.zip || die
unzip -q ffmpeg.zip || die
FFMPEG_DIR=$(find . -maxdepth 1 -type d -name 'ffmpeg-*' ! -name 'ffmpeg' | head -n1)
[ -n "$FFMPEG_DIR" ] || die
mv "$FFMPEG_DIR" ffmpeg || die
