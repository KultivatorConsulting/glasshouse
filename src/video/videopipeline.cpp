#include "videopipeline.h"
#include "logging.h"

#include <QHostAddress>
#include <QMetaObject>
#include <QNetworkInterface>
#include <QPointer>
#include <QRegularExpression>
#include <QSize>
#include <QVideoFrame>
#include <QVideoFrameFormat>
#include <QVideoSink>

#include <gst/app/gstappsink.h>
#include <gst/gst.h>
#include <gst/sdp/sdp.h>
#include <gst/video/video.h>
#include <gst/webrtc/webrtc.h>

#include <algorithm>
#include <atomic>
#include <cstring>

namespace glasshouse {

namespace {

// Probe available H.264 decoder factories in descending preference order.
// Same policy as the previous H.264-over-WS path — HW first, libav fallback.
QString pickDecoder() {
    static constexpr const char* kCandidates[] = {
        "nvh264dec",
        "vah264dec",
        "avdec_h264",
    };
    for (const char* name : kCandidates) {
        if (GstElementFactory* f = gst_element_factory_find(name)) {
            gst_object_unref(f);
            return QString::fromLatin1(name);
        }
    }
    return {};
}

}  // namespace

struct VideoPipeline::Impl {
    QPointer<QVideoSink>     sink;
    QPointer<VideoPipeline>  owner;
    QString                  decoder;

    GstElement* pipeline   = nullptr;
    GstElement* webrtcbin  = nullptr;
    GstElement* appsink    = nullptr;  // created on pad-added

    std::atomic<bool>        firstFrameEmitted{false};
    std::atomic<bool>        answerEmitted{false};

    ~Impl() {
        if (pipeline) {
            gst_element_set_state(pipeline, GST_STATE_NULL);
            gst_object_unref(pipeline);
            pipeline = nullptr;
        }
    }

    // ---------- pad-added: build the decode chain lazily -------------------
    //
    // webrtcbin produces a src pad per incoming media track once DTLS/SRTP is
    // established. We only expect one (video/H264, PiKVM sends nothing else).
    static void onPadAdded(GstElement* /*webrtc*/, GstPad* pad, gpointer user) {
        auto* self = static_cast<Impl*>(user);

        // Log every pad-added — direction included — so a "no video"
        // failure mode tells us whether webrtcbin is firing this signal
        // at all vs. whether the src pad never materialises.
        {
            const char* dir = (GST_PAD_DIRECTION(pad) == GST_PAD_SRC) ? "SRC"
                            : (GST_PAD_DIRECTION(pad) == GST_PAD_SINK) ? "SINK"
                            : "UNKNOWN";
            const gchar* name = GST_PAD_NAME(pad) ? GST_PAD_NAME(pad) : "?";
            qCInfo(lcVideo) << "webrtcbin pad-added direction=" << dir
                            << "name=" << name;
        }

        if (GST_PAD_DIRECTION(pad) != GST_PAD_SRC) return;

        GstCaps* caps = gst_pad_get_current_caps(pad);
        if (!caps) caps = gst_pad_query_caps(pad, nullptr);
        if (!caps) {
            qCWarning(lcVideo) << "pad-added with no caps";
            return;
        }
        {
            gchar* capsStr = gst_caps_to_string(caps);
            qCInfo(lcVideo) << "webrtcbin pad-added caps:"
                            << QString::fromUtf8(capsStr ? capsStr : "");
            g_free(capsStr);
        }
        const GstStructure* s = gst_caps_get_structure(caps, 0);
        const gchar* enc = gst_structure_get_string(s, "encoding-name");
        if (g_strcmp0(enc, "H264") != 0) {
            qCWarning(lcVideo) << "unexpected encoding on webrtcbin pad:"
                               << QString::fromUtf8(enc ? enc : "?");
            gst_caps_unref(caps);
            return;
        }
        gst_caps_unref(caps);

        GstElement* queue = gst_element_factory_make("queue",         nullptr);
        GstElement* depay = gst_element_factory_make("rtph264depay",  nullptr);
        GstElement* parse = gst_element_factory_make("h264parse",     nullptr);
        GstElement* dec   = gst_element_factory_make(
            self->decoder.toUtf8().constData(), nullptr);
        GstElement* conv  = gst_element_factory_make("videoconvert",  nullptr);
        GstElement* capsf = gst_element_factory_make("capsfilter",    nullptr);
        GstElement* sink  = gst_element_factory_make("appsink",       nullptr);
        if (!queue || !depay || !parse || !dec || !conv || !capsf || !sink) {
            qCWarning(lcVideo) << "decode chain element creation failed"
                               << "(decoder=" << self->decoder << ")";
            return;
        }

        // h264parse config-interval=-1: re-inject SPS/PPS before every IDR.
        // Protects decoders that don't retain them from the stream header.
        g_object_set(parse, "config-interval", -1, nullptr);

        GstCaps* rawCaps = gst_caps_new_simple(
            "video/x-raw", "format", G_TYPE_STRING, "BGRA", nullptr);
        g_object_set(capsf, "caps", rawCaps, nullptr);
        gst_caps_unref(rawCaps);

        g_object_set(sink,
            "emit-signals", TRUE,
            "sync",         FALSE,
            "max-buffers",  2,
            "drop",         TRUE,
            nullptr);
        GstAppSinkCallbacks cb{};
        cb.new_sample = &onNewSample;
        gst_app_sink_set_callbacks(GST_APP_SINK(sink), &cb, self, nullptr);
        self->appsink = sink;

        gst_bin_add_many(GST_BIN(self->pipeline),
                         queue, depay, parse, dec, conv, capsf, sink, nullptr);
        if (!gst_element_link_many(queue, depay, parse, dec, conv,
                                   capsf, sink, nullptr)) {
            qCWarning(lcVideo) << "failed to link decode chain";
            return;
        }

        GstPad* queueSink = gst_element_get_static_pad(queue, "sink");
        const auto linkRet = gst_pad_link(pad, queueSink);
        gst_object_unref(queueSink);
        if (linkRet != GST_PAD_LINK_OK) {
            qCWarning(lcVideo) << "pad link failed, ret=" << linkRet;
            return;
        }

        // New elements start in NULL; push them up to the parent's state.
        gst_element_sync_state_with_parent(queue);
        gst_element_sync_state_with_parent(depay);
        gst_element_sync_state_with_parent(parse);
        gst_element_sync_state_with_parent(dec);
        gst_element_sync_state_with_parent(conv);
        gst_element_sync_state_with_parent(capsf);
        gst_element_sync_state_with_parent(sink);
    }

