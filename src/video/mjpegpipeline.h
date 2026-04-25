#pragma once

#include <QObject>
#include <QString>
#include <memory>

class QVideoSink;

namespace glasshouse {

// MJPEG-over-HTTP pipeline for older PiKVM firmware (e.g. PiKVM 3) that
// doesn't expose Janus. Pulls `GET /streamer/stream` as a long-lived
// `multipart/x-mixed-replace` response and decodes one JPEG per part:
//
//     souphttpsrc location=https://<host>/streamer/stream
//                 is-live=true ssl-strict=<not insecure_tls>
//                 cookies=["auth_token=…"]
//        ! multipartdemux
//        ! image/jpeg
//        ! jpegdec
//        ! videoconvert
//        ! video/x-raw,format=BGRA
//        ! appsink            (→ QVideoFrame → QVideoSink)
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
    // for `/api/ws`). Returns empty string on success.
    QString start(const QString& host,
                  const QString& authCookieHeader,
                  bool insecureTls);

    void stop();

    // Static identifier for the status bar.
    QString activeDecoder() const { return QStringLiteral("jpegdec"); }

signals:
    void firstFrameRendered();
    void errorOccurred(const QString& msg);

private:
    struct Impl;
    std::unique_ptr<Impl>  m_impl;
    QVideoSink*            m_sink;  // not owned
};

}  // namespace glasshouse
