#pragma once

#include <QDBusAbstractAdaptor>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QVariantMap>

class FileChooserPortal : public QDBusAbstractAdaptor
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.impl.portal.FileChooser")
    Q_PROPERTY(uint version READ version CONSTANT)

public:
    FileChooserPortal(QObject *parent, const QDBusConnection &bus);

    uint version() const { return 4; }

public slots:
    uint OpenFile(const QDBusObjectPath &handle, const QString &appId, const QString &parentWindow,
                  const QString &title, const QVariantMap &options, const QDBusMessage &message,
                  QVariantMap &results);
    uint SaveFile(const QDBusObjectPath &handle, const QString &appId, const QString &parentWindow,
                  const QString &title, const QVariantMap &options, const QDBusMessage &message,
                  QVariantMap &results);
    uint SaveFiles(const QDBusObjectPath &handle, const QString &appId, const QString &parentWindow,
                   const QString &title, const QVariantMap &options, const QDBusMessage &message,
                   QVariantMap &results);

private:
    uint choose(const QString &mode, const QDBusObjectPath &handle, const QString &appId,
                const QString &parentWindow, const QString &title, const QVariantMap &options,
                const QDBusMessage &message);

    QDBusConnection m_bus;
};
