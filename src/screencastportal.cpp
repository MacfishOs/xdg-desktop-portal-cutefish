#include "screencastportal.h"
#include "pipewirecore.h"
#include "requesterwatch.h"
#include "screencaststream.h"
#include "screencasttypes.h"
#include "waylandcapture.h"

#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDebug>
#include <QStringList>

#include <climits>
#include <cmath>

namespace {

constexpr uint sourceMonitor = 1;
constexpr uint sourceWindow = 2;
constexpr uint cursorHidden = 1;
constexpr uint cursorEmbedded = 2;
constexpr uint persistNone = 0;
constexpr uint persistUntilRevoked = 2;

QStringList stringList(const QVariant &value)
{
    return value.canConvert<QDBusArgument>() ? qdbus_cast<QStringList>(value)
                                             : value.toStringList();
}

QStringList restoredMonitors(const QVariant &value)
{
    if (!value.canConvert<QDBusArgument>())
        return {};
    const auto data = qdbus_cast<RestoreData>(value);
    if (data.vendor != QLatin1String("Cutefish") || data.version != 1)
        return {};
    const QVariant payload = data.data.variant();
    const QVariantMap map = payload.canConvert<QDBusArgument>() ? qdbus_cast<QVariantMap>(payload)
                                                                : payload.toMap();
    return stringList(map.value(QStringLiteral("monitors")));
}

QVariant restoreData(const QStringList &monitors)
{
    RestoreData data;
    data.vendor = QStringLiteral("Cutefish");
    data.version = 1;
    data.data = QDBusVariant(QVariantMap {{QStringLiteral("monitors"), monitors}});
    return QVariant::fromValue(data);
}

int framerateFor(const CaptureOutput &output)
{
    return output.refreshMilliHz > 0 ? int(std::ceil(output.refreshMilliHz / 1000.0)) : 60;
}

const CaptureOutput *findOutput(const QList<CaptureOutput> &outputs, const QString &name)
{
    const auto it = std::find_if(outputs.begin(), outputs.end(), [&](const CaptureOutput &output) {
        return output.name == name;
    });
    return it != outputs.end() ? &*it : nullptr;
}

QDBusMessage permissionsCall(const QString &method)
{
    return QDBusMessage::createMethodCall(QStringLiteral("com.cutefish.Services"),
                                          QStringLiteral("/com/cutefish/Services/Permissions"),
                                          QStringLiteral("com.cutefish.Services.Permissions"),
                                          method);
}

} // namespace

class StartRequest;

class ScreenCastSession : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.impl.portal.Session")
    Q_PROPERTY(uint version READ version CONSTANT)

public:
    ScreenCastSession(const QDBusConnection &bus, const QString &path, const QString &owner,
                      const QString &appId, QObject *parent)
        : QObject(parent)
        , bus(bus)
        , path(path)
        , owner(owner)
        , appId(appId)
    {
        watchRequester(this, bus, owner, [this] {
            close(false);
        });
    }

    uint version() const { return 1; }

    // Frees the exported path and the streams. The frontend learns about a
    // close it did not ask for through Closed.
    void close(bool notify);

public slots:
    void Close(const QDBusMessage &message)
    {
        if (message.service() != owner) {
            message.setDelayedReply(true);
            bus.send(message.createErrorReply(QStringLiteral("org.freedesktop.DBus.Error.AccessDenied"),
                                              QStringLiteral("Only the requesting service may close this session")));
            return;
        }
        close(false);
    }

public:
    QDBusConnection bus;
    QString path;
    QString owner;
    QString appId;
    uint cursorMode = cursorHidden;
    uint persistMode = persistNone;
    bool started = false;
    bool closed = false;
    uint types = sourceMonitor;
    QStringList restore;
    QList<ScreenCastStream *> streams;
    StartRequest *request = nullptr;
};

// The Start request: the permission prompt, then the streams, then one reply.
class StartRequest : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.impl.portal.Request")

