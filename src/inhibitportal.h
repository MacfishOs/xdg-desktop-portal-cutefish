#pragma once

#include <QDBusAbstractAdaptor>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QVariantMap>

class InhibitPortal : public QDBusAbstractAdaptor
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.impl.portal.Inhibit")

public:
    InhibitPortal(QObject *parent, const QDBusConnection &bus);

public slots:
    void Inhibit(const QDBusObjectPath &handle, const QString &appId, const QString &window,
                 uint flags, const QVariantMap &options, const QDBusMessage &message);

private:
    QDBusConnection m_bus;
};
