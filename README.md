# xdg-desktop-portal-cutefish

The `xdg-desktop-portal` backend for Cutefish: Settings, FileChooser, Inhibit
and ScreenCast.

## Dependencies

It is built on the base library of
[cutefish-toolkit](https://github.com/cutefishos/cutefish-toolkit); install it
first.

```sh
sudo apt install cmake ninja-build pkg-config libsystemd-dev libpipewire-0.3-dev \
    libwayland-dev wayland-protocols libgbm-dev libdrm-dev
```

At runtime it needs `xdg-desktop-portal`, `xdg-desktop-portal-gtk`, PipeWire,
cutefish-services and the File Manager daemon (file dialogs). Tests also need
`dbus-run-session` and GoogleTest (`libgtest-dev`).

## Build

```sh
cmake -S . -B build -G Ninja -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build
sudo cmake --install build
```

## Tests

```sh
ctest --test-dir build --output-on-failure
```

## License

This project has been licensed by GPLv3.
