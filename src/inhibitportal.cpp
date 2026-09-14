#include "inhibitportal.h"

#include <QDBusConnectionInterface>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusReply>
#include <QDBusServiceWatcher>

namespace {
void error(const QDBusConnection &bus, const QDBusMessage &message, const QString &name,
           const QString &description)
{
    message.setDelayedReply(true);
    bus.send(message.createErrorReply(name, description));
}
} // namespace

class InhibitRequest : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.impl.portal.Request")

public:
    InhibitRequest(QObject *parent, const QDBusConnection &bus, const QDBusMessage &message,
                   const QString &path, const QString &owner)
        : QObject(parent)
        , m_bus(bus)
        , m_message(message)
        , m_path(path)
        , m_owner(owner)
    {
        auto *watcher = new QDBusServiceWatcher(message.service(), bus,
                                                QDBusServiceWatcher::WatchForUnregistration, this);
        connect(watcher, &QDBusServiceWatcher::serviceUnregistered, this, [this] {
            close();
        });
    }

    ~InhibitRequest() override { release(); }

    void start(const QString &appId, const QString &reason)
    {
        m_message.setDelayedReply(true);
        auto call = QDBusMessage::createMethodCall(
                m_owner, QStringLiteral("/org/freedesktop/ScreenSaver"),
                QStringLiteral("org.freedesktop.ScreenSaver"), QStringLiteral("Inhibit"));
        call.setArguments({appId, reason});
        auto *pending = new QDBusPendingCallWatcher(m_bus.asyncCall(call), this);
        connect(pending, &QDBusPendingCallWatcher::finished, this,
                [this](QDBusPendingCallWatcher *pending) {
                    QDBusPendingReply<uint> reply = *pending;
                    m_pending = false;
                    pending->deleteLater();
                    if (!reply.isError()) {
                        m_cookie = reply.value();
                        m_acquired = true;
                    }
                    if (m_closed) {
                        release();
                        deleteLater();
                        return;
                    }
                    if (reply.isError()) {
                        m_bus.send(m_message.createErrorReply(reply.error()));
                        m_bus.unregisterObject(m_path);
                        deleteLater();
                        return;
                    }
                    m_bus.send(m_message.createReply());
                });
    }

public slots:

    void Close(const QDBusMessage &message)
    {
        if (message.service() != m_message.service()) {
            error(m_bus, message, QStringLiteral("org.freedesktop.DBus.Error.AccessDenied"),
                  QStringLiteral("Only the requesting service may close this inhibition"));
            return;
        }
        close();
    }

private:
    void release()
    {
        if (!m_acquired)
            return;
        m_acquired = false;
        // Pin the unique owner so a restarted service cannot receive an old cookie.
        auto call = QDBusMessage::createMethodCall(
                m_owner, QStringLiteral("/org/freedesktop/ScreenSaver"),
                QStringLiteral("org.freedesktop.ScreenSaver"), QStringLiteral("UnInhibit"));
        call.setArguments({m_cookie});
        m_bus.asyncCall(call);
    }

    void close()
    {
        if (m_closed)
            return;
        m_closed = true;
        m_bus.unregisterObject(m_path);
        if (m_pending) {
            m_bus.send(m_message.createErrorReply(
                    QStringLiteral("org.freedesktop.portal.Error.Cancelled"),
                    QStringLiteral("Inhibition was closed")));
            // Keep the pending call alive to release a cookie returned after Close.
            return;
        }
        release();
        deleteLater();
    }

    QDBusConnection m_bus;
    QDBusMessage m_message;
    QString m_path;
    QString m_owner;
    uint m_cookie = 0;
    bool m_acquired = false;
    bool m_pending = true;
    bool m_closed = false;
};

InhibitPortal::InhibitPortal(QObject *parent, const QDBusConnection &bus)
    : QDBusAbstractAdaptor(parent)
    , m_bus(bus)
{
}

void InhibitPortal::Inhibit(const QDBusObjectPath &handle, const QString &appId,
                            const QString &window, uint flags, const QVariantMap &options,
                            const QDBusMessage &message)
{
    Q_UNUSED(window)
    if (flags != 8) {
        error(m_bus, message, QStringLiteral("org.freedesktop.portal.Error.NotSupported"),
              QStringLiteral("Only idle inhibition (flag 8) is supported"));
        return;
    }
    if (!handle.path().startsWith(QStringLiteral("/org/freedesktop/portal/desktop/request/"))) {
        error(m_bus, message, QStringLiteral("org.freedesktop.portal.Error.InvalidArgument"),
              QStringLiteral("Invalid request path"));
        return;
    }
    const QDBusReply<QString> owner =
            m_bus.interface()->serviceOwner(QStringLiteral("org.freedesktop.ScreenSaver"));
    if (!owner.isValid()) {
        error(m_bus, message, QStringLiteral("org.freedesktop.portal.Error.Failed"),
              QStringLiteral("ScreenSaver inhibition service is unavailable"));
        return;
    }
    auto *request = new InhibitRequest(this, m_bus, message, handle.path(), owner.value());
    if (!m_bus.registerObject(handle.path(), request, QDBusConnection::ExportAllSlots)) {
        delete request;
        error(m_bus, message, QStringLiteral("org.freedesktop.portal.Error.Failed"),
              QStringLiteral("Cannot export inhibition request"));
        return;
    }
    request->start(appId, options.value(QStringLiteral("reason")).toString());
}

#include "inhibitportal.moc"
