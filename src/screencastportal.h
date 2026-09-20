#pragma once

#include <QDBusAbstractAdaptor>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QHash>
#include <QVariantMap>

class PipeWireCore;
class ScreenCastSession;
class WaylandCapture;

class ScreenCastPortal : public QDBusAbstractAdaptor
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.impl.portal.ScreenCast")
    Q_PROPERTY(uint version READ version CONSTANT)
    Q_PROPERTY(uint AvailableSourceTypes READ availableSourceTypes CONSTANT)
    Q_PROPERTY(uint AvailableCursorModes READ availableCursorModes CONSTANT)

public:
    ScreenCastPortal(QObject *parent, const QDBusConnection &bus);
    ~ScreenCastPortal() override;

    uint version() const { return 4; }
    // Monitors and windows; the compositor exposes no virtual sources.
    uint availableSourceTypes() const { return 1 | 2; }
    // Hidden or embedded; cursor metadata is not produced.
    uint availableCursorModes() const { return 1 | 2; }

public slots:
    uint CreateSession(const QDBusObjectPath &handle, const QDBusObjectPath &sessionHandle,
                       const QString &appId, const QVariantMap &options,
                       const QDBusMessage &message, QVariantMap &results);
    uint SelectSources(const QDBusObjectPath &handle, const QDBusObjectPath &sessionHandle,
                       const QString &appId, const QVariantMap &options,
                       const QDBusMessage &message, QVariantMap &results);
    uint Start(const QDBusObjectPath &handle, const QDBusObjectPath &sessionHandle,
               const QString &appId, const QString &parentWindow, const QVariantMap &options,
               const QDBusMessage &message, QVariantMap &results);

private:
    ScreenCastSession *session(const QDBusObjectPath &handle, const QDBusMessage &message) const;

    QDBusConnection m_bus;
    WaylandCapture *m_wayland;
    PipeWireCore *m_pipewire;
    QHash<QString, ScreenCastSession *> m_sessions;
};
