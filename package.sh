#!/bin/sh
set -e

PKGDIR="${MESON_BUILD_ROOT}/pkg"

mkdir -p "$PKGDIR"
cd "$PKGDIR"

mkdir -p wine/x86_64-windows
cp "${MESON_BUILD_ROOT}/spout2pw.exe" wine/x86_64-windows
mkdir -p wine/x86_64-unix
cp "${MESON_BUILD_ROOT}/spout2pw.so" wine/x86_64-unix
cp "${MESON_BUILD_ROOT}/subprojects/spoutdxtoc/spoutdxtoc.dll" .
cp "${MESON_SOURCE_ROOT}/spout2pw.inf" .
cp "${MESON_SOURCE_ROOT}/install_spout2pw.cmd" .
cp "${MESON_SOURCE_ROOT}/install_into_proton.sh" .

echo -ne "Wine builtin DLL\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0" | \
    dd of=wine/x86_64-windows/spout2pw.exe bs=1 seek=64 conv=notrunc \
    2>/dev/null

