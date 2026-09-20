#include "waylandcapture.h"

#include <QDebug>
#include <QSocketNotifier>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <ext-image-capture-source-v1-client-protocol.h>
#include <ext-image-copy-capture-v1-client-protocol.h>
#include <wayland-client.h>
#include <xdg-output-unstable-v1-client-protocol.h>

struct WaylandCapture::Output {
    wl_output *proxy = nullptr;
    zxdg_output_v1 *xdgOutput = nullptr;
    uint32_t global = 0;
    CaptureOutput pending;
    CaptureOutput current;
    bool done = false;
};

namespace {

const wl_output_listener outputListener = {
        [](void *data, wl_output *, int32_t x, int32_t y, int32_t, int32_t, int32_t, const char *,
           const char *, int32_t) {
            auto *output = static_cast<WaylandCapture::Output *>(data);
            if (!output->xdgOutput)
                output->pending.logicalPosition = QPoint(x, y);
        },
        [](void *data, wl_output *, uint32_t flags, int32_t width, int32_t height,
           int32_t refresh) {
            auto *output = static_cast<WaylandCapture::Output *>(data);
            if (!(flags & WL_OUTPUT_MODE_CURRENT))
                return;
            output->pending.pixelSize = QSize(width, height);
            output->pending.refreshMilliHz = refresh;
            if (!output->xdgOutput)
                output->pending.logicalSize = QSize(width, height);
        },
        [](void *data, wl_output *) {
            auto *output = static_cast<WaylandCapture::Output *>(data);
            output->current = output->pending;
            output->done = true;
        },
        [](void *, wl_output *, int32_t) {},
        [](void *data, wl_output *, const char *name) {
            static_cast<WaylandCapture::Output *>(data)->pending.name = QString::fromUtf8(name);
        },
        [](void *data, wl_output *, const char *description) {
            static_cast<WaylandCapture::Output *>(data)->pending.description =
                    QString::fromUtf8(description);
        }};

const zxdg_output_v1_listener xdgOutputListener = {
        [](void *data, zxdg_output_v1 *, int32_t x, int32_t y) {
            static_cast<WaylandCapture::Output *>(data)->pending.logicalPosition = QPoint(x, y);
        },
        [](void *data, zxdg_output_v1 *, int32_t width, int32_t height) {
            static_cast<WaylandCapture::Output *>(data)->pending.logicalSize =
                    QSize(width, height);
        },
        // Version 3 folds this into wl_output.done.
        [](void *, zxdg_output_v1 *) {},
        [](void *, zxdg_output_v1 *, const char *) {},
        [](void *, zxdg_output_v1 *, const char *) {}};

} // namespace

struct WaylandCaptureEvents {
    static void global(void *data, wl_registry *, uint32_t name, const char *interface,
                       uint32_t version)
    {
        static_cast<WaylandCapture *>(data)->handleGlobal(name, interface, version);
    }

    static void globalRemove(void *data, wl_registry *, uint32_t name)
    {
        static_cast<WaylandCapture *>(data)->handleGlobalRemove(name);
    }
};

struct CaptureSessionEvents {
    static void bufferSize(void *data, ext_image_copy_capture_session_v1 *, uint32_t width,
                           uint32_t height)
    {
        auto *session = static_cast<CaptureSession *>(data);
        session->m_pendingSize = width > 0 && height > 0 && width <= 16384 && height <= 16384
                ? QSize(int(width), int(height))
                : QSize();
        session->m_pendingArgb = false;
    }

    static void shmFormat(void *data, ext_image_copy_capture_session_v1 *, uint32_t format)
    {
        if (format == WL_SHM_FORMAT_ARGB8888)
            static_cast<CaptureSession *>(data)->m_pendingArgb = true;
    }

    static void dmabufDevice(void *, ext_image_copy_capture_session_v1 *, wl_array *) {}

    static void dmabufFormat(void *, ext_image_copy_capture_session_v1 *, uint32_t, wl_array *) {}

    static void done(void *data, ext_image_copy_capture_session_v1 *)
    {
        auto *session = static_cast<CaptureSession *>(data);
        session->m_size = session->m_pendingSize;
        session->m_argb = session->m_pendingArgb;
        session->m_ready = true;
        Q_EMIT session->constraintsChanged();
    }

    static void stopped(void *data, ext_image_copy_capture_session_v1 *)
    {
        auto *session = static_cast<CaptureSession *>(data);
        if (session->m_stopped)
            return;
        session->m_stopped = true;
        Q_EMIT session->stopped();
    }

    static void transform(void *data, ext_image_copy_capture_frame_v1 *, uint32_t transform)
    {
        static_cast<CaptureSession *>(data)->m_transform = transform;
    }

    static void damage(void *, ext_image_copy_capture_frame_v1 *, int32_t, int32_t, int32_t,
                       int32_t)
    {
    }

    static void presentationTime(void *data, ext_image_copy_capture_frame_v1 *, uint32_t hi,
                                 uint32_t lo, uint32_t nsec)
    {
        static_cast<CaptureSession *>(data)->m_presentationNs =
                ((uint64_t(hi) << 32) | lo) * 1000000000ull + nsec;
    }

