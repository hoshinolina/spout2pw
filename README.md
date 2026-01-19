# Spout2 to PipeWire bridge

This is very WIP.

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

To run with steam runtime (needs hack for libgbm problem):
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
