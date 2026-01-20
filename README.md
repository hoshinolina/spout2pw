# Spout2 to PipeWire bridge

[![Github-sponsors](https://img.shields.io/github/sponsors/hoshinolina?label=Sponsor&logo=GitHub)](https://lina.yt/sponsor)
[![Ko-Fi](https://shields.io/badge/ko--fi-Tip-ff5f5f?logo=ko-fi)](https://lina.yt/kofi)

**If you like this, please help support me with the links above!**

See the [wiki page](https://github.com/hoshinolina/spout2pw/wiki) for installation and usage instructions.

## Building

```bash
git submodule init
git submodule update
./build.sh
```

This creates a package at `build/pkg`.

## Build dependencies

On Debian: `sudo apt install meson ninja-build libdbus-1-dev libwine-dev mingw-w64 libgbm-dev libdrm-dev libvulkan-dev wine64-tools`

On Fedora: `sudo dnf install meson ninja-build dbus-devel wine-devel mingw64-gcc-c++ mesa-libgbm-devel libdrm-devel vulkan-loader-devel`

## Credits

Based on a prototype by tytan652:

* https://codeberg.org/tytan652/spoutdxtoc
* https://codeberg.org/tytan652/spout2xdp