    static void ready(void *data, ext_image_copy_capture_frame_v1 *)
    {
        auto *session = static_cast<CaptureSession *>(data);
        ext_image_copy_capture_frame_v1_destroy(session->m_frame);
        session->m_frame = nullptr;
        Q_EMIT session->frameReady(session->m_presentationNs, session->m_transform);
    }

    static void failed(void *data, ext_image_copy_capture_frame_v1 *, uint32_t reason)
    {
        auto *session = static_cast<CaptureSession *>(data);
        ext_image_copy_capture_frame_v1_destroy(session->m_frame);
        session->m_frame = nullptr;
        Q_EMIT session->frameFailed(reason);
    }
};

namespace {

const wl_registry_listener registryListener = {WaylandCaptureEvents::global,
                                               WaylandCaptureEvents::globalRemove};

const ext_image_copy_capture_session_v1_listener sessionListener = {
        CaptureSessionEvents::bufferSize,   CaptureSessionEvents::shmFormat,
        CaptureSessionEvents::dmabufDevice, CaptureSessionEvents::dmabufFormat,
        CaptureSessionEvents::done,         CaptureSessionEvents::stopped};

const ext_image_copy_capture_frame_v1_listener frameListener = {
        CaptureSessionEvents::transform, CaptureSessionEvents::damage,
        CaptureSessionEvents::presentationTime, CaptureSessionEvents::ready,
        CaptureSessionEvents::failed};

} // namespace

WaylandCapture::WaylandCapture(QObject *parent)
    : QObject(parent)
{
}

WaylandCapture::~WaylandCapture()
{
    disconnect();
}

bool WaylandCapture::connectToDisplay()
{
    if (m_display)
        return true;
    m_display = wl_display_connect(nullptr);
    if (!m_display) {
        qWarning() << "Cannot connect to the Wayland display for screen capture";
        return false;
    }
    m_registry = wl_display_get_registry(m_display);
    wl_registry_add_listener(m_registry, &registryListener, this);
    // Globals, then the output events the bindings trigger.
    if (wl_display_roundtrip(m_display) < 0 || wl_display_roundtrip(m_display) < 0) {
        disconnect();
        return false;
    }
    m_notifier = new QSocketNotifier(wl_display_get_fd(m_display), QSocketNotifier::Read, this);
    connect(m_notifier, &QSocketNotifier::activated, this, &WaylandCapture::dispatch);
    return true;
}

bool WaylandCapture::captureAvailable() const
{
    return m_display && m_shm && m_sources && m_copies;
}

void WaylandCapture::handleGlobal(uint32_t name, const char *interface, uint32_t version)
{
    if (!strcmp(interface, wl_output_interface.name)) {
        auto output = std::make_unique<Output>();
        output->global = name;
        output->proxy = static_cast<wl_output *>(
                wl_registry_bind(m_registry, name, &wl_output_interface, std::min(version, 4u)));
        wl_output_add_listener(output->proxy, &outputListener, output.get());
        m_outputs.push_back(std::move(output));
        bindXdgOutputs();
    } else if (!strcmp(interface, wl_shm_interface.name)) {
        m_shm = static_cast<wl_shm *>(wl_registry_bind(m_registry, name, &wl_shm_interface, 1));
    } else if (!strcmp(interface, zxdg_output_manager_v1_interface.name)) {
        m_xdgOutputs = static_cast<zxdg_output_manager_v1 *>(wl_registry_bind(
                m_registry, name, &zxdg_output_manager_v1_interface, std::min(version, 3u)));
        bindXdgOutputs();
    } else if (!strcmp(interface, ext_output_image_capture_source_manager_v1_interface.name)) {
        m_sources = static_cast<ext_output_image_capture_source_manager_v1 *>(wl_registry_bind(
                m_registry, name, &ext_output_image_capture_source_manager_v1_interface, 1));
    } else if (!strcmp(interface, ext_image_copy_capture_manager_v1_interface.name)) {
        m_copies = static_cast<ext_image_copy_capture_manager_v1 *>(
                wl_registry_bind(m_registry, name, &ext_image_copy_capture_manager_v1_interface, 1));
    }
}

void WaylandCapture::handleGlobalRemove(uint32_t name)
{
    const auto it = std::find_if(m_outputs.begin(), m_outputs.end(), [name](const auto &output) {
        return output->global == name;
    });
    if (it == m_outputs.end())
        return;
    if ((*it)->xdgOutput)
        zxdg_output_v1_destroy((*it)->xdgOutput);
    wl_output_destroy((*it)->proxy);
    m_outputs.erase(it);
    Q_EMIT outputsChanged();
}

void WaylandCapture::bindXdgOutputs()
{
    if (!m_xdgOutputs)
        return;
    for (auto &output : m_outputs) {
        if (output->xdgOutput)
            continue;
        output->xdgOutput = zxdg_output_manager_v1_get_xdg_output(m_xdgOutputs, output->proxy);
        zxdg_output_v1_add_listener(output->xdgOutput, &xdgOutputListener, output.get());
    }
}

