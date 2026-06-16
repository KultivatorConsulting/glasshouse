#pragma once

#include <QObject>
#include <QString>
#include <memory>

class QVideoSink;

namespace glasshouse {

// MJPEG-over-HTTP pipeline for older PiKVM firmware (e.g. PiKVM 3) that
// doesn't expose Janus — and a leak-free alternative to the Janus/webrtcbin
// path for any PiKVM. Pulls `GET /streamer/stream` as a long-lived
// `multipart/x-mixed-replace` response and decodes one JPEG per part:
//
//     souphttpsrc location=https://<host>/streamer/stream
//                 is-live=true ssl-strict=<not insecure_tls>
//                 cookies=["auth_token=…"]
//        ! multipartdemux
//        ! image/jpeg
//        ! queue leaky=downstream max-size-buffers=1     (stale-frame dropper)
//        ! jpegdec
//        ! videoconvert
//        ! video/x-raw,format=BGRA
//        ! appsink            (→ QVideoFrame → QVideoSink)
//
// Latency + CPU control, both *before* jpegdec: a leaky-downstream `queue`
// is the deterministic stale-frame dropper — MJPEG is all-intra so dropping
// a whole JPEG is safe, and keeping only the freshest buffers stops a slow
// decode/render from backing frames up in souphttpsrc + the kernel socket
// (the real source of >1 s MJPEG lag; ustreamer only ever holds the latest
// frame, so any backlog is client-side). Keeping only the single freshest
// buffer (max-size-buffers=1) keeps that backlog to a minimum. The former
// `jpegparse ! videorate max-rate=N` cap was removed — on ustreamer's
// untimestamped output videorate never capped (GStreamer #720104), only
// added latency, and froze the stream with drop-only=true. `maxFps` is now
// informational only.
//
// Why software jpegdec and not a HW decoder: ustreamer emits 4:2:2
// (`2x1,1x1,1x1`) baseline JPEG, which NVIDIA's `nvjpegdec` rejects with
// not-negotiated (and `vajpegdec` isn't present on NVIDIA at all),
// verified against a live PiKVM 2026-06-12. jpegdec is the only decoder
// that handles the real stream here; for genuinely low decode CPU the
// answer is the H.264/Janus transport with nvh264dec, not HW JPEG. See
// DESIGN.md §10.6.
//
// No signalling — this is just an HTTP GET that produces frames. The
// caller wires `firstFrameRendered` / `errorOccurred` for status, same
// shape as `VideoPipeline`.
class MjpegPipeline : public QObject {
    Q_OBJECT
public:
    MjpegPipeline(QVideoSink* sink, QObject* parent = nullptr);
    ~MjpegPipeline() override;

    // Build the pipeline and bring it to PLAYING. `authCookieHeader` is
    // the `auth_token=…` value from PiKvmClient (same cookie kvmd issues
    // for `/api/ws`). `maxFps` caps the client-side frame rate via a
    // pre-decode videorate (<=0 = uncapped). Returns empty string on
    // success.
    QString start(const QString& host,
                  const QString& authCookieHeader,
                  bool insecureTls,
                  int  maxFps = 0);

    void stop();

    // Decoder + active cap, for the status bar (e.g. "jpegdec" or
    // "jpegdec ≤30fps"). Reports "jpegdec" until start().
    QString activeDecoder() const { return m_activeDecoder; }

    // Steady-state telemetry (DESIGN §10.2): frames painted to the sink, and
    // frames coalesced away because the GUI thread was behind. Poll on a
    // timer to derive effective render FPS. Both zero before start().
    quint64 framesDelivered() const;
    quint64 framesCoalesced() const;

signals:
    void firstFrameRendered();
    void errorOccurred(const QString& msg);

private:
    struct Impl;
    std::unique_ptr<Impl>  m_impl;
    QVideoSink*            m_sink;  // not owned
    QString                m_activeDecoder = QStringLiteral("jpegdec");
};

}  // namespace glasshouse