    // ---------- appsink: decoded frame → QVideoSink -----------------------
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

        // Marshal onto the thread that owns the QVideoSink (GUI thread).
        QPointer<QVideoSink>     sinkPtr  = self->sink;
        QPointer<VideoPipeline>  ownerPtr = self->owner;
        const bool wasFirst = !self->firstFrameEmitted.exchange(true);

        QMetaObject::invokeMethod(sinkPtr,
            [sinkPtr, ownerPtr, frame, wasFirst]() mutable {
                if (!sinkPtr) return;
                sinkPtr->setVideoFrame(frame);
                if (wasFirst && ownerPtr) emit ownerPtr->firstFrameRendered();
            },
            Qt::QueuedConnection);

        return GST_FLOW_OK;
    }

    // ---------- bus: surface errors as Qt signals ------------------------
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
                QPointer<VideoPipeline> ownerPtr = self->owner;
                QMetaObject::invokeMethod(ownerPtr, [ownerPtr, text]() {
                    if (ownerPtr) emit ownerPtr->errorOccurred(text);
                }, Qt::QueuedConnection);
                if (err) g_error_free(err);
                if (dbg) g_free(dbg);
                break;
            }
            case GST_MESSAGE_EOS:
                qCInfo(lcVideo) << "GStreamer EOS";
                break;
            default:
                break;
        }
        return TRUE;
    }

    // ---------- WebRTC signalling callbacks ------------------------------

    static void onRemoteDescSet(GstPromise* promise, gpointer user) {
        gst_promise_unref(promise);
        auto* self = static_cast<Impl*>(user);
        if (!self->webrtcbin) return;

        GstPromise* p = gst_promise_new_with_change_func(
            &onAnswerCreated, self, nullptr);
        g_signal_emit_by_name(self->webrtcbin, "create-answer", nullptr, p);
    }

    static void onAnswerCreated(GstPromise* promise, gpointer user) {
        auto* self = static_cast<Impl*>(user);

        const GstStructure* reply = gst_promise_get_reply(promise);
        GstWebRTCSessionDescription* answer = nullptr;
        if (reply) {
            gst_structure_get(reply, "answer",
                GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &answer, nullptr);
        }
        gst_promise_unref(promise);

        if (!answer) {
            qCWarning(lcVideo) << "create-answer produced no answer";
            return;
        }

        // set-local-description is fire-and-forget; the final SDP including
        // gathered ICE candidates comes out via notify::ice-gathering-state.
        GstPromise* setLocal = gst_promise_new();
        g_signal_emit_by_name(self->webrtcbin, "set-local-description",
                              answer, setLocal);
        gst_promise_interrupt(setLocal);
        gst_promise_unref(setLocal);

        gst_webrtc_session_description_free(answer);
    }

    // Diagnostic: surface ICE *connection* state (host-pair connectivity)
    // and overall peer-connection state separately from gathering state.
    // "webrtcup" from Janus is necessary but not always sufficient — if
    // we never see a connected ICE state on our side, the RTP packets
    // never get demultiplexed and pad-added doesn't fire.
    static void onIceConnectionStateChanged(
            GObject* obj, GParamSpec* /*pspec*/, gpointer /*user*/) {
        GstWebRTCICEConnectionState state =
            GST_WEBRTC_ICE_CONNECTION_STATE_NEW;
        g_object_get(obj, "ice-connection-state", &state, nullptr);
        qCInfo(lcVideo) << "ICE connection state:" << state;
    }

    static void onPeerConnectionStateChanged(
            GObject* obj, GParamSpec* /*pspec*/, gpointer /*user*/) {
        GstWebRTCPeerConnectionState state =
            GST_WEBRTC_PEER_CONNECTION_STATE_NEW;
        g_object_get(obj, "connection-state", &state, nullptr);
        qCInfo(lcVideo) << "peer connection state:" << state;
    }

    // Vanilla ICE: wait for gathering to finish, then grab the local
    // description (which now contains all `a=candidate` lines) and forward
    // it to the signalling side as the answer.
    static void onIceGatheringStateChanged(
            GObject* obj, GParamSpec* /*pspec*/, gpointer user) {
        GstWebRTCICEGatheringState state = GST_WEBRTC_ICE_GATHERING_STATE_NEW;
        g_object_get(obj, "ice-gathering-state", &state, nullptr);
        qCInfo(lcVideo) << "ICE gathering state:" << state;
        if (state != GST_WEBRTC_ICE_GATHERING_STATE_COMPLETE) return;

        auto* self = static_cast<Impl*>(user);
        if (self->answerEmitted.exchange(true)) return;  // once is enough

        GstWebRTCSessionDescription* localDesc = nullptr;
        g_object_get(obj, "local-description", &localDesc, nullptr);
        if (!localDesc) {
            qCWarning(lcVideo) << "local-description missing at gather-complete";
            return;
        }
        gchar* sdpText = gst_sdp_message_as_text(localDesc->sdp);
        QString sdpStr = QString::fromUtf8(sdpText ? sdpText : "");
        g_free(sdpText);
        gst_webrtc_session_description_free(localDesc);

        // Augment the answer to look more like a browser's. Janus's
        // ustreamer plugin sends H.264 with packetization-mode=1
        // (fragmented NALs across RTP packets) and rtcp-fb negotiating
        // nack + goog-remb; webrtcbin's auto-answer omits all of this
        // (just `nack pli` and `profile-level-id`), and empirically the
        // plugin stays silent on media when those bits are missing —
        // even though `webrtcup` fires. Browser SDP answers include
        // them, browsers see video, we don't.
        //
        // Surgical text edits rather than rebuilding the SDP because
        // we want to keep webrtcbin's ICE candidates and DTLS
        // fingerprint intact.
        const QString origAnswer = sdpStr;
        // 1. Add packetization-mode=1 to the fmtp line if absent.
        sdpStr.replace(
            QRegularExpression(QStringLiteral(
                "a=fmtp:(\\d+) (profile-level-id=[0-9A-Fa-f]+)(?!.*packetization-mode)")),
            QStringLiteral("a=fmtp:\\1 \\2;packetization-mode=1"));
        // 2. After the existing rtcp-fb line, mirror the offer's full set
        //    (nack, goog-remb) so the plugin trusts we'll honour them.
        sdpStr.replace(
            QRegularExpression(QStringLiteral("(a=rtcp-fb:(\\d+) nack pli\\n?)")),
            QStringLiteral("\\1a=rtcp-fb:\\2 nack\na=rtcp-fb:\\2 goog-remb\n"));

        if (sdpStr != origAnswer) {
            qCInfo(lcVideo) << "augmented answer SDP for Janus compatibility";
        }

        qCInfo(lcVideo) << "ICE gathering complete, answer ready,"
                        << sdpStr.size() << "bytes";
        // Full SDP at debug level so it doesn't dominate logs by default;
        // enable with QT_LOGGING_RULES='glasshouse.video.debug=true'.
        qCDebug(lcVideo).noquote() << "answer SDP:\n" << sdpStr;

        QPointer<VideoPipeline> ownerPtr = self->owner;
        QMetaObject::invokeMethod(ownerPtr, [ownerPtr, sdpStr]() {
            if (ownerPtr) emit ownerPtr->answerReady(sdpStr);
        }, Qt::QueuedConnection);
    }
};