public:
    StartRequest(const QDBusConnection &bus, const QDBusMessage &message, const QString &path,
                 ScreenCastSession *session, WaylandCapture *wayland, PipeWireCore *pipewire)
        : QObject(session)
        , bus(bus)
        , message(message)
        , path(path)
        , session(session)
        , wayland(wayland)
        , pipewire(pipewire)
    {
        session->request = this;
    }

    ~StartRequest() override { session->request = nullptr; }

    void run(const QString &parentWindow)
    {
        if (!wayland->connectToDisplay() || !wayland->captureAvailable()) {
            qWarning() << "Screen capture is unavailable on this Wayland display";
            finish(2);
            return;
        }
        const auto outputs = wayland->outputs();
        if (outputs.isEmpty()) {
            finish(2);
            return;
        }
        const auto known = [&](const QString &name) {
            return findOutput(outputs, name) != nullptr;
        };
        // The only thing that skips the chooser is the application's own
        // restore token naming monitors that still exist. Windows are chosen
        // afresh every time, their identifiers being good for one lifetime.
        if ((session->types & sourceMonitor) && !session->restore.isEmpty()
            && std::all_of(session->restore.begin(), session->restore.end(), known)) {
            QList<CaptureTarget> targets;
            for (const QString &name : session->restore)
                targets.append({CaptureTarget::Kind::Monitor, name});
            startStreams(targets, session->persistMode);
            return;
        }
        QVariantList windows;
        if (session->types & sourceWindow) {
            for (const auto &toplevel : wayland->toplevels()) {
                windows.append(QVariantMap {{QStringLiteral("identifier"), toplevel.identifier},
                                            {QStringLiteral("title"), toplevel.title},
                                            {QStringLiteral("app_id"), toplevel.appId}});
            }
        }
        QVariantList monitors;
        for (const auto &output : outputs) {
            monitors.append(QVariantMap {{QStringLiteral("name"), output.name},
                                         {QStringLiteral("description"), output.description},
                                         {QStringLiteral("x"), output.logicalPosition.x()},
                                         {QStringLiteral("y"), output.logicalPosition.y()},
                                         {QStringLiteral("width"), output.logicalSize.width()},
                                         {QStringLiteral("height"), output.logicalSize.height()}});
        }
        auto call = permissionsCall(QStringLiteral("Prompt"));
        QVariantMap options {{QStringLiteral("types"), session->types}};
        if (session->types & sourceMonitor)
            options.insert(QStringLiteral("monitors"), monitors);
        if (session->types & sourceWindow)
            options.insert(QStringLiteral("windows"), windows);
        call.setArguments({path, QStringLiteral("screen-recording"), session->appId, parentWindow,
                           options});
        // The user answers in their own time.
        prompting = true;
        auto *pending = new QDBusPendingCallWatcher(bus.asyncCall(call, INT_MAX), this);
        connect(pending, &QDBusPendingCallWatcher::finished, this,
                [this](QDBusPendingCallWatcher *pending) {
                    QDBusPendingReply<uint, QVariantMap> reply = *pending;
                    pending->deleteLater();
                    prompting = false;
                    if (finished)
                        return;
                    if (reply.isError()) {
                        qWarning() << "Permission prompt failed:" << reply.error().message();
                        finish(2);
                        return;
                    }
                    answered(reply.argumentAt<0>(), reply.argumentAt<1>());
                });
    }

    void finish(uint response, const QVariantMap &results = {})
    {
        if (finished)
            return;
        finished = true;
        bus.unregisterObject(path);
        bus.send(message.createReply({response, results}));
        if (prompting) {
            auto call = permissionsCall(QStringLiteral("Dismiss"));
            call.setArguments({path});
            bus.asyncCall(call);
        }
        if (response != 0) {
            qDeleteAll(pending);
            pending.clear();
        }
        deleteLater();
    }

public slots:
    void Close(const QDBusMessage &call)
    {
        if (call.service() != message.service()) {
            call.setDelayedReply(true);
            bus.send(call.createErrorReply(QStringLiteral("org.freedesktop.DBus.Error.AccessDenied"),
                                           QStringLiteral("Only the requesting service may close this request")));
            return;
        }
        finish(1);
    }

private:
    void answered(uint response, const QVariantMap &results)
    {
        if (response != 0) {
            finish(response == 1 ? 1 : 2);
            return;
        }
        const QStringList monitors = stringList(results.value(QStringLiteral("monitors")));
        const QStringList windows = stringList(results.value(QStringLiteral("windows")));
        QList<CaptureTarget> targets;
        for (const QString &name : monitors)
            targets.append({CaptureTarget::Kind::Monitor, name});
        for (const QString &identifier : windows)
            targets.append({CaptureTarget::Kind::Window, identifier});
        // A token can only name monitors, so a window choice gets none.
        startStreams(targets, windows.isEmpty() ? session->persistMode : persistNone);
    }

