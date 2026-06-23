#include "mjpegpipeline.h"
#include "logging.h"

#include <QMetaObject>
#include <QPointer>
#include <QSize>
#include <QVideoFrame>
#include <QVideoFrameFormat>
#include <QVideoSink>

#include <gst/app/gstappsink.h>
#include <gst/gst.h>
#include <gst/video/video.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>

namespace glasshouse {

namespace {
// Latest-frame mailbox: bounds the GUI-thread delivery reservoir. The
// appsink's drop=true / max-buffers=2 only bounds *decoded* frames waiting
// on our pull; once a frame is marshalled to the GUI thread via a queued
// connection, a slow render thread would let those deliveries pile up
// unbounded in the Qt event loop. Stashing only the freshest frame and
// keeping at most one delivery in flight means the viewer jumps to the
// latest frame instead of draining a backlog.
struct FrameMailbox {
    std::mutex           mutex;
    QVideoFrame          latest;
    std::atomic<bool>    pending{false};
    std::atomic<quint64> delivered{0};   // frames painted to the sink
    std::atomic<quint64> coalesced{0};   // frames skipped (GUI thread behind)
};
}  // namespace

struct MjpegPipeline::Impl {
    QPointer<QVideoSink>     sink;
    QPointer<MjpegPipeline>  owner;

    GstElement* pipeline = nullptr;
    GstElement* appsink  = nullptr;

    std::atomic<bool>        firstFrameEmitted{false};
    std::shared_ptr<FrameMailbox> mailbox = std::make_shared<FrameMailbox>();

    ~Impl() {
        if (pipeline) {
            gst_element_set_state(pipeline, GST_STATE_NULL);
            gst_object_unref(pipeline);
            pipeline = nullptr;
        }
    }

    // Same QVideoFrame delivery as VideoPipeline. Worth refactoring into a
    // shared util if a third pipeline shape ever lands; until then, the
    // duplication is small enough that extracting it would obscure more
    // than it saves.
    static GstFlowReturn onNewSample(GstAppSink* sink, gpointer userData) {
        auto* self = static_cast<Impl*>(userData);

        GstSample* sample = gst_app_sink_pull_sample(sink);
        if (!sample) return GST_FLOW_ERROR;

        GstBuffer* buf  = gst_sample_get_buffer(sample);
        GstCaps*   caps = gst_sample_get_caps(sample);
        if (!buf || !caps) {
            gst_sample_unref(sample);
            return GST_FLOW_OK;
        }

        GstVideoInfo vinfo;
        if (!gst_video_info_from_caps(&vinfo, caps)) {
            gst_sample_unref(sample);
            return GST_FLOW_OK;
        }

        GstMapInfo map;
        if (!gst_buffer_map(buf, &map, GST_MAP_READ)) {
            gst_sample_unref(sample);
            return GST_FLOW_OK;
        }

        QVideoFrameFormat fmt(QSize(vinfo.width, vinfo.height),
                              QVideoFrameFormat::Format_BGRA8888);
        QVideoFrame frame(fmt);
        if (frame.map(QVideoFrame::WriteOnly)) {
            uchar* dst = frame.bits(0);
            const int dstStride = frame.bytesPerLine(0);
            const int srcStride = vinfo.stride[0];
            const int rowBytes  = std::min(srcStride, dstStride);
            for (int y = 0; y < vinfo.height; ++y) {
                std::memcpy(dst + y * dstStride,
                            map.data + y * srcStride,
                            rowBytes);
            }
            frame.unmap();
        }

        gst_buffer_unmap(buf, &map);
        gst_sample_unref(sample);

        // Coalesce delivery onto the GUI thread (see FrameMailbox above):
        // stash the freshest frame and schedule at most one pending paint.
        auto mailbox = self->mailbox;
        {
            std::lock_guard<std::mutex> lk(mailbox->mutex);
            mailbox->latest = frame;
        }
        const bool wasFirst = !self->firstFrameEmitted.exchange(true);
        if (mailbox->pending.exchange(true)) {
            // A paint is already queued; this frame supersedes the previous
            // undelivered one (GUI thread behind) — count it as coalesced.
            mailbox->coalesced.fetch_add(1, std::memory_order_relaxed);
        } else {
            QPointer<QVideoSink>     sinkPtr  = self->sink;
            QPointer<MjpegPipeline>  ownerPtr = self->owner;
            QMetaObject::invokeMethod(sinkPtr,
                [mailbox, sinkPtr, ownerPtr, wasFirst]() mutable {
                    // Clear before snapshotting so a frame arriving mid-paint
                    // schedules the next delivery rather than being stranded.
                    mailbox->pending.store(false);
                    QVideoFrame f;
                    {
                        std::lock_guard<std::mutex> lk(mailbox->mutex);
                        f = mailbox->latest;
                    }
                    if (!sinkPtr) return;
                    sinkPtr->setVideoFrame(f);
                    mailbox->delivered.fetch_add(1, std::memory_order_relaxed);
                    if (wasFirst && ownerPtr) emit ownerPtr->firstFrameRendered();
                },
                Qt::QueuedConnection);
        }

        return GST_FLOW_OK;
    }

