#!/bin/bash
set -e

PKGDIR="${MESON_BUILD_ROOT}/pkg"

mkdir -p "$PKGDIR"
cd "$PKGDIR"

mkdir -p spout2pw-dlls/x86_64-windows
cp "${MESON_BUILD_ROOT}/spout2pw.exe" spout2pw-dlls/x86_64-windows
cp "${MESON_BUILD_ROOT}/subprojects/spoutdxtoc/spoutdxtoc.dll" spout2pw-dlls/x86_64-windows
mkdir -p spout2pw-dlls/x86_64-unix
cp "${MESON_BUILD_ROOT}/spout2pw.so" spout2pw-dlls/x86_64-unix
cp "${MESON_SOURCE_ROOT}/misc/spout2pw.inf" .
cp "${MESON_SOURCE_ROOT}/misc/spout2pw.sh" .

for i in spout2pw-dlls/x86_64-windows/*; do
    echo -ne "Wine builtin DLL\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0" | \
        dd of=$i bs=1 seek=64 conv=notrunc \
        2>/dev/null
done
