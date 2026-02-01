# Spout2 to PipeWire bridge

This is very WIP. Only try this if you know what you are doing. Won't work on nvidia proprietary drivers yet either.

To build:

```bash
git submodule init
git submodule update
./build.sh
```

This creates a package at `build/pkg`.

The following steps assume you use umu-launcher and have `umu-run` setup. If you use `GAMEID` to launch your game, set that environment variable for **all** of the following steps.

To install into a Proton + Proton Prefix (for example, GE-Proton10-28 here):

```bash
build/pkg/install_into_proton.sh ~/.local/share/Steam/compatibilitytools.d/GE-Proton10-28/
```

To run with steam runtime (needs hack for libgbm problem due to a [steam runtime bug](https://github.com/ValveSoftware/steam-runtime/issues/797)):
```bash
WINEDEBUG=+spout2pw PROTONPATH=GE-Proton GBM_BACKENDS_PATH=/run/host/usr/lib64/gbm \
umu-run "VTube Studio/VTube Studio.exe"
```

To run without steam runtime:
```bash
WINEDEBUG=+spout2pw PROTONPATH=GE-Proton UMU_NO_RUNTIME=1 \
umu-run "VTube Studio/VTube Studio.exe"
```

You need to use `qpwgraph` to connect the video stream to OBS (and [obs-pwvideo](https://github.com/hoshinolina/obs-pwvideo))!!

## Build dependencies

On Debian: `sudo apt install meson ninja-build libdbus-1-dev libwine-dev mingw-w64 libgbm-dev libdrm-dev libvulkan-dev`

## Credits

Based on a prototype by tytan652:

* https://codeberg.org/tytan652/spoutdxtoc
* https://codeberg.org/tytan652/spout2xdp
