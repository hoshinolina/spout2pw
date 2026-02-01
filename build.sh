#!/bin/bash
set -e

base="$(realpath $(dirname "$0"))"

builddir="$base/build"
pw_builddir="$base/build-pw"
pw_srcdir="$base/subprojects/pipewire-static"

mkdir -p "$pw_builddir"

if [ ! -e "$pw_builddir"/build.ninja ]; then
    meson setup "$pw_builddir" "$pw_srcdir" \
        -Dprefix="$builddir/prefix/usr" \
        -Dexamples=disabled \
        -Dtests=disabled \
        -Dgstreamer=disabled \
        -Dlibsystemd=disabled \
        -Dlogind=disabled \
        -Dselinux=disabled \
        -Dpipewire-alsa=disabled \
        -Dpipewire-jack=disabled \
        -Dpipewire-v4l2=disabled \
        -Dspa-plugins=enabled \
        -Dudev=disabled \
        -Dsdl2=disabled \
        -Dv4l2=disabled \
        -Dalsa=disabled \
        -Dx11=disabled \
        -Dlibffado=disabled \
        -Dsnap=disabled \
        -Dopus=disabled \
        -Dreadline=disabled \
        -Dgsettings=disabled \
        -Dsession-managers='[]' \
        -Ddefault_library=static \
        -Djack=disabled \
        -Davahi=disabled \
        -Dpipewire-alsa=disabled \
        -Draop=disabled -Davb=disabled \
        -Dlibpulse=disabled \
        -Dflatpak=disabled \
        -Dsupport=enabled \
        -Dstatic=true \
        -Dlibdir=lib
fi

echo "building"
ninja -C "$pw_builddir"
echo "installing"
ninja -C "$pw_builddir" install

cat > "$builddir/native.txt" <<EOF
[built-in options]
pkg_config_path='$builddir/prefix/usr/lib/pkgconfig'
EOF

meson setup \
    --native-file "$builddir/native.txt" \
    --cross-file "$base"/misc/x86_64-w64-mingw32.txt \
    -Dlibpipewire_static_lib="$builddir/prefix/usr/lib/libpipewire-static-0.3.a" \
    "$builddir" "$base"

ninja -C "$builddir" install
