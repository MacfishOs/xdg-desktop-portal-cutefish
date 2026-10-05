#include "settingsportal.h"

#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusReply>
#include <QDBusServiceWatcher>
#include <QDebug>
#include <QSettings>
#include <QStandardPaths>

#include <algorithm>

static const QString AppearanceService = QStringLiteral("com.cutefish.Services");
static const QString AppearancePath = QStringLiteral("/com/cutefish/Services/Appearance");
static const QString AppearanceInterface = QStringLiteral("com.cutefish.Services.Appearance");
static const QString AppearanceNamespace = QStringLiteral("org.freedesktop.appearance");
static const QString ColorSchemeKey = QStringLiteral("color-scheme");

static bool matchesNamespace(const QString &pattern)
{
    return pattern.isEmpty() || pattern == QStringLiteral("*") || pattern == AppearanceNamespace
            || (pattern.endsWith(QStringLiteral(".*"))
                && AppearanceNamespace.startsWith(pattern.chopped(1)));
}

SettingsPortal::SettingsPortal(const QDBusConnection &bus, QObject *parent)
    : QObject(parent)
    , m_bus(bus)
{
    qDBusRegisterMetaType<SettingsMap>();

    QSettings settings(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
                               + QStringLiteral("/cutefish/services.conf"),
                       QSettings::IniFormat);
    settings.beginGroup(QStringLiteral("Appearance"));
    m_colorScheme = settings.value(QStringLiteral("DarkMode"), false).toBool() ? 1u : 2u;

    if (!m_bus.connect(AppearanceService, AppearancePath, AppearanceInterface,
                       QStringLiteral("darkModeChanged"), this, SLOT(setDarkMode(bool)))) {
        qWarning() << "Cannot subscribe to Cutefish dark mode changes:" << m_bus.lastError();
    }

    auto *watcher = new QDBusServiceWatcher(AppearanceService, m_bus,
                                            QDBusServiceWatcher::WatchForOwnerChange, this);
    connect(watcher, &QDBusServiceWatcher::serviceOwnerChanged, this,
            [this](const QString &, const QString &, const QString &newOwner) {
                if (!newOwner.isEmpty())
                    refreshAppearance();
            });

    refreshAppearance();
}

SettingsMap SettingsPortal::ReadAll(const QStringList &namespaces) const
{
    if (namespaces.isEmpty()
        || std::any_of(namespaces.cbegin(), namespaces.cend(), matchesNamespace)) {
        return {{AppearanceNamespace, {{ColorSchemeKey, m_colorScheme}}}};
    }
    return {};
}

QDBusVariant SettingsPortal::Read(const QString &nameSpace, const QString &key)
{
    if (nameSpace != AppearanceNamespace || key != ColorSchemeKey) {
        sendErrorReply(QStringLiteral("org.freedesktop.portal.Error.NotFound"),
                       QStringLiteral("Unknown setting: %1/%2").arg(nameSpace, key));
        return QDBusVariant(QVariant::fromValue(0u));
    }
    return QDBusVariant(QVariant::fromValue(m_colorScheme));
}

void SettingsPortal::setDarkMode(bool darkMode)
{
    const uint colorScheme = darkMode ? 1u : 2u;
    if (m_colorScheme == colorScheme)
        return;

    m_colorScheme = colorScheme;
    emit SettingChanged(AppearanceNamespace, ColorSchemeKey,
                        QDBusVariant(QVariant::fromValue(m_colorScheme)));
}

void SettingsPortal::refreshAppearance()
{
    auto message = QDBusMessage::createMethodCall(AppearanceService, AppearancePath,
                                                  QStringLiteral("org.freedesktop.DBus.Properties"),
                                                  QStringLiteral("Get"));
    message.setArguments({AppearanceInterface, QStringLiteral("isDarkMode")});
    message.setAutoStartService(false);

    // The live property can be newer than the QSettings file when a change is pending on disk.
    const QDBusReply<QDBusVariant> reply = m_bus.call(message, QDBus::Block, 1000);
    if (reply.isValid() && reply.value().variant().metaType().id() == QMetaType::Bool)
        setDarkMode(reply.value().variant().toBool());
}
