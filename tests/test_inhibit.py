#!/usr/bin/env python3
"""Exercise inhibition request lifetimes against an isolated ScreenSaver mock."""

import os
from pathlib import Path
import subprocess
import tempfile
import unittest

from gi.repository import Gio, GLib
import test_settings as support
from test_settings import BACKEND, SERVICE, spin_until


INTERFACE = "org.freedesktop.impl.portal.Inhibit"
SOURCE = "org.freedesktop.ScreenSaver"
REQUEST = "/org/freedesktop/portal/desktop/request/test/"
XML = """<node><interface name="org.freedesktop.ScreenSaver">
<method name="Inhibit"><arg type="s" direction="in"/><arg type="s" direction="in"/>
<arg type="u" direction="out"/></method>
<method name="UnInhibit"><arg type="u" direction="in"/></method>
</interface></node>"""


class InhibitPortalTest(unittest.TestCase):
    call = support.SettingsPortalTest.call
    bus_call = support.SettingsPortalTest.bus_call
    owned = support.SettingsPortalTest.owned
    connection = support.SettingsPortalTest.connection

    def setUp(self):
        self.bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
        self.cookies = {}
        self.next_cookie = 0
        self.delayed = None
        self.delay = False
        self.fail = False
        info = Gio.DBusNodeInfo.new_for_xml(XML).interfaces[0]
        self.registration = self.bus.register_object(
            "/org/freedesktop/ScreenSaver", info, self.source_call, None, None)
        self.bus_call("RequestName", GLib.Variant("(su)", (SOURCE, 4)))
        self.process = subprocess.Popen([BACKEND])
        self.addCleanup(self.stop)
        spin_until(self.owned)

    def stop(self):
        self.process.terminate()
        self.process.wait(timeout=5)
        self.bus_call("ReleaseName", GLib.Variant("(s)", (SOURCE,)))
        self.bus.unregister_object(self.registration)

    def source_call(self, connection, sender, path, interface, method, parameters, invocation):
        if method == "UnInhibit":
            self.cookies.pop(parameters.unpack()[0], None)
            invocation.return_value(GLib.Variant("()", ()))
        elif self.fail:
            invocation.return_dbus_error("org.freedesktop.DBus.Error.Failed", "Unavailable")
        else:
            self.next_cookie += 1
            self.cookies[self.next_cookie] = parameters.unpack()
            if self.delay:
                self.delayed = (invocation, self.next_cookie)
            else:
                invocation.return_value(GLib.Variant("(u)", (self.next_cookie,)))

    def parameters(self, name, flags=8):
        return GLib.Variant("(ossua{sv})", (REQUEST + name, "org.test.Player", "", flags,
                            {"reason": GLib.Variant("s", "Playing video")}))

    def inhibit(self, name, flags=8):
        return self.call(INTERFACE, "Inhibit", self.parameters(name, flags))

    def close(self, name):
        return self.call("org.freedesktop.impl.portal.Request", "Close", path=REQUEST + name)

    def test_multiple_requests_close_and_duplicate_path(self):
        self.assertEqual(self.inhibit("one").unpack(), ())
        self.inhibit("two")
        self.assertEqual(list(self.cookies.values()), [("org.test.Player", "Playing video")] * 2)
        with self.assertRaises(GLib.Error):
            self.inhibit("one")
        self.close("one")
        spin_until(lambda: len(self.cookies) == 1)
        self.close("two")
        spin_until(lambda: not self.cookies)

    def test_flags_and_source_failure(self):
        for flags in (0, 1, 2, 4, 16):
            with self.assertRaises(GLib.Error):
                self.inhibit("flags", flags)
        self.assertFalse(self.cookies)
        self.inhibit("both", 12)
        self.close("both")
        spin_until(lambda: not self.cookies)
        self.fail = True
        with self.assertRaises(GLib.Error):
            self.inhibit("failure")
        self.fail = False
        self.inhibit("failure")
        self.close("failure")
        spin_until(lambda: not self.cookies)

    def test_owner_and_disconnect(self):
        owner = self.bus
        self.bus = self.connection()
        try:
            self.inhibit("owner")
            connection = self.bus
            self.bus = owner
            with self.assertRaises(GLib.Error):
                self.close("owner")
            self.assertEqual(len(self.cookies), 1)
            connection.close_sync(None)
            spin_until(lambda: not self.cookies)
        finally:
            self.bus = owner

    def test_close_before_cookie_arrives(self):
        self.delay = True
        replies = []

        def done(connection, pending):
            try:
                replies.append(connection.call_finish(pending))
            except GLib.Error as error:
                replies.append(error)

        self.bus.call(SERVICE, "/org/freedesktop/portal/desktop", INTERFACE, "Inhibit",
                      self.parameters("pending"), None, Gio.DBusCallFlags.NONE, 3000, None, done)
        spin_until(lambda: self.delayed is not None)
        self.close("pending")
        spin_until(lambda: bool(replies))
        self.assertIsInstance(replies[0], GLib.Error)
        invocation, cookie = self.delayed
        invocation.return_value(GLib.Variant("(u)", (cookie,)))
        spin_until(lambda: not self.cookies)

    def test_close_right_after_inhibit(self):
        replies = {}

        def finished(name):
            def done(connection, pending):
                try:
                    replies[name] = connection.call_finish(pending)
                except GLib.Error as error:
                    replies[name] = error
            return done

        # Sent back to back, as the frontend does when the app closes at once.
        self.bus.call(SERVICE, "/org/freedesktop/portal/desktop", INTERFACE, "Inhibit",
                      self.parameters("early"), None, Gio.DBusCallFlags.NONE, 3000, None,
                      finished("inhibit"))
        self.bus.call(SERVICE, REQUEST + "early", "org.freedesktop.impl.portal.Request", "Close",
                      None, None, Gio.DBusCallFlags.NONE, 3000, None, finished("close"))
        spin_until(lambda: len(replies) == 2)
        # assert* would reach the fail attribute these tests shadow.
        if isinstance(replies["close"], GLib.Error):
            raise AssertionError(replies["close"].message)
        spin_until(lambda: not self.cookies)

    @unittest.skipUnless(Path("/usr/libexec/xdg-desktop-portal").exists(),
                         "xdg-desktop-portal frontend is not installed")
    def test_public_frontend_inhibit_and_close(self):
        with tempfile.TemporaryDirectory(prefix="cutefish-inhibit-frontend-") as directory:
            config = Path(directory) / "xdg-desktop-portal"
            config.mkdir()
            (config / "cutefish-portals.conf").write_text(
                "[preferred]\ndefault=none;\norg.freedesktop.impl.portal.Inhibit=cutefish;\n")
            environment = dict(os.environ, XDG_CONFIG_HOME=directory,
                               XDG_CURRENT_DESKTOP="Cutefish",
                               XDG_DESKTOP_PORTAL_DIR=str(Path(__file__).resolve().parent.parent / "data"))
            frontend = subprocess.Popen(["/usr/libexec/xdg-desktop-portal"], env=environment,
                                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            try:
                service = "org.freedesktop.portal.Desktop"
                spin_until(lambda: self.owned(service))
                reply = self.call("org.freedesktop.portal.Inhibit", "Inhibit",
                                  GLib.Variant("(sua{sv})", ("", 8,
                                      {"reason": GLib.Variant("s", "Public portal test")})),
                                  service=service)
                spin_until(lambda: len(self.cookies) == 1)
                self.assertEqual(next(iter(self.cookies.values()))[1], "Public portal test")
                self.call("org.freedesktop.portal.Request", "Close", service=service,
                          path=reply.unpack()[0])
                spin_until(lambda: not self.cookies)
            finally:
                frontend.terminate()
                frontend.wait(timeout=5)


if __name__ == "__main__":
    unittest.main()