VideoPipeline::VideoPipeline(QVideoSink* sink, QObject* parent)
    : QObject(parent), m_sink(sink) {
    if (!gst_is_initialized()) {
        gst_init(nullptr, nullptr);
    }
}

VideoPipeline::~VideoPipeline() { stop(); }

QString VideoPipeline::start() {
    if (m_impl) return {};  // already started

    m_activeDecoder = pickDecoder();
    if (m_activeDecoder.isEmpty()) {
        return QStringLiteral(
            "no H.264 decoder factory found — install gstreamer1.0-libav "
            "for avdec_h264, gstreamer1.0-plugins-bad for nvh264dec/vah264dec");
    }
    qCInfo(lcVideo) << "decoder:" << m_activeDecoder;

    GstElement* pipeline  = gst_pipeline_new("glasshouse-webrtc");
    GstElement* webrtcbin = gst_element_factory_make("webrtcbin", "webrtc");
    if (!pipeline || !webrtcbin) {
        if (pipeline)  gst_object_unref(pipeline);
        if (webrtcbin) gst_object_unref(webrtcbin);
        return QStringLiteral(
            "gst element factory failed for pipeline/webrtcbin "
            "(check gstreamer1.0-plugins-bad is installed)");
    }

    // Janus bundles everything onto a single transport; match that.
    g_object_set(webrtcbin,
        "bundle-policy", GST_WEBRTC_BUNDLE_POLICY_MAX_BUNDLE, nullptr);

    gst_bin_add(GST_BIN(pipeline), webrtcbin);

    // Restrict ICE candidate gathering to IPv4 LAN-shaped local
    // addresses. Empirically (tcpdump on enp5s0f0 captured by hand
    // 2026-04-27): on a stock PiKVM 4 Plus, ICE selecting an IPv6
    // link-local pair (fe80::) succeeds for STUN binding + DTLS
    // handshake, Janus emits `webrtcup`, but the ustreamer plugin
    // never delivers SRTP to the IPv6 endpoint — webrtcbin sees zero
    // inbound RTP, no `pad-added`, blank video. Forcing libnice to
    // only gather IPv4 addresses sidesteps the issue: ICE picks the
    // IPv4 host pair, media flows.
    //
    // `add-local-ip-address` since GStreamer 1.20: any explicit add
    // restricts gathering to the added set (no implicit "use everything").
    {
        GstObject* iceAgent = nullptr;
        g_object_get(webrtcbin, "ice-agent", &iceAgent, nullptr);
        if (iceAgent) {
            int addedCount = 0;
            for (const auto& iface : QNetworkInterface::allInterfaces()) {
                const auto flags = iface.flags();
                if (!(flags & QNetworkInterface::IsUp))         continue;
                if (flags & QNetworkInterface::IsLoopBack)      continue;
                // Skip Docker / VirtualBox / KVM bridges — they can't
                // reach a PiKVM on the physical LAN; their candidates
                // just slow ICE down.
                const QString name = iface.name();
                if (name.startsWith(QLatin1String("docker"))
                 || name.startsWith(QLatin1String("br-"))
                 || name.startsWith(QLatin1String("vmnet"))
                 || name.startsWith(QLatin1String("virbr"))
                 || name.startsWith(QLatin1String("veth"))) {
                    continue;
                }
                for (const auto& entry : iface.addressEntries()) {
                    const QHostAddress addr = entry.ip();
                    if (addr.protocol() != QAbstractSocket::IPv4Protocol) continue;
                    const QString s = addr.toString();
                    if (s.startsWith(QLatin1String("169.254."))) continue; // IPv4 LL
                    const QByteArray ba = s.toUtf8();
                    gboolean ret = FALSE;
                    g_signal_emit_by_name(iceAgent, "add-local-ip-address",
                                          ba.constData(), &ret);
                    qCInfo(lcVideo) << "ICE: add local ip"
                                    << s << "on" << name
                                    << "ok=" << (ret ? "1" : "0");
                    if (ret) ++addedCount;
                }
            }
            if (addedCount == 0) {
                qCWarning(lcVideo) << "ICE: no LAN IPv4 addresses added; "
                                      "libnice will fall back to gathering "
                                      "everything (including IPv6 link-local)";
            }
            gst_object_unref(iceAgent);
        }
    }

    auto impl = std::make_unique<Impl>();
    impl->pipeline  = pipeline;
    impl->webrtcbin = webrtcbin;
    impl->sink      = m_sink;
    impl->owner     = this;
    impl->decoder   = m_activeDecoder;

    g_signal_connect(webrtcbin, "pad-added",
                     G_CALLBACK(&Impl::onPadAdded), impl.get());
    g_signal_connect(webrtcbin, "notify::ice-gathering-state",
                     G_CALLBACK(&Impl::onIceGatheringStateChanged), impl.get());
    g_signal_connect(webrtcbin, "notify::ice-connection-state",
                     G_CALLBACK(&Impl::onIceConnectionStateChanged), impl.get());
    g_signal_connect(webrtcbin, "notify::connection-state",
                     G_CALLBACK(&Impl::onPeerConnectionStateChanged), impl.get());

    GstBus* bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline));
    gst_bus_add_watch(bus, &Impl::onBusMessage, impl.get());
    gst_object_unref(bus);

    const auto stateRet = gst_element_set_state(pipeline, GST_STATE_PLAYING);
    if (stateRet == GST_STATE_CHANGE_FAILURE) {
        // The bus watch runs async via the GLib main loop; at this point
        // the error posted during state-change may not have been dispatched
        // yet. Drain it synchronously so the caller gets the real reason.
        GstBus* bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline));
        GstMessage* msg = gst_bus_timed_pop_filtered(
            bus, 250 * GST_MSECOND,
            static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_WARNING));
        QString detail;
        if (msg) {
            GError* gerr = nullptr;
            gchar*  dbg  = nullptr;
            gst_message_parse_error(msg, &gerr, &dbg);
            detail = QStringLiteral(": %1 [%2]")
                .arg(QString::fromUtf8(gerr ? gerr->message : "(no GError)"),
                     QString::fromUtf8(dbg ? dbg : ""));
            if (gerr) g_error_free(gerr);
            if (dbg)  g_free(dbg);
            gst_message_unref(msg);
        }
        gst_object_unref(bus);
        return QStringLiteral("failed to set pipeline to PLAYING%1").arg(detail);
    }
    qCInfo(lcVideo) << "pipeline state change returned" << stateRet;

    m_impl = std::move(impl);
    return {};
}

