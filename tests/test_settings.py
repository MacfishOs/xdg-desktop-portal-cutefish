#!/usr/bin/env python3
"""Exercise the backend over a private D-Bus session using a mock appearance service."""

import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest

from gi.repository import Gio, GLib


BACKEND = str(Path(sys.argv.pop(1)).resolve())
SERVICE = "org.freedesktop.impl.portal.desktop.cutefish"
PATH = "/org/freedesktop/portal/desktop"
INTERFACE = "org.freedesktop.impl.portal.Settings"
NAMESPACE = "org.freedesktop.appearance"
SOURCE_SERVICE = "com.cutefish.Services"
SOURCE_PATH = "/com/cutefish/Services/Appearance"
SOURCE_INTERFACE = "com.cutefish.Services.Appearance"
SOURCE_XML = f"""<node><interface name="{SOURCE_INTERFACE}">
  <property name="isDarkMode" type="b" access="read"/>
  <signal name="darkModeChanged"><arg type="b"/></signal>
</interface></node>"""


def spin_until(predicate, timeout=5):
    deadline = time.monotonic() + timeout
    context = GLib.MainContext.default()
    while time.monotonic() < deadline:
        while context.pending():
            context.iteration(False)
        if predicate():
            return
        time.sleep(0.005)
    raise AssertionError("Timed out waiting for D-Bus activity")


class SettingsPortalTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="cutefish-portal-test-")
        self.addCleanup(self.directory.cleanup)
        self.config = Path(self.directory.name)
        self.bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
        self.process = None
        self.frontend = None
        self.source_owned = False
        self.dark = False
        self.events = []
        self.subscriptions = []
        info = Gio.DBusNodeInfo.new_for_xml(SOURCE_XML).interfaces[0]
        self.registration = self.bus.register_object(
            SOURCE_PATH, info, None,
            lambda *args: GLib.Variant("b", self.dark), None,
        )
        self.addCleanup(self.stop)

    def stop(self):
        for process in (self.frontend, self.process):
            if process is not None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=5)
        if self.source_owned:
            self.release_source()
        for subscription in self.subscriptions:
            self.bus.signal_unsubscribe(subscription)
        self.bus.unregister_object(self.registration)

    def call(self, interface, method, parameters=None, service=SERVICE, path=PATH):
        result = []

        def done(connection, pending):
            try:
                result.append(connection.call_finish(pending))
            except GLib.Error as error:
                result.append(error)

        self.bus.call(service, path, interface, method, parameters, None,
                      Gio.DBusCallFlags.NO_AUTO_START, 3000, None, done)
        spin_until(lambda: bool(result))
        if isinstance(result[0], GLib.Error):
            raise result[0]
        return result[0]

    def bus_call(self, method, parameters):
        return self.call("org.freedesktop.DBus", method, parameters,
                         "org.freedesktop.DBus", "/org/freedesktop/DBus")

    def owned(self, service=SERVICE):
        return self.bus_call("NameHasOwner", GLib.Variant("(s)", (service,))).unpack()[0]

    def own_source(self, dark):
        self.dark = dark
        result = self.bus_call("RequestName", GLib.Variant("(su)", (SOURCE_SERVICE, 4)))
        self.assertEqual(result.unpack(), (1,))
        self.source_owned = True

    def release_source(self):
        self.bus_call("ReleaseName", GLib.Variant("(s)", (SOURCE_SERVICE,)))
        self.source_owned = False

    def start(self, saved=None):
        if saved is not None:
            theme = self.config / "cutefish" / "theme.conf"
            theme.parent.mkdir()
            theme.write_text(f"[General]\nDarkMode={'true' if saved else 'false'}\n")
        environment = dict(os.environ, XDG_CONFIG_HOME=str(self.config),
                           XDG_CONFIG_DIRS=str(self.config / "empty"))
        self.process = subprocess.Popen([BACKEND], env=environment)
        spin_until(self.owned)
        self.assertIsNone(self.process.poll())

    def subscribe(self, service=SERVICE, interface=INTERFACE):
        self.subscriptions.append(self.bus.signal_subscribe(
            service, interface, "SettingChanged", PATH, None,
            Gio.DBusSignalFlags.NONE,
            lambda connection, sender, path, iface, signal, args: self.events.append(args),
        ))
        self.bus_call("GetId", None)

    def change(self, dark):
        self.dark = dark
        self.bus.emit_signal(None, SOURCE_PATH, SOURCE_INTERFACE,
                             "darkModeChanged", GLib.Variant("(b)", (dark,)))
        self.bus.flush_sync(None)

    def read(self, service=SERVICE, interface=INTERFACE, method="Read"):
        value = self.call(interface, method,
                          GLib.Variant("(ss)", (NAMESPACE, "color-scheme")), service)
        self.assertEqual(value.get_type_string(), "(v)")
        inner = value.get_child_value(0).get_variant()
        self.assertEqual(inner.get_type_string(), "u")
        return inner.unpack()

    def test_default_light_and_version(self):
        self.start()
        self.assertEqual(self.read(), 2)
        version = self.call("org.freedesktop.DBus.Properties", "Get",
                            GLib.Variant("(ss)", (INTERFACE, "version")))
        self.assertEqual(version.get_child_value(0).get_variant().get_type_string(), "u")
        self.assertEqual(version.unpack(), (1,))

    def test_saved_dark_without_service(self):
        self.start(saved=True)
        self.assertEqual(self.read(), 1)

    def test_live_property_overrides_stale_file(self):
        self.own_source(True)
        self.start(saved=False)
        self.assertEqual(self.read(), 1)

    def test_read_all_filters_and_types(self):
        self.start()
        for namespaces in ([], [""], ["*"], [NAMESPACE], ["org.*"],
                           ["org.freedesktop.*"], ["other", NAMESPACE]):
            with self.subTest(namespaces=namespaces):
                reply = self.call(INTERFACE, "ReadAll", GLib.Variant("(as)", (namespaces,)))
                self.assertEqual(reply.get_type_string(), "(a{sa{sv}})")
                self.assertEqual(reply.unpack(), ({NAMESPACE: {"color-scheme": 2}},))
        for pattern in ("other", "org.freedesktop.appearance.*", "org.*.appearance",
                        "org.freedesktop.appear*", "*appearance"):
            with self.subTest(pattern=pattern):
                reply = self.call(INTERFACE, "ReadAll", GLib.Variant("(as)", ([pattern],)))
                self.assertEqual(reply.unpack(), ({},))

    def test_unknown_settings_return_not_found(self):
        self.start()
        for namespace, key in (("unknown", "color-scheme"), (NAMESPACE, "unknown")):
            with self.assertRaises(GLib.Error) as error:
                self.call(INTERFACE, "Read", GLib.Variant("(ss)", (namespace, key)))
            self.assertEqual(Gio.DBusError.get_remote_error(error.exception),
                             "org.freedesktop.portal.Error.NotFound")

    def test_live_signals_and_duplicate_suppression(self):
        self.own_source(False)
        self.start()
        self.subscribe()
        for index, dark in enumerate((True, False), start=1):
            self.change(dark)
            spin_until(lambda: len(self.events) == index)
            value = 1 if dark else 2
            self.assertEqual(self.events[-1].get_type_string(), "(ssv)")
            self.assertEqual(self.events[-1].get_child_value(2).get_variant().get_type_string(), "u")
            self.assertEqual(self.events[-1].unpack(), (NAMESPACE, "color-scheme", value))
            self.assertEqual(self.read(), value)
        self.change(False)
        deadline = time.monotonic() + 0.15
        spin_until(lambda: time.monotonic() >= deadline)
        self.assertEqual(len(self.events), 2)

    def test_late_service_and_restart(self):
        self.start()
        self.subscribe()
        self.own_source(True)
        spin_until(lambda: len(self.events) == 1)
        self.assertEqual(self.read(), 1)
        self.release_source()
        self.assertEqual(self.read(), 1)
        self.own_source(False)
        spin_until(lambda: len(self.events) == 2)
        self.assertEqual(self.read(), 2)
        self.change(True)
        spin_until(lambda: len(self.events) == 3)
        self.assertEqual(self.read(), 1)

    def test_second_instance_fails(self):
        self.start()
        second = subprocess.run([BACKEND], capture_output=True, timeout=5)
        self.assertNotEqual(second.returncode, 0)
        self.assertIn(b"Cannot own", second.stderr)
        self.assertEqual(self.read(), 2)

    @unittest.skipUnless(Path("/usr/libexec/xdg-desktop-portal").exists(),
                         "xdg-desktop-portal frontend is not installed")
    def test_frontend_forwards_reads_and_changes(self):
        self.own_source(False)
        self.start()
        data = Path(__file__).resolve().parent.parent / "data"
        portal_config = self.config / "xdg-desktop-portal"
        portal_config.mkdir()
        (portal_config / "cutefish-portals.conf").write_text(
            "[preferred]\ndefault=none;\norg.freedesktop.impl.portal.Settings=cutefish;\n"
        )
        environment = dict(os.environ, XDG_CONFIG_HOME=str(self.config),
                           XDG_CURRENT_DESKTOP="Cutefish", XDG_DESKTOP_PORTAL_DIR=str(data))
        self.frontend = subprocess.Popen(
            ["/usr/libexec/xdg-desktop-portal"], env=environment,
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        )
        frontend_service = "org.freedesktop.portal.Desktop"
        frontend_interface = "org.freedesktop.portal.Settings"
        spin_until(lambda: self.owned(frontend_service))
        self.assertEqual(self.read(frontend_service, frontend_interface, "ReadOne"), 2)
        self.subscribe(frontend_service, frontend_interface)
        self.change(True)
        spin_until(lambda: bool(self.events))
        self.assertEqual(self.events[-1].unpack(), (NAMESPACE, "color-scheme", 1))
        self.assertEqual(self.read(frontend_service, frontend_interface, "ReadOne"), 1)


if __name__ == "__main__":
    unittest.main(verbosity=2)
