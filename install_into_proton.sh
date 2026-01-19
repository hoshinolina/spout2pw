#!/bin/sh
set -e

proton="$1"

if [ ! -d "$proton/files" ]; then
    echo "Usage: $0 <proton install base>"
    exit 1
fi

base="$(dirname "$0")"
cp -vr "$base/wine"/* "$proton"/files/lib/wine

cd "$base"

PROTONPATH="$proton" umu-run ./install_spout2pw.cmd
