#pragma once

#include <QObject>

struct pw_context;
struct pw_core;
struct pw_loop;
struct spa_hook;

class QSocketNotifier;

// One PipeWire connection for all screen cast streams, iterated from the Qt
// event loop instead of a PipeWire thread so stream callbacks stay on the
// thread that owns the Wayland proxies.
class PipeWireCore : public QObject
{
    Q_OBJECT

public:
    explicit PipeWireCore(QObject *parent = nullptr);
    ~PipeWireCore() override;

    // Connects on first use; after a daemon error the next call reconnects.
    bool connectToDaemon();
    pw_core *core() const { return m_core; }

    // Drops the connection after notifying streams; the next connectToDaemon()
    // starts over.
    void fail();

Q_SIGNALS:
    void disconnected();

private:
    void iterate();
    void disconnect();

    pw_loop *m_loop = nullptr;
    pw_context *m_context = nullptr;
    pw_core *m_core = nullptr;
    spa_hook *m_listener = nullptr;
    QSocketNotifier *m_notifier = nullptr;
    bool m_iterating = false;
};