private:
    void startStreams(const QList<CaptureTarget> &targets, uint persist)
    {
        const auto outputs = wayland->outputs();
        const auto toplevels = wayland->toplevels();
        // A window is painted at its output's rate; the fastest is close enough.
        int windowFramerate = 1;
        for (const auto &output : outputs)
            windowFramerate = std::max(windowFramerate, framerateFor(output));
        QList<CaptureTarget> selected;
        for (const auto &target : targets) {
            int framerate = windowFramerate;
            if (target.kind == CaptureTarget::Kind::Monitor) {
                const CaptureOutput *output = findOutput(outputs, target.name);
                if (!output)
                    continue;
                framerate = framerateFor(*output);
            } else if (std::none_of(toplevels.begin(), toplevels.end(), [&](const auto &toplevel) {
                           return toplevel.identifier == target.name;
                       })) {
                continue;
            }
            if (std::any_of(selected.begin(), selected.end(), [&](const CaptureTarget &other) {
                    return other.kind == target.kind && other.name == target.name;
                }))
                continue;
            selected.append(target);
            auto *stream = new ScreenCastStream(wayland, pipewire, target,
                                                session->cursorMode == cursorEmbedded, framerate,
                                                this);
            pending.append(stream);
            connect(stream, &ScreenCastStream::failed, this, [this] {
                finish(2);
            });
            connect(stream, &ScreenCastStream::ready, this, [this] {
                deliver();
            });
            connect(stream, &ScreenCastStream::stopped, this, [this] {
                finish(2);
            });
            if (!stream->start()) {
                finish(2);
                return;
            }
        }
        if (selected.isEmpty()) {
            finish(2);
            return;
        }
        granted = persist;
    }

    void deliver()
    {
        if (finished)
            return;
        for (auto *stream : pending) {
            if (!stream->nodeId())
                return;
        }
        const auto outputs = wayland->outputs();
        QList<StreamInfo> streams;
        QStringList monitors;
        for (auto *stream : pending) {
            const CaptureTarget &target = stream->target();
            QVariantMap properties;
            if (target.kind == CaptureTarget::Kind::Monitor) {
                monitors.append(target.name);
                properties.insert(QStringLiteral("source_type"), sourceMonitor);
                if (const CaptureOutput *output = findOutput(outputs, target.name)) {
                    properties.insert(QStringLiteral("position"),
                                      QVariant::fromValue(IntPair {output->logicalPosition.x(),
                                                                   output->logicalPosition.y()}));
                    properties.insert(QStringLiteral("size"),
                                      QVariant::fromValue(IntPair {output->logicalSize.width(),
                                                                   output->logicalSize.height()}));
                }
            } else {
                properties.insert(QStringLiteral("source_type"), sourceWindow);
                properties.insert(QStringLiteral("size"),
                                  QVariant::fromValue(IntPair {stream->size().width(),
                                                               stream->size().height()}));
            }
            streams.append({stream->nodeId(), properties});
        }
        QVariantMap results {{QStringLiteral("streams"), QVariant::fromValue(streams)}};
        results.insert(QStringLiteral("persist_mode"), granted);
        if (granted != persistNone)
            results.insert(QStringLiteral("restore_data"), restoreData(monitors));
        // The session owns the streams from here; a stream ending closes it.
        for (auto *stream : pending) {
            stream->setParent(session);
            disconnect(stream, nullptr, this, nullptr);
            connect(stream, &ScreenCastStream::stopped, session, [session = this->session] {
                session->close(true);
            });
        }
        session->streams = pending;
        session->started = true;
        pending.clear();
        finish(0, results);
    }

    QDBusConnection bus;
    QDBusMessage message;
    QString path;
    ScreenCastSession *session;
    WaylandCapture *wayland;
    PipeWireCore *pipewire;
    QList<ScreenCastStream *> pending;
    uint granted = persistNone;
    bool prompting = false;
    bool finished = false;
};

