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
//        ! jpegparse ! videorate max-rate=<maxFps>   (only when maxFps>0)
//        ! jpegdec
//        ! videoconvert
//        ! video/x-raw,format=BGRA
//        ! appsink            (→ QVideoFrame → QVideoSink)
//
// Frame-rate cap: ustreamer pushes JPEG at the HDMI capture rate (with
// drop-same-frames suppressing static frames). A `videorate max-rate=N`
// placed *before* jpegdec drops surplus frames while they are still
// encoded, so the cap cuts software decode **and** colour-convert **and**
// the main-thread render together — KVM/console work doesn't need 60fps.
// `max-rate` implies drop-only, so motion is paced and a static screen
// costs nothing. `maxFps <= 0` disables the cap (decode every frame).
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
