// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QDBusVirtualObject>
#include <QHash>
#include <QPointer>

class QDBusConnection;

// Serves every org.freedesktop.impl.portal.Request object. QtDBus looks a path
// up on its own thread, ahead of the queued method that registers the object
// there, so a Close sent right behind its request would find nothing. A call
// to this one object is queued too, and so handled after that method.
class RequestRouter : public QDBusVirtualObject
{
public:
    // Before the service name is taken.
    static bool install(QDBusConnection &bus);

    // A request answers Close(QDBusMessage) as a slot.
    static bool add(const QString &path, QObject *request);
    static void remove(const QString &path);

    QString introspect(const QString &path) const override;
    bool handleMessage(const QDBusMessage &message, const QDBusConnection &connection) override;

private:
    QHash<QString, QPointer<QObject>> m_requests;
};
