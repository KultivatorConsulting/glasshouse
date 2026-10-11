#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>

#include <memory>

class QNetworkAccessManager;
class QNetworkReply;

namespace glasshouse {

// Minimal client for Tui's chat endpoint (Kultivator's model backend).
// Docs: https://tui.kultivator.net.nz/llms.txt, OpenAPI at /openapi.yaml.
//
// OpenAI-compatible `POST /v1/chat/completions`, authenticated with an
// `X-API-Key` header. We use exactly one shape — a single user message
// carrying a prompt plus one inline PNG — so this is deliberately not a
// general chat client.
//
// Takes PNG bytes rather than a QImage so `glasshouse_core` needn't link
// Qt Gui; the caller encodes.
class TuiClient : public QObject {
    Q_OBJECT
public:
    struct Options {
        QString endpoint;
        QString apiKey;
        QString model            = QStringLiteral("auto");
        QString reasoningEffort  = QStringLiteral("none");
        // Must be generous: reasoning tokens count against this budget and
        // the server answers 200 with EMPTY content when it runs out, with
        // nothing to say why. 8000 is the documented floor.
        int     maxTokens        = 8000;
        int     timeoutMs        = 60000;
    };

    explicit TuiClient(Options opts, QObject* parent = nullptr);
    ~TuiClient() override;

    bool configured() const { return !m_opts.apiKey.isEmpty()
                                  && !m_opts.endpoint.isEmpty(); }

    // Send `pngBytes` with `prompt` and report the model's reply text.
    // `finished` fires exactly once per call, including on error. Images
    // over ~2.1 MP are downscaled server-side; a 1920x1080 frame is 2.07 MP
    // and so arrives intact.
    void extractText(const QByteArray& pngBytes, const QString& prompt);

    // True from extractText() until `finished`. Extraction can take a
    // while, so the UI needs to know when to show progress — and when a
    // second request should mean "stop waiting" instead of "start again".
    bool busy() const { return m_reply != nullptr; }

    // Abandon the request in flight; `finished` still fires, with ok=false
    // and a "cancelled" reason. No-op when idle.
    void cancel();

signals:
    // `ok` false means `error` is set and `text` is empty.
    void finished(bool ok, const QString& text, const QString& error);

private:
    Options                                m_opts;
    std::unique_ptr<QNetworkAccessManager> m_nam;
    QNetworkReply*                         m_reply = nullptr;
};

}  // namespace glasshouse
