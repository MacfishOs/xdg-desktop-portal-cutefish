#include "pipewirecore.h"

#include <QDebug>
#include <QSocketNotifier>

#include <pipewire/pipewire.h>

static const pw_core_events coreEvents = [] {
    pw_core_events events {};
    events.version = PW_VERSION_CORE_EVENTS;
    events.error = [](void *data, uint32_t id, int, int result, const char *message) {
        if (id != PW_ID_CORE)
            return;
        qWarning() << "PipeWire core error" << result << message;
        // The connection is unusable; tear it down outside the callback.
        auto *core = static_cast<PipeWireCore *>(data);
        QMetaObject::invokeMethod(core, &PipeWireCore::fail, Qt::QueuedConnection);
    };
    return events;
}();

PipeWireCore::PipeWireCore(QObject *parent)
    : QObject(parent)
{
    pw_init(nullptr, nullptr);
}

PipeWireCore::~PipeWireCore()
{
    disconnect();
    pw_deinit();
}

bool PipeWireCore::connectToDaemon()
{
    if (m_core)
        return true;
    m_loop = pw_loop_new(nullptr);
    m_context = m_loop ? pw_context_new(m_loop, nullptr, 0) : nullptr;
    m_core = m_context ? pw_context_connect(m_context, nullptr, 0) : nullptr;
    if (!m_core) {
        qWarning() << "Cannot connect to PipeWire for screen casting";
        disconnect();
        return false;
    }
    m_listener = new spa_hook {};
    pw_core_add_listener(m_core, m_listener, &coreEvents, this);
    pw_loop_enter(m_loop);
    m_notifier = new QSocketNotifier(pw_loop_get_fd(m_loop), QSocketNotifier::Read, this);
    connect(m_notifier, &QSocketNotifier::activated, this, &PipeWireCore::iterate);
    return true;
}

void PipeWireCore::iterate()
{
    if (!m_loop || m_iterating)
        return;
    m_iterating = true;
    const int result = pw_loop_iterate(m_loop, 0);
    m_iterating = false;
    if (result < 0)
        fail();
}

void PipeWireCore::fail()
{
    if (!m_loop)
        return;
    // Streams destroy themselves against a live core before it goes away.
    Q_EMIT disconnected();
    disconnect();
}

void PipeWireCore::disconnect()
{
    if (!m_loop)
        return;
    delete m_notifier;
    m_notifier = nullptr;
    if (m_listener) {
        spa_hook_remove(m_listener);
        delete m_listener;
        m_listener = nullptr;
    }
    if (m_core) {
        pw_core_disconnect(m_core);
        m_core = nullptr;
    }
    if (m_context) {
        pw_context_destroy(m_context);
        m_context = nullptr;
    }
    pw_loop_leave(m_loop);
    pw_loop_destroy(m_loop);
    m_loop = nullptr;
}
