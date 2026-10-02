// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusServiceWatcher>

#include <functional>

// Runs gone when service has no owner. It can run twice, so it must ignore a repeat.
inline void watchRequester(QObject *context, const QDBusConnection &bus, const QString &service,
                           std::function<void()> gone)
{
    auto *watcher = new QDBusServiceWatcher(service, bus,
                                            QDBusServiceWatcher::WatchForUnregistration, context);
    QObject::connect(watcher, &QDBusServiceWatcher::serviceUnregistered, context, gone);
    // The watch misses a requester that left before its match rule reached the bus;
    // NameHasOwner, sent behind that rule on the same connection, does not.
    auto *check = new QDBusPendingCallWatcher(
            bus.interface()->asyncCall(QStringLiteral("NameHasOwner"), service), context);
    QObject::connect(check, &QDBusPendingCallWatcher::finished, context,
                     [gone = std::move(gone)](QDBusPendingCallWatcher *call) {
                         const QDBusPendingReply<bool> reply = *call;
                         call->deleteLater();
                         if (!reply.isError() && !reply.value())
                             gone();
                     });
}
