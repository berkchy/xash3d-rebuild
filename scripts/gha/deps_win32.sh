#!/bin/bash

. scripts/lib.sh

download_ffmpeg()
{
	for url in "$@"; do
		echo "Trying FFmpeg archive: $url"
		if curl -fL "$url" -o ffmpeg.zip; then
			return 0
		fi
	done

	return 1
}

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
FFMPEG_URLS=(
	"https://github.com/FWGS/FFmpeg-Builds/releases/download/latest/$FFMPEG_ARCHIVE.zip"
)

# BtbN still publishes the current win64 shared release builds even when the
# FWGS mirror lags behind or temporarily misses a matching archive.
if [ "$GH_CPU_ARCH" = "amd64" ]; then
	FFMPEG_URLS+=(
		"https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/$FFMPEG_ARCHIVE.zip"
	)
fi

if download_ffmpeg "${FFMPEG_URLS[@]}"; then
	unzip -q ffmpeg.zip || die
	FFMPEG_DIR=$(find . -maxdepth 1 -type d -name 'ffmpeg-*' ! -name 'ffmpeg' | head -n1)
	[ -n "$FFMPEG_DIR" ] || die
	mv "$FFMPEG_DIR" ffmpeg || die
else
	echo "Warning: no prebuilt FFmpeg archive found for $GH_CPU_OS/$GH_CPU_ARCH, continuing without FFmpeg support"
fi
