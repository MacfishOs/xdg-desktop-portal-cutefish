#pragma once

#include <QList>
#include <QObject>
#include <QPoint>
#include <QRect>
#include <QSize>
#include <QString>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <sys/types.h>
#include <vector>

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
struct ext_image_capture_source_v1;
struct ext_foreign_toplevel_image_capture_source_manager_v1;
struct ext_foreign_toplevel_list_v1;
struct ext_foreign_toplevel_handle_v1;
struct zwp_linux_dmabuf_v1;

class QSocketNotifier;

struct CaptureOutput {
    QString name;
    QString description;
    QPoint logicalPosition;
    QSize logicalSize;
    QSize pixelSize;
    int refreshMilliHz = 0;
};

// An application window the compositor lists for capture. The identifier is
// stable for the window's lifetime and is what a selection names.
struct CaptureToplevel {
    QString identifier;
    QString title;
    QString appId;
};

// One plane of a DMA-BUF the portal allocated; the fd stays the caller's.
struct DmaBufPlane {
    int fd = -1;
    uint32_t offset = 0;
    uint32_t stride = 0;
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
    struct Toplevel;

    explicit WaylandCapture(QObject *parent = nullptr);
    ~WaylandCapture() override;

    // Connects on first use; a broken connection is dropped and retried here.
    bool connectToDisplay();
    bool connected() const { return m_display != nullptr; }
    bool captureAvailable() const;

    QList<CaptureOutput> outputs() const;
    // Empty unless the compositor lists windows to this client.
    QList<CaptureToplevel> toplevels() const;
    bool dmabufAvailable() const { return m_dmabuf != nullptr; }

    std::unique_ptr<CaptureSession> createSession(const QString &output, bool cursor);
    std::unique_ptr<CaptureSession> createToplevelSession(const QString &identifier, bool cursor);
    wl_buffer *createShmBuffer(int fd, const QSize &size, int stride);
    // A buffer over planes the caller allocated, delivered to `done` once the
    // compositor has imported it, or as null when it refused. False without
    // linux-dmabuf. A refusal is an event, not a protocol error, which is why
    // this is asynchronous.
    bool createDmaBufBuffer(const QSize &size, uint32_t format, uint64_t modifier,
                            const std::vector<DmaBufPlane> &planes,
                            std::function<void(wl_buffer *)> done);
    void flush();

Q_SIGNALS:
    void outputsChanged();
    void toplevelsChanged();
    void disconnected();

private:
    friend class CaptureSession;
    friend struct WaylandCaptureEvents;

    void handleGlobal(uint32_t name, const char *interface, uint32_t version);
    void handleGlobalRemove(uint32_t name);
    void bindXdgOutputs();
    void addToplevel(ext_foreign_toplevel_handle_v1 *handle);
    std::unique_ptr<CaptureSession> makeSession(ext_image_capture_source_v1 *source, bool cursor);
    void dispatch();
    void disconnect();

    wl_display *m_display = nullptr;
    wl_registry *m_registry = nullptr;
    wl_shm *m_shm = nullptr;
    zxdg_output_manager_v1 *m_xdgOutputs = nullptr;
    ext_output_image_capture_source_manager_v1 *m_sources = nullptr;
    ext_foreign_toplevel_image_capture_source_manager_v1 *m_toplevelSources = nullptr;
    ext_foreign_toplevel_list_v1 *m_toplevelList = nullptr;
    ext_image_copy_capture_manager_v1 *m_copies = nullptr;
    zwp_linux_dmabuf_v1 *m_dmabuf = nullptr;
    QSocketNotifier *m_notifier = nullptr;
    std::vector<std::unique_ptr<Output>> m_outputs;
    std::vector<std::unique_ptr<Toplevel>> m_toplevels;
    bool m_dispatching = false;
};

// One ext-image-copy-capture session on one source. The compositor allows a
// single frame at a time, so a capture is refused while one is in flight.
class CaptureSession : public QObject
{
    Q_OBJECT

public:
    using DmaBufFormats = std::map<uint32_t, std::vector<uint64_t>>;

    ~CaptureSession() override;

    bool ready() const { return m_ready; }
    bool isStopped() const { return m_stopped; }
    QSize bufferSize() const { return m_size; }
    bool argbSupported() const { return m_argb; }
    // DRM fourcc to modifiers the compositor renders into, with the device
    // (st_rdev of a DRM node) to allocate on; empty when SHM is all there is.
    const DmaBufFormats &dmabufFormats() const { return m_dmabufFormats; }
    dev_t dmabufDevice() const { return m_dmabufDevice; }
    bool capturing() const { return m_frame != nullptr; }

    // `damage` is what changed in the buffer since it was last filled; empty
    // means all of it.
    bool capture(wl_buffer *buffer, const QList<QRect> &damage = {});
    void cancel();

Q_SIGNALS:
    void constraintsChanged();
    void stopped();
    // `damage` is in buffer coordinates, relative to this session's previous frame.
    void frameReady(uint64_t presentationNs, uint32_t transform, const QList<QRect> &damage);
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
    DmaBufFormats m_pendingDmabufFormats;
    DmaBufFormats m_dmabufFormats;
    dev_t m_pendingDmabufDevice = 0;
    dev_t m_dmabufDevice = 0;
    bool m_ready = false;
    bool m_stopped = false;
    uint32_t m_transform = 0;
    uint64_t m_presentationNs = 0;
    QList<QRect> m_damage;
};
