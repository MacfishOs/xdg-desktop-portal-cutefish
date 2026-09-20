#!/usr/bin/env python3
"""Exercise the ScreenCast backend's session and request handling on a private bus.

No compositor or PipeWire is reachable here (run_session.py isolates
XDG_RUNTIME_DIR), so Start can only fail cleanly; streaming is verified
against a live session."""

import os
import subprocess
import unittest

from gi.repository import Gio, GLib
import test_settings as support
from test_settings import BACKEND, SERVICE, PATH, spin_until


INTERFACE = "org.freedesktop.impl.portal.ScreenCast"
SESSION_INTERFACE = "org.freedesktop.impl.portal.Session"
REQUEST = "/org/freedesktop/portal/desktop/request/test/"
SESSION = "/org/freedesktop/portal/desktop/session/test/"
APP = "org.test.Recorder"


class ScreenCastPortalTest(unittest.TestCase):
    call = support.SettingsPortalTest.call
    bus_call = support.SettingsPortalTest.bus_call
    owned = support.SettingsPortalTest.owned

    def setUp(self):
        self.bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
        self.closed = []
        self.subscription = self.bus.signal_subscribe(
            SERVICE, SESSION_INTERFACE, "Closed", None, None, Gio.DBusSignalFlags.NONE,
            lambda connection, sender, path, iface, signal, args: self.closed.append(path))
        self.process = subprocess.Popen([BACKEND])
        self.addCleanup(self.stop)
        spin_until(self.owned)

    def stop(self):
        self.process.terminate()
        self.process.wait(timeout=5)
        self.bus.signal_unsubscribe(self.subscription)

    def connection(self):
        connection = Gio.DBusConnection.new_for_address_sync(
            os.environ["DBUS_SESSION_BUS_ADDRESS"],
            Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT |
            Gio.DBusConnectionFlags.MESSAGE_BUS_CONNECTION, None, None)
        self.addCleanup(lambda: None if connection.is_closed() else connection.close_sync(None))
        return connection

    def create(self, name, app=APP):
        return self.call(INTERFACE, "CreateSession",
                         GLib.Variant("(oosa{sv})", (REQUEST + name, SESSION + name, app, {})))

    def select(self, name, options):
        return self.call(INTERFACE, "SelectSources",
                         GLib.Variant("(oosa{sv})", (REQUEST + name, SESSION + name, APP, options)))

    def start(self, name):
        return self.call(INTERFACE, "Start",
                         GLib.Variant("(oossa{sv})", (REQUEST + name, SESSION + name, APP, "", {})))

    def session_version(self, name):
        return self.call("org.freedesktop.DBus.Properties", "Get",
                         GLib.Variant("(ss)", (SESSION_INTERFACE, "version")),
                         path=SESSION + name).unpack()[0]

    def test_properties(self):
        values = self.call("org.freedesktop.DBus.Properties", "GetAll",
                           GLib.Variant("(s)", (INTERFACE,))).unpack()[0]
        self.assertEqual(values["version"], 4)
        self.assertEqual(values["AvailableSourceTypes"], 3)
        self.assertEqual(values["AvailableCursorModes"], 3)

    def test_session_lifecycle(self):
        response, results = self.create("one").unpack()
        self.assertEqual(response, 0)
        self.assertEqual(results["session_id"], "one")
        self.assertEqual(self.session_version("one"), 1)
        # The same handle cannot be created twice, and bad paths are refused.
        self.assertEqual(self.create("one").unpack()[0], 2)
        self.assertEqual(self.call(INTERFACE, "CreateSession", GLib.Variant(
            "(oosa{sv})", (REQUEST + "x", "/somewhere/else", APP, {}))).unpack()[0], 2)

        other = self.connection()
        with self.assertRaises(GLib.Error):
            other.call_sync(SERVICE, SESSION + "one", SESSION_INTERFACE, "Close", None, None,
                            Gio.DBusCallFlags.NONE, 3000, None)
        self.assertEqual(self.session_version("one"), 1)

        self.call(SESSION_INTERFACE, "Close", path=SESSION + "one")
        with self.assertRaises(GLib.Error):
            self.session_version("one")
        self.assertEqual(self.closed, [])

    def test_owner_disconnect_frees_session(self):
        owner = self.connection()
        owner.call_sync(SERVICE, PATH, INTERFACE, "CreateSession",
                        GLib.Variant("(oosa{sv})", (REQUEST + "gone", SESSION + "gone", APP, {})),
                        None, Gio.DBusCallFlags.NONE, 3000, None)
        self.assertEqual(self.session_version("gone"), 1)
        owner.close_sync(None)
        spin_until(lambda: not self.exists("gone"))

    def exists(self, name):
        try:
            self.session_version(name)
            return True
        except GLib.Error:
            return False

    def test_select_sources_validation(self):
        self.create("types")
        # Virtual sources alone are refused; monitors or windows are fine.
        self.assertEqual(self.select("types", {"types": GLib.Variant("u", 4)}).unpack()[0], 2)
        spin_until(lambda: self.closed == [SESSION + "types"])
        self.assertFalse(self.exists("types"))
        self.create("windows")
        self.assertEqual(self.select("windows", {"types": GLib.Variant("u", 2)}).unpack()[0], 0)

        self.create("cursor")
        self.assertEqual(self.select("cursor", {"cursor_mode": GLib.Variant("u", 4)}).unpack()[0], 2)
        spin_until(lambda: SESSION + "cursor" in self.closed)

        self.create("ok")
        self.assertEqual(self.select("ok", {
            "types": GLib.Variant("u", 3),
            "cursor_mode": GLib.Variant("u", 2),
            "persist_mode": GLib.Variant("u", 2),
            "restore_data": GLib.Variant("(suv)", ("Cutefish", 1, GLib.Variant("a{sv}", {
                "monitors": GLib.Variant("as", ["eDP-1"])}))),
        }).unpack()[0], 0)
        self.assertTrue(self.exists("ok"))
        # Another connection may not drive this session.
        other = self.connection()
        response = other.call_sync(SERVICE, PATH, INTERFACE, "SelectSources",
                                   GLib.Variant("(oosa{sv})", (REQUEST + "ok", SESSION + "ok", APP, {})),
                                   None, Gio.DBusCallFlags.NONE, 3000, None).unpack()[0]
        self.assertEqual(response, 2)

    def test_start_without_compositor_fails_cleanly(self):
        self.create("start")
        self.select("start", {})
        self.assertEqual(self.start("start").unpack()[0], 2)
        # The request path is released again, and the session survives for the
        # frontend to close.
        with self.assertRaises(GLib.Error):
            self.call("org.freedesktop.impl.portal.Request", "Close", path=REQUEST + "start")
        self.assertTrue(self.exists("start"))
        self.assertEqual(self.start("unknown").unpack()[0], 2)


if __name__ == "__main__":
    unittest.main()