QList<CaptureOutput> WaylandCapture::outputs() const
{
    QList<CaptureOutput> result;
    for (const auto &output : m_outputs) {
        if (output->done && !output->current.name.isEmpty())
            result.append(output->current);
    }
    return result;
}

std::unique_ptr<CaptureSession> WaylandCapture::createSession(const QString &name, bool cursor)
{
    if (!captureAvailable())
        return nullptr;
    const auto it = std::find_if(m_outputs.begin(), m_outputs.end(), [&](const auto &output) {
        return output->done && output->current.name == name;
    });
    if (it == m_outputs.end())
        return nullptr;
    auto *source = ext_output_image_capture_source_manager_v1_create_source(m_sources, (*it)->proxy);
    auto *session = ext_image_copy_capture_manager_v1_create_session(
            m_copies, source, cursor ? EXT_IMAGE_COPY_CAPTURE_MANAGER_V1_OPTIONS_PAINT_CURSORS : 0);
    // The session keeps its own reference to the source.
    ext_image_capture_source_v1_destroy(source);
    std::unique_ptr<CaptureSession> result(new CaptureSession(this, session));
    ext_image_copy_capture_session_v1_add_listener(session, &sessionListener, result.get());
    flush();
    return result;
}

wl_buffer *WaylandCapture::createShmBuffer(int fd, const QSize &size, int stride)
{
    if (!m_shm)
        return nullptr;
    auto *pool = wl_shm_create_pool(m_shm, fd, stride * size.height());
    auto *buffer = wl_shm_pool_create_buffer(pool, 0, size.width(), size.height(), stride,
                                             WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    return buffer;
}

void WaylandCapture::flush()
{
    if (m_display && wl_display_flush(m_display) < 0 && errno != EAGAIN)
        disconnect();
}

void WaylandCapture::dispatch()
{
    if (!m_display || m_dispatching)
        return;
    m_dispatching = true;
    bool failed = false;
    while (wl_display_prepare_read(m_display) != 0) {
        if (wl_display_dispatch_pending(m_display) < 0) {
            failed = true;
            break;
        }
    }
    if (!failed) {
        wl_display_flush(m_display);
        failed = wl_display_read_events(m_display) < 0
                || wl_display_dispatch_pending(m_display) < 0;
    }
    m_dispatching = false;
    if (failed)
        disconnect();
    else
        flush();
}

void WaylandCapture::disconnect()
{
    if (!m_display)
        return;
    qWarning() << "Lost the Wayland display used for screen capture";
    // Sessions release their proxies while the display still exists.
    Q_EMIT disconnected();
    delete m_notifier;
    m_notifier = nullptr;
    for (auto &output : m_outputs) {
        if (output->xdgOutput)
            zxdg_output_v1_destroy(output->xdgOutput);
        wl_output_destroy(output->proxy);
    }
    m_outputs.clear();
    if (m_copies)
        ext_image_copy_capture_manager_v1_destroy(m_copies);
    if (m_sources)
        ext_output_image_capture_source_manager_v1_destroy(m_sources);
    if (m_xdgOutputs)
        zxdg_output_manager_v1_destroy(m_xdgOutputs);
    if (m_shm)
        wl_shm_destroy(m_shm);
    wl_registry_destroy(m_registry);
    wl_display_disconnect(m_display);
    m_copies = nullptr;
    m_sources = nullptr;
    m_xdgOutputs = nullptr;
    m_shm = nullptr;
    m_registry = nullptr;
    m_display = nullptr;
    Q_EMIT outputsChanged();
}

CaptureSession::CaptureSession(WaylandCapture *wayland, ext_image_copy_capture_session_v1 *session)
    : m_wayland(wayland)
    , m_session(session)
{
    connect(wayland, &WaylandCapture::disconnected, this, [this] {
        if (m_frame)
            ext_image_copy_capture_frame_v1_destroy(m_frame);
        m_frame = nullptr;
        ext_image_copy_capture_session_v1_destroy(m_session);
        m_session = nullptr;
        if (!m_stopped) {
            m_stopped = true;
            Q_EMIT stopped();
        }
    });
}

CaptureSession::~CaptureSession()
{
    cancel();
    if (m_session)
        ext_image_copy_capture_session_v1_destroy(m_session);
    m_wayland->flush();
}

bool CaptureSession::capture(wl_buffer *buffer)
{
    if (!m_session || m_stopped || m_frame || !buffer)
        return false;
    m_frame = ext_image_copy_capture_session_v1_create_frame(m_session);
    ext_image_copy_capture_frame_v1_add_listener(m_frame, &frameListener, this);
    ext_image_copy_capture_frame_v1_attach_buffer(m_frame, buffer);
    ext_image_copy_capture_frame_v1_damage_buffer(m_frame, 0, 0, m_size.width(), m_size.height());
    ext_image_copy_capture_frame_v1_capture(m_frame);
    m_wayland->flush();
    return true;
}

void CaptureSession::cancel()
{
    if (!m_frame)
        return;
    ext_image_copy_capture_frame_v1_destroy(m_frame);
    m_frame = nullptr;
    m_wayland->flush();
}
