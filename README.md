# xdg-desktop-portal-cutefish

A Qt 6 backend for `xdg-desktop-portal`, providing Settings and FileChooser.
The Settings interface is
`org.freedesktop.impl.portal.Settings`, version 1, exposing
`org.freedesktop.appearance/color-scheme` as a D-Bus `uint32`:

| Cutefish dark mode | Portal value |
| --- | --- |
| Enabled | `1` (prefer dark) |
| Disabled | `2` (prefer light) |

The backend implements `Read`, namespace-filtered `ReadAll`, and
`SettingChanged`, following the [Settings backend specification](https://flatpak.github.io/xdg-desktop-portal/docs/doc-org.freedesktop.impl.portal.Settings.html).
The backend depends on Qt Core and Qt D-Bus.
File chooser UI runs in File Manager's separate `cutefish-filechooser` process.

## File chooser

`org.freedesktop.impl.portal.FileChooser` implements `OpenFile`, `SaveFile`, and
`SaveFiles`. It delegates each request to `/usr/libexec/cutefish-filechooser`, built
and installed by the sibling `filemanager` project. The dialog uses File Manager's
folder and places models and FishUI appearance, without opening a browser window.

Supported features include file/folder selection, multiple selection, path
navigation, mounted places, hidden files, glob and MIME filters, custom choices,
suggested names/locations, and overwrite confirmation. `SaveFiles` selects a
destination folder and returns unused names for collisions. The chooser never
writes the selected save file; the requesting application performs the save.

Requests run asynchronously and independently. Closing a portal request kills its
dialog and returns cancellation. Helper launch/crash/protocol failures return
response `2`; only successful, local `file://` URIs are forwarded. The portal
frontend handles sandbox access and document export.

X11 parent identifiers attach the dialog to its requesting window. Wayland foreign
parent handles are not yet attached, so the dialog opens as a separate window on
Wayland; cross-process modality is not enforced there. Non-local locations such as
Trash are excluded. Native filenames must be representable as Unicode for the
private JSON protocol.

Build/install both `filemanager` and this project, then restart the backend and
frontend to load the new executable and portal metadata:

```sh
systemctl --user restart xdg-desktop-portal-cutefish.service
systemctl --user restart xdg-desktop-portal.service
```

For development, `--file-chooser-helper /path/to/cutefish-filechooser` selects a
built helper without installing it. See `filemanager/chooser/README.md` for UI
build instructions and the private helper protocol.

## How updates reach applications

```text
Cutefish Settings → com.cutefish.Services.Appearance.darkModeChanged(bool)
                 → xdg-desktop-portal-cutefish
                 → xdg-desktop-portal: org.freedesktop.portal.Settings.SettingChanged
                 → applications consuming the system color-scheme preference
```

At startup, the backend reads `DarkMode` from the existing Cutefish QSettings
configuration (`$XDG_CONFIG_HOME/cutefish/theme.conf`, normally
`~/.config/cutefish/theme.conf`). It then reads `isDarkMode` from
`com.cutefish.Services` at `/com/cutefish/Services/Appearance`, on interface
`com.cutefish.Services.Appearance`. The live value takes precedence over the file.

Changes are forwarded immediately from the signal payload, without waiting for
the settings file to be flushed. Duplicate values do not emit notifications.
If Cutefish services exit, the last known preference is retained; the property
is read again when a new service owner appears. The backend also supports starting
before Cutefish services. Editing `theme.conf` directly while the backend runs
does not generate a notification.

Applications must consume the Settings portal and follow the system preference.
This backend does not force a theme onto applications with their own appearance
override or add portal support to older GTK applications. Cutefish services retain
their existing GTK settings-file handling.

## Build and test

Build requirements: CMake 3.18+, a C++17 compiler, and Qt 6 Core/DBus development
packages. Tests additionally require `dbus-run-session`, Python 3, and GObject/GIO
bindings (`python3-gi` on Debian). An installed `xdg-desktop-portal` enables the
frontend forwarding test.

Run from this repository:

```sh
cmake -S . -B /tmp/xdg-desktop-portal-cutefish-build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/usr \
  -DPython3_EXECUTABLE=/usr/bin/python3
cmake --build /tmp/xdg-desktop-portal-cutefish-build
ctest --test-dir /tmp/xdg-desktop-portal-cutefish-build --output-on-failure
```

Use `-DBUILD_TESTING=OFF` to build without test dependencies. Tests run on a private
session bus with temporary settings and a mock Cutefish appearance service; they
do not change the running desktop's theme. They cover initial values, D-Bus types,
filters, errors, live notifications, duplicate suppression, late startup, service
restart, duplicate process rejection, and forwarding through the portal frontend.

## Install and activate

Runtime requirements are Qt 6 Core/DBus, `xdg-desktop-portal`, `cutefish-services`,
and File Manager's `cutefish-filechooser` helper. Install `xdg-desktop-portal-gtk` for the other portal interfaces
selected by the supplied fallback configuration.

```sh
sudo cmake --install /tmp/xdg-desktop-portal-cutefish-build
```

The install includes:

- `libexec/xdg-desktop-portal-cutefish`
- `share/dbus-1/services/org.freedesktop.impl.portal.desktop.cutefish.service`
- `lib/systemd/user/xdg-desktop-portal-cutefish.service`
- `share/xdg-desktop-portal/portals/cutefish.portal`
- `share/xdg-desktop-portal/cutefish-portals.conf`

Paths are relative to the configured install prefix. `SYSTEMD_USER_UNIT_DIR` is
configurable for distributions using another unit directory. Use `DESTDIR` to
stage packaging files under `/tmp`; configure the final prefix before building
because activation files contain absolute executable paths.

The service starts on demand through D-Bus and does not need to be enabled.
Log out and back into Cutefish after installation. For an existing systemd user
session, reload the units and restart the portal frontend:

```sh
systemctl --user daemon-reload
systemctl --user restart xdg-desktop-portal.service
```

The activation environment must contain `XDG_CURRENT_DESKTOP=Cutefish`, which
Cutefish's session script already exports. The supplied desktop-specific
configuration selects Cutefish for Settings and FileChooser, and GTK for other interfaces.
Existing higher-priority user configurations may override this selection; see
[portals.conf](https://flatpak.github.io/xdg-desktop-portal/docs/portals.conf.html).

Read the preference through the public portal (requires frontend version 1.18+
for `ReadOne`):

```sh
gdbus call --session --dest org.freedesktop.portal.Desktop \
  --object-path /org/freedesktop/portal/desktop \
  --method org.freedesktop.portal.Settings.ReadOne \
  org.freedesktop.appearance color-scheme
```

Monitor changes while switching dark mode in Cutefish Settings:

```sh
gdbus monitor --session --dest org.freedesktop.portal.Desktop \
  --object-path /org/freedesktop/portal/desktop
```

Older portal frontends use the legacy `Read` method; `cutefish.portal` also includes
the legacy `UseIn=Cutefish` backend selection hint.
