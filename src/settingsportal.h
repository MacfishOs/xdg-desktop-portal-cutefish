#pragma once

#include "base/dbus.h"

#include <cstdint>
#include <string>

class SettingsPortal
{
public:
    // `servicesConfig` is cutefish-services' settings file, read once for
    // the dark mode until the live value arrives.
    SettingsPortal(cutefish::base::dbus::Connection *bus, const std::string &servicesConfig);

    bool publish();

    uint32_t version() const { return 1; }

private:
    void readAll(cutefish::base::dbus::MethodCall &call) const;
    void read(cutefish::base::dbus::MethodCall &call) const;
    void setDarkMode(bool darkMode);
    void refreshAppearance();

    cutefish::base::dbus::Connection *m_bus;
    cutefish::base::dbus::Object m_object;
    cutefish::base::dbus::Proxy m_appearance;
    cutefish::base::dbus::Match m_darkModeChanged;
    cutefish::base::dbus::NameWatch m_appearanceOwner;
    cutefish::base::dbus::PendingCall m_refresh;
    uint32_t m_colorScheme = 2;
};
