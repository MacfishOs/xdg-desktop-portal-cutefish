#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QSize>
#include <QString>
#include <QTimer>

#include <cstdint>
#include <memory>

struct pw_buffer;
struct pw_stream;
struct spa_hook;
struct spa_pod;
struct spa_pod_builder;

class CaptureSession;
class PipeWireCore;
class WaylandCapture;

// One monitor exported as a PipeWire video source. Frames are copied by the
// compositor into memfd-backed wl_shm buffers that PipeWire hands out to the
// consumer, so a frame is only requested while a buffer is free.
class ScreenCastStream : public QObject
{
    Q_OBJECT

public:
    ScreenCastStream(WaylandCapture *wayland, PipeWireCore *pipewire, const QString &output,
                     bool cursor, int maxFramerate, QObject *parent = nullptr);
    ~ScreenCastStream() override;

    bool start();
    uint32_t nodeId() const { return m_nodeId; }

Q_SIGNALS:
    // Exactly one of these fires before the node id is known.
    void ready();
    void failed();
    // After ready(): the compositor or PipeWire ended the stream.
    void stopped();

private:
    struct Buffer;
    friend struct ScreenCastStreamEvents;

    bool connectStream();
    void updateParams();
    const spa_pod *buildFormat(spa_pod_builder *builder) const;
    void handleStateChanged(int previous, int state, const char *error);
    void handleFormat(const spa_pod *param);
    void addBuffer(pw_buffer *buffer);
    void removeBuffer(pw_buffer *buffer);
    void captureNext();
    void beginCapture();
    void frameReady(uint64_t presentationNs, uint32_t transform);
    void frameFailed(uint32_t reason);
    void returnCurrent(bool corrupted);
    void destroyStream();
    void finish();

    WaylandCapture *m_wayland;
    PipeWireCore *m_pipewire;
    QString m_output;
    bool m_cursor;
    int m_maxFramerate;
    std::unique_ptr<CaptureSession> m_session;
    pw_stream *m_stream = nullptr;
    spa_hook *m_listener = nullptr;
    QSize m_size;
    uint32_t m_nodeId = 0;
    bool m_announced = false;
    bool m_streaming = false;
    bool m_finished = false;
    int m_framerate = 0;
    uint64_t m_sequence = 0;
    pw_buffer *m_current = nullptr;
    QElapsedTimer m_lastCapture;
    QTimer m_throttle;
};
