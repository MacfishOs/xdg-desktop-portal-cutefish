# xdg-desktop-portal-cutefish

A Qt 6 backend for `xdg-desktop-portal`, providing Settings, FileChooser, idle
inhibition, and ScreenCast.
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

X11 parent identifiers attach the dialog to its requesting window, and Wayland
`wayland:<handle>` identifiers are imported through `xdg-foreign-unstable-v2`,
which parents the dialog and makes it modal. Non-local locations such as
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

## Idle inhibition

`org.freedesktop.impl.portal.Inhibit.Inhibit` supports the Idle flag (`8`),
forwarding the application ID and reason to the compositor's
`org.freedesktop.ScreenSaver.Inhibit`. Each exported request owns one cookie.
`Request.Close` or caller disconnection releases it, including when a request
is closed before the ScreenSaver reply arrives. Only the requesting connection
may close a request. ScreenSaver failures are returned as D-Bus errors.

Logout, user switching, suspend, combined flags, and session monitoring are not
implemented. Unsupported flags return `org.freedesktop.portal.Error.NotSupported`.
This backend follows the [Inhibit backend interface](https://flatpak.github.io/xdg-desktop-portal/docs/doc-org.freedesktop.impl.portal.Inhibit.html).
The compositor must include ScreenSaver inhibition support.

The inhibition tests use a private bus and mock ScreenSaver service to check
cookie lifetimes, ownership, cancellation during acquisition, and error handling.

## Screen cast

`org.freedesktop.impl.portal.ScreenCast` (version 4) lets applications share a
monitor or a window through PipeWire: OBS, Chromium and Electron applications,
Firefox, and screen sharing in Discord, Zoom or Meet all go through this
interface.

```text
application → xdg-desktop-portal (ScreenCast, OpenPipeWireRemote)
            → xdg-desktop-portal-cutefish
                 ├─ com.cutefish.Services.Permissions.Prompt   (the dialog)
                 ├─ ext-image-copy-capture-v1 → cutefish-compositor (frames)
                 └─ PipeWire Video/Source node                 (to the app)
```

No new Wayland protocol is involved: the backend is an ordinary client of the
compositor's `ext-image-capture-source-v1` / `ext-image-copy-capture-v1`
implementation, the same one `cutefish-screenshot` uses, plus the standard
`ext-foreign-toplevel-list-v1` for the windows it may capture and
`linux-dmabuf-v1` for the buffers it allocates. The compositor only announces
those globals to allowlisted executables, and it allowlists this backend at
its installed path, so a build-tree backend cannot capture a live session.

Each stream is one PipeWire `Video/Source` node at the source's pixel size,
with a variable frame rate capped at the output's refresh rate and at whatever
the consumer negotiates. Buffers are negotiated with the consumer:

- DMA-BUFs, when the compositor advertises formats it can render into and the
  consumer accepts one of their modifiers. The backend allocates them with
  GBM on the render node the compositor names, wraps them as `wl_buffer`s
  through `linux-dmabuf-v1`, and hands them to PipeWire as `DmaBuf` data; the
  compositor renders each frame straight into the buffer on the GPU. A
  modifier choice the consumer leaves open is settled by test-allocating, as
  xdg-desktop-portal-wlr does; if nothing allocates, the offer is withdrawn.
- Otherwise memfd-backed `wl_shm` buffers handed to PipeWire as `MemFd`
  (`BGRA`, with `BGRx` offered for consumers that ignore alpha), filled by a
  GPU readback in the compositor.

Frames carry `Header` (presentation time from the compositor, sequence
number), `VideoTransform` and `VideoDamage` metadata. Damage is what the
compositor tracked since the session's previous frame, up to 16 rectangles
(more collapse into their bounding box; an empty list means nothing changed);
the backend also tells the compositor what each buffer missed while other
buffers were filled. A frame is only requested while a PipeWire buffer is
free, and the throttle is one capture per frame period. Source size changes
renegotiate the stream; output removal, a closed window, or a compositor
disconnect closes the session, which the frontend sees as `Session.Closed`.

Supported: `MONITOR` and `WINDOW` sources (`AvailableSourceTypes` = 3), one
source per session, `Hidden` and `Embedded` cursor modes (`AvailableCursorModes`
= 3), `persist_mode` and `restore_data` for monitor selections (vendor
`Cutefish`, version 1, `monitors`). A window stream reports `size` and
`source_type` 2 and no position; the window is captured with its frame and
rounded corners, without shadow. Not supported: virtual sources and cursor
metadata.

### Choosing what to share

`Start` asks the user through `cutefish-services`
(`com.cutefish.Services.Permissions.Prompt`, kind `screen-recording`), which
shows the requesting application and the displays and windows to choose from,
as the application's source types allow, with Cancel and Share. There is no
standing grant: like other Wayland desktops, the backend asks for a choice
every time, and the application's own `restore_data` is what skips the
chooser:

- a monitor choice is returned with `persist_mode` and `restore_data` as the
  application requested, and a later `Start` carrying it streams those
  monitors without asking while they still exist;
- a window is chosen afresh every time: its identifier lasts one window
  lifetime, so a window choice gets no `restore_data`;
- Cancel closes the request with response 1, and the frontend closes the
  session.

The request handle accepts `Close` while the prompt is open, which dismisses
it. The session object implements `org.freedesktop.impl.portal.Session`
(`Close`, `Closed`, `version`); `Close` from the frontend or the frontend
leaving the bus stops the streams.

### Runtime requirements

PipeWire must be running in the session (the frontend also needs it for
`OpenPipeWireRemote`), `cutefish-services` must be on the bus for the prompt,
and the compositor must be one that allowlists this backend, which
`cutefish-compositor` does once both are installed under the same prefix.
`WAYLAND_DISPLAY` and `XDG_RUNTIME_DIR` must be in the D-Bus activation
environment, which the Cutefish session exports. Without a reachable compositor
or capture globals, `Start` fails with response 2.

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

Build requirements: CMake 3.18+, a C++17 compiler, Qt 6 Core/DBus development
packages, `libpipewire-0.3`, `wayland-client`, `wayland-scanner`,
`wayland-protocols` 1.39+, `gbm`, and `libdrm`. Tests additionally require
`dbus-run-session`, Python 3, and GObject/GIO bindings (`python3-gi` on
Debian). An installed `xdg-desktop-portal` enables the
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
The screen cast test covers session ownership and lifetime, `SelectSources`
validation and `Closed`, and a `Start` that fails cleanly without a compositor;
streaming itself is verified against a live session.

## Install and activate

Runtime requirements are Qt 6 Core/DBus, `xdg-desktop-portal`, `cutefish-services`,
PipeWire, and File Manager's `cutefish-filechooser` helper. Install `xdg-desktop-portal-gtk` for the other portal interfaces
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
configuration selects Cutefish for Settings, FileChooser, Inhibit, and ScreenCast,
and GTK for other interfaces.
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
