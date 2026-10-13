// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "requestrouter.h"

#include <QDBusConnection>
#include <QDBusMessage>

static const QString s_prefix = QStringLiteral("/org/freedesktop/portal/desktop/request");
static const QString s_interface = QStringLiteral("org.freedesktop.impl.portal.Request");

static RequestRouter *s_router = nullptr;

bool RequestRouter::install(QDBusConnection &bus)
{
    static RequestRouter router;
    if (!bus.registerVirtualObject(s_prefix, &router, QDBusConnection::SubPath))
        return false;
    s_router = &router;
    return true;
}

bool RequestRouter::add(const QString &path, QObject *request)
{
    if (!s_router || !path.startsWith(s_prefix + QLatin1Char('/')))
        return false;
    QPointer<QObject> &entry = s_router->m_requests[path];
    if (entry)
        return false;
    entry = request;
    return true;
}

void RequestRouter::remove(const QString &path)
{
    if (s_router)
        s_router->m_requests.remove(path);
}

QString RequestRouter::introspect(const QString &path) const
{
    if (!m_requests.value(path))
        return {};
    return QStringLiteral("<interface name=\"%1\"><method name=\"Close\"/></interface>").arg(s_interface);
}

bool RequestRouter::handleMessage(const QDBusMessage &message, const QDBusConnection &connection)
{
    QObject *request = m_requests.value(message.path());
    if (!request) {
        connection.send(message.createErrorReply(QDBusError::UnknownObject,
                                                 QStringLiteral("No request at %1").arg(message.path())));
        return true;
    }
    if (message.member() != QLatin1String("Close")
        || (!message.interface().isEmpty() && message.interface() != s_interface))
        return false;

    QMetaObject::invokeMethod(request, "Close", Qt::DirectConnection, Q_ARG(QDBusMessage, message));
    if (!message.isDelayedReply())
        connection.send(message.createReply());
    return true;
}
