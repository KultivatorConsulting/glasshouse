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
//        queue (bounded, non-leaky) ! rtph264depay
//              ! h264parse config-interval=-1
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

    // webrtcbin jitterbuffer dwell time (ms). Set before start(); applied to
    // the bin as it goes to PLAYING. Lower = less glass-to-glass latency,
    // less jitter tolerance. Default 100 (LAN-tuned; webrtcbin's own is 200).
    void setJitterLatencyMs(int ms) { m_jitterLatencyMs = ms; }

    // Steady-state telemetry (DESIGN §10.2): frames painted to the sink, and
    // frames coalesced away because the GUI thread was behind. Poll on a
    // timer to derive effective render FPS. Both zero before start().
    quint64 framesDelivered() const;
    quint64 framesCoalesced() const;

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
    int                    m_jitterLatencyMs = 100;
};

}  // namespace glasshouse
