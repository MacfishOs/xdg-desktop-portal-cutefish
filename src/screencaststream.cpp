#include "screencaststream.h"

#include "pipewirecore.h"
#include "waylandcapture.h"

#include <QDebug>

#include <algorithm>
#include <ext-image-copy-capture-v1-client-protocol.h>
#include <fcntl.h>
#include <pipewire/pipewire.h>
#include <spa/buffer/meta.h>
#include <spa/param/video/format-utils.h>
#include <spa/pod/builder.h>
#include <spa/utils/result.h>
#include <sys/mman.h>
#include <unistd.h>

struct ScreenCastStream::Buffer {
    int fd = -1;
    wl_buffer *wlBuffer = nullptr;
};

struct ScreenCastStreamEvents {
    static void stateChanged(void *data, pw_stream_state previous, pw_stream_state state,
                             const char *error)
    {
        static_cast<ScreenCastStream *>(data)->handleStateChanged(previous, state, error);
    }

    static void paramChanged(void *data, uint32_t id, const spa_pod *param)
    {
        if (id == SPA_PARAM_Format && param)
            static_cast<ScreenCastStream *>(data)->handleFormat(param);
    }

    static void addBuffer(void *data, pw_buffer *buffer)
    {
        static_cast<ScreenCastStream *>(data)->addBuffer(buffer);
    }

    static void removeBuffer(void *data, pw_buffer *buffer)
    {
        static_cast<ScreenCastStream *>(data)->removeBuffer(buffer);
    }

    static void process(void *data)
    {
        static_cast<ScreenCastStream *>(data)->captureNext();
    }
};

namespace {

const pw_stream_events streamEvents = [] {
    pw_stream_events events {};
    events.version = PW_VERSION_STREAM_EVENTS;
    events.state_changed = ScreenCastStreamEvents::stateChanged;
    events.param_changed = ScreenCastStreamEvents::paramChanged;
    events.add_buffer = ScreenCastStreamEvents::addBuffer;
    events.remove_buffer = ScreenCastStreamEvents::removeBuffer;
    events.process = ScreenCastStreamEvents::process;
    return events;
}();

constexpr int bytesPerPixel = 4;

} // namespace

ScreenCastStream::ScreenCastStream(WaylandCapture *wayland, PipeWireCore *pipewire,
                                   const QString &output, bool cursor, int maxFramerate,
                                   QObject *parent)
    : QObject(parent)
    , m_wayland(wayland)
    , m_pipewire(pipewire)
    , m_output(output)
    , m_cursor(cursor)
    , m_maxFramerate(std::clamp(maxFramerate, 1, 240))
{
    m_throttle.setSingleShot(true);
    connect(&m_throttle, &QTimer::timeout, this, &ScreenCastStream::beginCapture);
    // Emitted outside PipeWire callbacks, so the stream can go right away, and
    // it must: the core is destroyed as soon as the signal returns.
    connect(pipewire, &PipeWireCore::disconnected, this, [this] {
        destroyStream();
        finish();
    });
    // Likewise the wl_buffers must be destroyed while the display exists.
    connect(wayland, &WaylandCapture::disconnected, this, [this] {
        destroyStream();
        finish();
    });
}

ScreenCastStream::~ScreenCastStream()
{
    m_finished = true;
    m_throttle.stop();
    if (m_session)
        m_session->cancel();
    destroyStream();
}

void ScreenCastStream::destroyStream()
{
    if (m_stream) {
        pw_stream_disconnect(m_stream);
        pw_stream_destroy(m_stream);
        m_stream = nullptr;
    }
    delete m_listener;
    m_listener = nullptr;
}