void ScreenCastSession::close(bool notify)
{
    if (closed)
        return;
    closed = true;
    if (request)
        request->finish(1);
    qDeleteAll(streams);
    streams.clear();
    // Sent by hand: QtDBus relays emitted signals by object path from its own
    // thread, which would race the unregistration below.
    if (notify)
        bus.send(QDBusMessage::createSignal(path,
                                            QStringLiteral("org.freedesktop.impl.portal.Session"),
                                            QStringLiteral("Closed")));
    bus.unregisterObject(path);
    deleteLater();
}

ScreenCastPortal::ScreenCastPortal(QObject *parent, const QDBusConnection &bus)
    : QDBusAbstractAdaptor(parent)
    , m_bus(bus)
    , m_wayland(new WaylandCapture(this))
    , m_pipewire(new PipeWireCore(this))
{
    registerScreenCastTypes();
}

ScreenCastPortal::~ScreenCastPortal()
{
    // Streams go before the connections they use. Each deletion removes its
    // entry from m_sessions, so the deletions run over a copy.
    qDeleteAll(m_sessions.values());
}

ScreenCastSession *ScreenCastPortal::session(const QDBusObjectPath &handle,
                                             const QDBusMessage &message) const
{
    auto *session = m_sessions.value(handle.path());
    return session && session->owner == message.service() ? session : nullptr;
}

uint ScreenCastPortal::CreateSession(const QDBusObjectPath &handle,
                                     const QDBusObjectPath &sessionHandle, const QString &appId,
                                     const QVariantMap &options, const QDBusMessage &message,
                                     QVariantMap &results)
{
    Q_UNUSED(handle)
    Q_UNUSED(options)
    const QString path = sessionHandle.path();
    if (!path.startsWith(QStringLiteral("/org/freedesktop/portal/desktop/session/"))
        || m_sessions.contains(path))
        return 2;
    auto *session = new ScreenCastSession(m_bus, path, message.service(), appId, this);
    if (!m_bus.registerObject(path, session,
                              QDBusConnection::ExportAllSlots | QDBusConnection::ExportAllSignals
                                      | QDBusConnection::ExportAllProperties)) {
        delete session;
        return 2;
    }
    m_sessions.insert(path, session);
    connect(session, &QObject::destroyed, this, [this, path] {
        m_sessions.remove(path);
    });
    results.insert(QStringLiteral("session_id"), path.section(QLatin1Char('/'), -1));
    return 0;
}

uint ScreenCastPortal::SelectSources(const QDBusObjectPath &handle,
                                     const QDBusObjectPath &sessionHandle, const QString &appId,
                                     const QVariantMap &options, const QDBusMessage &message,
                                     QVariantMap &results)
{
    Q_UNUSED(handle)
    Q_UNUSED(appId)
    Q_UNUSED(results)
    auto *session = this->session(sessionHandle, message);
    if (!session || session->started)
        return 2;
    const uint types = options.value(QStringLiteral("types"), sourceMonitor).toUInt();
    const uint cursorMode = options.value(QStringLiteral("cursor_mode"), cursorHidden).toUInt();
    const uint persistMode = options.value(QStringLiteral("persist_mode"), persistNone).toUInt();
    if (!(types & availableSourceTypes()) || (cursorMode & ~availableCursorModes())
        || cursorMode == 0 || persistMode > persistUntilRevoked) {
        session->close(true);
        return 2;
    }
    session->types = types & availableSourceTypes();
    session->cursorMode = cursorMode;
    session->persistMode = persistMode;
    session->restore = restoredMonitors(options.value(QStringLiteral("restore_data")));
    return 0;
}

uint ScreenCastPortal::Start(const QDBusObjectPath &handle, const QDBusObjectPath &sessionHandle,
                             const QString &appId, const QString &parentWindow,
                             const QVariantMap &options, const QDBusMessage &message,
                             QVariantMap &results)
{
    Q_UNUSED(appId)
    Q_UNUSED(options)
    Q_UNUSED(results)
    auto *session = this->session(sessionHandle, message);
    if (!session || session->started
        || !handle.path().startsWith(QStringLiteral("/org/freedesktop/portal/desktop/request/")))
        return 2;
    auto *request = new StartRequest(m_bus, message, handle.path(), session, m_wayland, m_pipewire);
    if (!m_bus.registerObject(handle.path(), request, QDBusConnection::ExportAllSlots)) {
        delete request;
        return 2;
    }
    message.setDelayedReply(true);
    request->run(parentWindow);
    return 0;
}

#include "screencastportal.moc"