void VideoPipeline::stop() {
    m_impl.reset();
}

void VideoPipeline::handleRemoteOffer(const QString& offerSdp) {
    if (!m_impl || !m_impl->webrtcbin) {
        qCWarning(lcVideo) << "handleRemoteOffer before start()";
        return;
    }

    GstSDPMessage* sdp = nullptr;
    gst_sdp_message_new(&sdp);
    const QByteArray bytes = offerSdp.toUtf8();
    if (gst_sdp_message_parse_buffer(
            reinterpret_cast<const guint8*>(bytes.constData()),
            bytes.size(), sdp) != GST_SDP_OK) {
        qCWarning(lcVideo) << "failed to parse SDP offer";
        gst_sdp_message_free(sdp);
        return;
    }

    // offer takes ownership of `sdp`; we must not free sdp ourselves.
    GstWebRTCSessionDescription* offer = gst_webrtc_session_description_new(
        GST_WEBRTC_SDP_TYPE_OFFER, sdp);
    if (!offer) {
        gst_sdp_message_free(sdp);
        qCWarning(lcVideo) << "failed to build WebRTC session description";
        return;
    }

    GstPromise* p = gst_promise_new_with_change_func(
        &Impl::onRemoteDescSet, m_impl.get(), nullptr);
    g_signal_emit_by_name(m_impl->webrtcbin, "set-remote-description", offer, p);

    gst_webrtc_session_description_free(offer);
}

}  // namespace glasshouse
