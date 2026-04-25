#pragma once

#include <QObject>
#include <QString>
#include <memory>

class QVideoSink;

namespace glasshouse {

// GStreamer pipeline built around `webrtcbin`: terminates the WebRTC peer
// with PiKVM's Janus gateway and renders the decoded H.264 stream into a
// QVideoSink.
//
// Pipeline shape:
//     webrtcbin name=webrtc bundle-policy=max-bundle
//        — on pad-added (fires once DTLS/SRTP is up) —
//        queue ! rtph264depay ! h264parse config-interval=-1
//              ! <decoder>   (nvh264dec | vah264dec | avdec_h264)
//              ! videoconvert
//              ! video/x-raw,format=BGRA
//              ! appsink                (→ QVideoFrame → QVideoSink)
//
// Signalling handshake, driven from outside (typically a JanusClient):
//   1. caller sets up connections and calls start()
//   2. caller forwards the Janus SDP offer via handleRemoteOffer()
//   3. this class runs set-remote → create-answer → set-local, waits for
//      ICE gathering to complete (vanilla ICE — all candidates in the SDP),
//      then emits answerReady() with the final SDP the caller sends to
//      Janus as the `start` message
class VideoPipeline : public QObject {
    Q_OBJECT
public:
    VideoPipeline(QVideoSink* sink, QObject* parent = nullptr);
    ~VideoPipeline() override;

    // Build the pipeline, bring it to PLAYING. Empty return string on success;
    // otherwise a human-readable error.
    QString start();
    void    stop();

    QString activeDecoder() const { return m_activeDecoder; }

public slots:
    // Feed the SDP offer received from Janus. Internally runs
    // set-remote-description → create-answer → set-local-description.
    // The corresponding answer surfaces on `answerReady` once ICE gathering
    // completes.
    void handleRemoteOffer(const QString& offerSdp);

signals:
    // Fires once the local SDP is final (ICE gathering complete). Route this
    // back to JanusClient::sendAnswer.
    void answerReady(QString sdp);
    void firstFrameRendered();
    void errorOccurred(const QString& msg);

private:
    struct Impl;
    std::unique_ptr<Impl>  m_impl;
    QString                m_activeDecoder;
    QVideoSink*            m_sink;  // not owned
};

}  // namespace glasshouse