bool ScreenCastStream::start()
{
    if (!m_wayland->connectToDisplay() || !m_pipewire->connectToDaemon())
        return false;
    m_session = m_wayland->createSession(m_output, m_cursor);
    if (!m_session)
        return false;
    connect(m_session.get(), &CaptureSession::stopped, this, [this] {
        finish();
    });
    connect(m_session.get(), &CaptureSession::frameReady, this, &ScreenCastStream::frameReady);
    connect(m_session.get(), &CaptureSession::frameFailed, this, &ScreenCastStream::frameFailed);
    connect(m_session.get(), &CaptureSession::constraintsChanged, this, [this] {
        if (m_finished)
            return;
        if (m_session->bufferSize().isEmpty() || !m_session->argbSupported()) {
            qWarning() << "Unsupported capture constraints for" << m_output;
            finish();
            return;
        }
        if (!m_stream) {
            m_size = m_session->bufferSize();
            if (!connectStream())
                finish();
        } else if (m_session->bufferSize() != m_size) {
            m_size = m_session->bufferSize();
            updateParams();
        }
    });
    return true;
}

const spa_pod *ScreenCastStream::buildFormat(spa_pod_builder *builder) const
{
    const spa_rectangle size {uint32_t(m_size.width()), uint32_t(m_size.height())};
    const spa_fraction variable {0, 1};
    const spa_fraction minimum {1, 1};
    const spa_fraction maximum {uint32_t(m_maxFramerate), 1};
    // ARGB8888 little-endian memory order is what SPA calls BGRA; consumers that
    // ignore alpha may pick BGRx over the same bytes.
    return static_cast<const spa_pod *>(spa_pod_builder_add_object(
            builder, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat, SPA_FORMAT_mediaType,
            SPA_POD_Id(SPA_MEDIA_TYPE_video), SPA_FORMAT_mediaSubtype,
            SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw), SPA_FORMAT_VIDEO_format,
            SPA_POD_CHOICE_ENUM_Id(3, SPA_VIDEO_FORMAT_BGRA, SPA_VIDEO_FORMAT_BGRA,
                                   SPA_VIDEO_FORMAT_BGRx),
            SPA_FORMAT_VIDEO_size, SPA_POD_Rectangle(&size), SPA_FORMAT_VIDEO_framerate,
            SPA_POD_Fraction(&variable), SPA_FORMAT_VIDEO_maxFramerate,
            SPA_POD_CHOICE_RANGE_Fraction(&maximum, &minimum, &maximum)));
}

bool ScreenCastStream::connectStream()
{
    auto *properties = pw_properties_new(PW_KEY_MEDIA_CLASS, "Video/Source", PW_KEY_MEDIA_ROLE,
                                         "Screen", PW_KEY_NODE_NAME, "cutefish-screencast",
                                         PW_KEY_NODE_DESCRIPTION, qPrintable(m_output), nullptr);
    m_stream = pw_stream_new(m_pipewire->core(), "cutefish-screencast", properties);
    if (!m_stream)
        return false;
    m_listener = new spa_hook {};
    pw_stream_add_listener(m_stream, m_listener, &streamEvents, this);

    uint8_t storage[1024];
    spa_pod_builder builder = SPA_POD_BUILDER_INIT(storage, sizeof(storage));
    const spa_pod *params[] = {buildFormat(&builder)};
    const int result = pw_stream_connect(
            m_stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
            static_cast<pw_stream_flags>(PW_STREAM_FLAG_DRIVER | PW_STREAM_FLAG_ALLOC_BUFFERS),
            params, 1);
    if (result < 0) {
        qWarning() << "Cannot connect the PipeWire stream for" << m_output << spa_strerror(result);
        return false;
    }
    return true;
}

void ScreenCastStream::updateParams()
{
    uint8_t storage[1024];
    spa_pod_builder builder = SPA_POD_BUILDER_INIT(storage, sizeof(storage));
    const spa_pod *params[] = {buildFormat(&builder)};
    pw_stream_update_params(m_stream, params, 1);
}