    static gboolean onBusMessage(GstBus* /*bus*/, GstMessage* msg, gpointer user) {
        auto* self = static_cast<Impl*>(user);
        switch (GST_MESSAGE_TYPE(msg)) {
            case GST_MESSAGE_ERROR: {
                GError* err = nullptr;
                gchar*  dbg = nullptr;
                gst_message_parse_error(msg, &err, &dbg);
                const QString text = QStringLiteral("GStreamer error: %1 (%2)")
                    .arg(QString::fromUtf8(err ? err->message : "unknown"),
                         QString::fromUtf8(dbg ? dbg : ""));
                qCWarning(lcVideo) << text;
                QPointer<MjpegPipeline> ownerPtr = self->owner;
                QMetaObject::invokeMethod(ownerPtr, [ownerPtr, text]() {
                    if (ownerPtr) emit ownerPtr->errorOccurred(text);
                }, Qt::QueuedConnection);
                if (err) g_error_free(err);
                if (dbg) g_free(dbg);
                break;
            }
            case GST_MESSAGE_EOS:
                qCInfo(lcVideo) << "MJPEG EOS";
                break;
            default:
                break;
        }
        return TRUE;
    }
};

MjpegPipeline::MjpegPipeline(QVideoSink* sink, QObject* parent)
    : QObject(parent), m_sink(sink) {
    if (!gst_is_initialized()) gst_init(nullptr, nullptr);
}

MjpegPipeline::~MjpegPipeline() { stop(); }

QString MjpegPipeline::start(const QString& host,
                             const QString& authCookieHeader,
                             bool insecureTls,
                             int  maxFps) {
    if (m_impl) return {};  // already started

    m_activeDecoder = QStringLiteral("jpegdec");
    qCInfo(lcVideo).noquote().nospace()
        << host << " starting MJPEG pipeline: jpegdec, frames governed by the"
        << " leaky queue (videorate cap removed)";
    if (maxFps > 0) {
        qCDebug(lcVideo).nospace() << host << " note: video.target_fps="
            << maxFps << " is not enforced on MJPEG (videorate was a no-op on"
            << " the untimestamped stream — see DESIGN §10.6)";
    }

    // Latency control is the leaky-downstream `queue` before jpegdec. MJPEG is
    // all-intra, so dropping a whole JPEG is safe; keeping only the single
    // freshest buffer (max-size-buffers=1) stops a slow decode/render from
    // backing stale frames up in souphttpsrc + the kernel socket — the real
    // source of MJPEG lag, since ustreamer only ever holds the latest frame so
    // any backlog is on us. The former `jpegparse ! videorate max-rate=N` cap
    // was removed: on ustreamer's untimestamped multipartdemux output
    // videorate never actually capped (GStreamer #720104), only added a frame
    // of latency, and froze the stream outright with drop-only=true. jpegdec
    // decodes the demuxed JPEG buffers directly. (HW JPEG decode is
    // deliberately not attempted: nvjpegdec rejects ustreamer's 4:2:2
    // subsampling — see the header note / DESIGN §10.6.)
    //
    // gst_parse_launch handles the multipartdemux→jpegdec dynamic-pad link.
    // The cookie array is set programmatically because GStreamer's
    // launch-string syntax doesn't carry GStrv values cleanly.
    const QString desc =
        QStringLiteral(
            "souphttpsrc name=src "
                "location=\"https://%1/streamer/stream\" "
                "is-live=true "
                "ssl-strict=%2 "
                "user-agent=\"glasshouse\" "
            "! multipartdemux "
            "! image/jpeg "
            "! queue leaky=downstream max-size-buffers=1 "
                    "max-size-bytes=0 max-size-time=0 "
            "! jpegdec "
            "! videoconvert "
            "! video/x-raw,format=BGRA "
            "! appsink name=sink emit-signals=true sync=false "
                      "drop=true max-buffers=1")
            .arg(host).arg(insecureTls ? "false" : "true");

    GError* err = nullptr;
    GstElement* pipeline = gst_parse_launch(desc.toUtf8().constData(), &err);
    if (!pipeline) {
        const QString msg = QStringLiteral("gst_parse_launch failed: %1")
            .arg(QString::fromUtf8(err ? err->message : "unknown"));
        if (err) g_error_free(err);
        return msg;
    }
    if (err) { g_error_free(err); err = nullptr; }

    auto impl = std::make_unique<Impl>();
    impl->pipeline = pipeline;
    impl->appsink  = gst_bin_get_by_name(GST_BIN(pipeline), "sink");
    impl->sink     = m_sink;
    impl->owner    = this;
    if (!impl->appsink) {
        return QStringLiteral("MJPEG pipeline built but appsink missing");
    }

    GstAppSinkCallbacks callbacks{};
    callbacks.new_sample = &Impl::onNewSample;
    gst_app_sink_set_callbacks(GST_APP_SINK(impl->appsink), &callbacks,
                               impl.get(), nullptr);

    // Set the auth cookie on souphttpsrc via the GStrv `cookies` property.
    // The Cookie header value coming from PiKvmClient is already
    // "auth_token=…" — exactly the form souphttpsrc wants per element.
    GstElement* httpsrc = gst_bin_get_by_name(GST_BIN(pipeline), "src");
    if (httpsrc) {
        const QByteArray cookieBa = authCookieHeader.toUtf8();
        const gchar* cookies[] = { cookieBa.constData(), nullptr };
        g_object_set(httpsrc, "cookies", cookies, nullptr);
        gst_object_unref(httpsrc);
    }

    GstBus* bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline));
    gst_bus_add_watch(bus, &Impl::onBusMessage, impl.get());
    gst_object_unref(bus);

    if (gst_element_set_state(pipeline, GST_STATE_PLAYING)
            == GST_STATE_CHANGE_FAILURE) {
        return QStringLiteral("failed to set MJPEG pipeline to PLAYING");
    }

    m_impl = std::move(impl);
    return {};
}

void MjpegPipeline::stop() {
    m_impl.reset();
}

quint64 MjpegPipeline::framesDelivered() const {
    return m_impl ? m_impl->mailbox->delivered.load(std::memory_order_relaxed) : 0;
}

quint64 MjpegPipeline::framesCoalesced() const {
    return m_impl ? m_impl->mailbox->coalesced.load(std::memory_order_relaxed) : 0;
}

}  // namespace glasshouse
