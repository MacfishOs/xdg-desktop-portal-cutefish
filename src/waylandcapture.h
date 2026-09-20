#pragma once

#include <QList>
#include <QObject>
#include <QPoint>
#include <QSize>
#include <QString>

#include <cstdint>
#include <memory>

struct wl_buffer;
struct wl_display;
struct wl_output;
struct wl_registry;
struct wl_shm;
struct zxdg_output_manager_v1;
struct ext_image_copy_capture_manager_v1;
struct ext_image_copy_capture_session_v1;
struct ext_image_copy_capture_frame_v1;
struct ext_output_image_capture_source_manager_v1;

class QSocketNotifier;

struct CaptureOutput {
    QString name;
    QString description;
    QPoint logicalPosition;
    QSize logicalSize;
    QSize pixelSize;
    int refreshMilliHz = 0;
};

class CaptureSession;

// One connection to the compositor for the whole portal, driven from the Qt
// event loop. It carries output enumeration and the capture globals, which the
// compositor only announces to allowlisted executables.
class WaylandCapture : public QObject
{
    Q_OBJECT

public:
    struct Output;

    explicit WaylandCapture(QObject *parent = nullptr);
    ~WaylandCapture() override;

    // Connects on first use; a broken connection is dropped and retried here.
    bool connectToDisplay();
    bool connected() const { return m_display != nullptr; }
    bool captureAvailable() const;

    QList<CaptureOutput> outputs() const;
    std::unique_ptr<CaptureSession> createSession(const QString &output, bool cursor);
    wl_buffer *createShmBuffer(int fd, const QSize &size, int stride);
    void flush();

Q_SIGNALS:
    void outputsChanged();
    void disconnected();

private:
    friend class CaptureSession;
    friend struct WaylandCaptureEvents;

    void handleGlobal(uint32_t name, const char *interface, uint32_t version);
    void handleGlobalRemove(uint32_t name);
    void bindXdgOutputs();
    void dispatch();
    void disconnect();

    wl_display *m_display = nullptr;
    wl_registry *m_registry = nullptr;
    wl_shm *m_shm = nullptr;
    zxdg_output_manager_v1 *m_xdgOutputs = nullptr;
    ext_output_image_capture_source_manager_v1 *m_sources = nullptr;
    ext_image_copy_capture_manager_v1 *m_copies = nullptr;
    QSocketNotifier *m_notifier = nullptr;
    std::vector<std::unique_ptr<Output>> m_outputs;
    bool m_dispatching = false;
};

// One ext-image-copy-capture session on one output. The compositor allows a
// single frame at a time, so a capture is refused while one is in flight.
class CaptureSession : public QObject
{
    Q_OBJECT

public:
    ~CaptureSession() override;

    bool ready() const { return m_ready; }
    bool isStopped() const { return m_stopped; }
    QSize bufferSize() const { return m_size; }
    bool argbSupported() const { return m_argb; }
    bool capturing() const { return m_frame != nullptr; }

    bool capture(wl_buffer *buffer);
    void cancel();

Q_SIGNALS:
    void constraintsChanged();
    void stopped();
    void frameReady(uint64_t presentationNs, uint32_t transform);
    void frameFailed(uint32_t reason);

private:
    friend class WaylandCapture;
    friend struct CaptureSessionEvents;
    CaptureSession(WaylandCapture *wayland, ext_image_copy_capture_session_v1 *session);

    WaylandCapture *m_wayland;
    ext_image_copy_capture_session_v1 *m_session;
    ext_image_copy_capture_frame_v1 *m_frame = nullptr;
    QSize m_pendingSize;
    QSize m_size;
    bool m_pendingArgb = false;
    bool m_argb = false;
    bool m_ready = false;
    bool m_stopped = false;
    uint32_t m_transform = 0;
    uint64_t m_presentationNs = 0;
};