void ScreenCastStream::handleStateChanged(int previous, int state, const char *error)
{
    switch (state) {
    case PW_STREAM_STATE_ERROR:
        qWarning() << "PipeWire stream error for" << m_output << error;
        finish();
        break;
    case PW_STREAM_STATE_UNCONNECTED:
        if (previous != PW_STREAM_STATE_CONNECTING)
            finish();
        break;
    case PW_STREAM_STATE_PAUSED:
        m_streaming = false;
        m_throttle.stop();
        if (m_current)
            returnCurrent(true);
        if (!m_announced && pw_stream_get_node_id(m_stream) != SPA_ID_INVALID) {
            m_nodeId = pw_stream_get_node_id(m_stream);
            m_announced = true;
            Q_EMIT ready();
        }
        break;
    case PW_STREAM_STATE_STREAMING:
        m_streaming = true;
        captureNext();
        break;
    default:
        break;
    }
}

void ScreenCastStream::handleFormat(const spa_pod *param)
{
    spa_video_info_raw info {};
    if (spa_format_video_raw_parse(param, &info) < 0)
        return;
    m_framerate = info.max_framerate.denom > 0
            ? int(info.max_framerate.num / info.max_framerate.denom)
            : 0;
    if (m_framerate <= 0)
        m_framerate = m_maxFramerate;
    const int stride = int(info.size.width) * bytesPerPixel;
    const int size = stride * int(info.size.height);

    uint8_t storage[1024];
    spa_pod_builder builder = SPA_POD_BUILDER_INIT(storage, sizeof(storage));
    const spa_pod *params[] = {
            static_cast<const spa_pod *>(spa_pod_builder_add_object(
                    &builder, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
                    SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(4, 2, 8),
                    SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1), SPA_PARAM_BUFFERS_size,
                    SPA_POD_Int(size), SPA_PARAM_BUFFERS_stride, SPA_POD_Int(stride),
                    SPA_PARAM_BUFFERS_align, SPA_POD_Int(16), SPA_PARAM_BUFFERS_dataType,
                    SPA_POD_CHOICE_FLAGS_Int(1 << SPA_DATA_MemFd))),
            static_cast<const spa_pod *>(spa_pod_builder_add_object(
                    &builder, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta, SPA_PARAM_META_type,
                    SPA_POD_Id(SPA_META_Header), SPA_PARAM_META_size,
                    SPA_POD_Int(sizeof(spa_meta_header)))),
            static_cast<const spa_pod *>(spa_pod_builder_add_object(
                    &builder, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta, SPA_PARAM_META_type,
                    SPA_POD_Id(SPA_META_VideoTransform), SPA_PARAM_META_size,
                    SPA_POD_Int(sizeof(spa_meta_videotransform)))),
    };
    pw_stream_update_params(m_stream, params, 3);
}

void ScreenCastStream::addBuffer(pw_buffer *buffer)
{
    spa_data *data = buffer->buffer->datas;
    const int stride = m_size.width() * bytesPerPixel;
    const int size = stride * m_size.height();
    if (buffer->buffer->n_datas < 1 || !(data[0].type & (1 << SPA_DATA_MemFd)) || size <= 0) {
        finish();
        return;
    }
    const int fd = memfd_create("cutefish-screencast", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0 || ftruncate(fd, size) != 0) {
        if (fd >= 0)
            close(fd);
        finish();
        return;
    }
    // The compositor maps this through wl_shm; a shrinking file would fault it.
    fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_SEAL);
    auto *entry = new Buffer {fd, m_wayland->createShmBuffer(fd, m_size, stride)};
    buffer->user_data = entry;
    data[0].type = SPA_DATA_MemFd;
    data[0].fd = fd;
    data[0].maxsize = uint32_t(size);
    data[0].mapoffset = 0;
    data[0].flags = SPA_DATA_FLAG_READABLE | SPA_DATA_FLAG_MAPPABLE;
    data[0].data = nullptr;
    data[0].chunk->offset = 0;
    data[0].chunk->stride = stride;
    data[0].chunk->size = uint32_t(size);
}

