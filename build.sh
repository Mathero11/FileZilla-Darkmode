#!/bin/sh
# Builds dist/version.dll (64-bit, like FileZilla) with MinGW and copies the sample ini next to it.
set -e
cd "$(dirname "$0")"

CC=x86_64-w64-mingw32-gcc
WINDRES=x86_64-w64-mingw32-windres

mkdir -p build dist
$WINDRES src/version.rc -O coff -o build/version_res.o
$CC -O2 -Wall -Wextra -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0A00 \
    -shared -s -static-libgcc \
    -o dist/version.dll \
    src/main.c src/config.c src/log.c src/proxy.c src/dark_api.c src/darkmode.c \
    src/colors.c src/controls.c src/iat.c src/wx_hooks.c src/menus.c \
    build/version_res.o src/version.def \
    -luser32 -lgdi32 -ladvapi32 \
    -Wl,--dynamicbase -Wl,--nxcompat -Wl,--high-entropy-va
cp fz-darkmode.ini dist/

echo "done: dist/version.dll"
