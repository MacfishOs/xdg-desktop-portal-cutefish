#pragma once

#include <QDBusConnection>
#include <QDBusContext>
#include <QDBusVariant>
#include <QMap>
#include <QObject>
#include <QStringList>
#include <QVariantMap>

using SettingsMap = QMap<QString, QVariantMap>;
Q_DECLARE_METATYPE(SettingsMap)

class SettingsPortal : public QObject, protected QDBusContext
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.impl.portal.Settings")
    Q_PROPERTY(uint version READ version CONSTANT)

public:
    explicit SettingsPortal(const QDBusConnection &bus, QObject *parent = nullptr);

    uint version() const { return 1; }

public slots:
    SettingsMap ReadAll(const QStringList &namespaces) const;
    QDBusVariant Read(const QString &nameSpace, const QString &key);

signals:
    void SettingChanged(const QString &nameSpace, const QString &key, const QDBusVariant &value);

private slots:
    void setDarkMode(bool darkMode);

private:
    void refreshAppearance();

    QDBusConnection m_bus;
    uint m_colorScheme = 2;
};