void ScreenCastStream::removeBuffer(pw_buffer *buffer)
{
    auto *entry = static_cast<Buffer *>(buffer->user_data);
    if (buffer == m_current) {
        if (m_session)
            m_session->cancel();
        m_current = nullptr;
    }
    if (entry) {
        if (entry->wlBuffer)
            wl_buffer_destroy(entry->wlBuffer);
        if (entry->fd >= 0)
            close(entry->fd);
        delete entry;
    }
    buffer->user_data = nullptr;
    buffer->buffer->datas[0].fd = -1;
}

void ScreenCastStream::captureNext()
{
    if (m_finished || !m_streaming || m_current || m_throttle.isActive() || !m_session
        || m_session->capturing())
        return;
    m_current = pw_stream_dequeue_buffer(m_stream);
    const int interval = 1000 / std::max(m_framerate, 1);
    if (!m_current) {
        // Every buffer is with the consumer; look again after one frame period.
        m_throttle.start(interval);
        return;
    }
    const int wait = m_lastCapture.isValid() ? interval - int(m_lastCapture.elapsed()) : 0;
    if (wait > 0)
        m_throttle.start(wait);
    else
        beginCapture();
}

void ScreenCastStream::beginCapture()
{
    if (m_finished || !m_streaming)
        return;
    if (!m_current) {
        captureNext();
        return;
    }
    auto *entry = static_cast<Buffer *>(m_current->user_data);
    m_lastCapture.restart();
    if (!entry || !entry->wlBuffer || !m_session->capture(entry->wlBuffer))
        returnCurrent(true);
}

void ScreenCastStream::frameReady(uint64_t presentationNs, uint32_t transform)
{
    if (!m_current)
        return;
    spa_buffer *buffer = m_current->buffer;
    if (auto *header = static_cast<spa_meta_header *>(
                spa_buffer_find_meta_data(buffer, SPA_META_Header, sizeof(spa_meta_header)))) {
        header->pts = int64_t(presentationNs);
        header->flags = 0;
        header->seq = m_sequence++;
        header->dts_offset = 0;
    }
    // wl_output transforms and SPA_META_TRANSFORMATION_* share their numbering.
    if (auto *videoTransform = static_cast<spa_meta_videotransform *>(spa_buffer_find_meta_data(
                buffer, SPA_META_VideoTransform, sizeof(spa_meta_videotransform))))
        videoTransform->transform = transform;
    buffer->datas[0].chunk->flags = SPA_CHUNK_FLAG_NONE;
    pw_stream_queue_buffer(m_stream, m_current);
    m_current = nullptr;
    pw_stream_trigger_process(m_stream);
}

void ScreenCastStream::frameFailed(uint32_t reason)
{
    if (reason == EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_STOPPED) {
        finish();
        return;
    }
    returnCurrent(true);
    // A discarded frame or a stale buffer size is transient; renegotiation, if
    // any, arrives through the session constraints.
    m_throttle.start(1000 / std::max(m_framerate, 1));
}

void ScreenCastStream::returnCurrent(bool corrupted)
{
    if (!m_current)
        return;
    if (m_session)
        m_session->cancel();
    m_current->buffer->datas[0].chunk->flags =
            corrupted ? SPA_CHUNK_FLAG_CORRUPTED : SPA_CHUNK_FLAG_NONE;
    pw_stream_queue_buffer(m_stream, m_current);
    m_current = nullptr;
}

void ScreenCastStream::finish()
{
    if (m_finished)
        return;
    m_finished = true;
    m_throttle.stop();
    if (m_session)
        m_session->cancel();
    // Callers may be PipeWire callbacks, where the stream cannot be destroyed.
    QMetaObject::invokeMethod(
            this,
            [this] {
                destroyStream();
                m_session.reset();
                if (m_announced)
                    Q_EMIT stopped();
                else
                    Q_EMIT failed();
            },
            Qt::QueuedConnection);
}
