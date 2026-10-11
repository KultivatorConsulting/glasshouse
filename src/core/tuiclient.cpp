#include "tuiclient.h"

#include "logging.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>

namespace glasshouse {

TuiClient::TuiClient(Options opts, QObject* parent)
    : QObject(parent), m_opts(std::move(opts)),
      m_nam(std::make_unique<QNetworkAccessManager>()) {}

TuiClient::~TuiClient() = default;

void TuiClient::extractText(const QByteArray& pngBytes, const QString& prompt) {
    if (busy()) {
        emit finished(false, QString(),
                      QStringLiteral("an extraction is already running"));
        return;
    }
    if (!configured()) {
        emit finished(false, QString(),
                      QStringLiteral("no Tui endpoint/API key configured"));
        return;
    }
    if (pngBytes.isEmpty()) {
        emit finished(false, QString(), QStringLiteral("empty image"));
        return;
    }

    QJsonObject imageUrl{
        {QStringLiteral("url"),
         QStringLiteral("data:image/png;base64,")
             + QString::fromLatin1(pngBytes.toBase64())},
    };
    QJsonArray content{
        QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                    {QStringLiteral("text"), prompt}},
        QJsonObject{{QStringLiteral("type"), QStringLiteral("image_url")},
                    {QStringLiteral("image_url"), imageUrl}},
    };
    QJsonObject body{
        {QStringLiteral("model"),      m_opts.model},
        {QStringLiteral("max_tokens"), m_opts.maxTokens},
        {QStringLiteral("messages"),
         QJsonArray{QJsonObject{{QStringLiteral("role"), QStringLiteral("user")},
                                {QStringLiteral("content"), content}}}},
    };
    if (!m_opts.reasoningEffort.isEmpty()) {
        body[QStringLiteral("reasoning_effort")] = m_opts.reasoningEffort;
    }

    QNetworkRequest req(QUrl(m_opts.endpoint));
    req.setHeader(QNetworkRequest::ContentTypeHeader,
                  QStringLiteral("application/json"));
    req.setRawHeader("X-API-Key", m_opts.apiKey.toUtf8());
    req.setTransferTimeout(m_opts.timeoutMs);

    const QByteArray payload = QJsonDocument(body).toJson(QJsonDocument::Compact);
    qCInfo(lcOcr) << "tui: requesting text extraction," << pngBytes.size()
                  << "byte PNG," << payload.size() << "byte request";

    QNetworkReply* reply = m_nam->post(req, payload);
    m_reply = reply;
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        if (m_reply == reply) m_reply = nullptr;

        if (reply->error() == QNetworkReply::OperationCanceledError) {
            // Either the user cancelled or the transfer timeout fired;
            // both mean the same thing to the caller.
            qCInfo(lcOcr) << "tui: request cancelled or timed out";
            emit finished(false, QString(), QStringLiteral("cancelled"));
            return;
        }

        const int status = reply->attribute(
                                   QNetworkRequest::HttpStatusCodeAttribute)
                               .toInt();
        const QByteArray raw = reply->readAll();

        if (reply->error() != QNetworkReply::NoError) {
            // The documented statuses are worth naming — "Error transferring
            // …" tells the user nothing about which knob to reach for.
            QString msg;
            switch (status) {
                case 401: msg = QStringLiteral("Tui rejected the API key (401)"); break;
                case 429: msg = QStringLiteral("Tui quota or rate limit (429)"); break;
                case 422: msg = QStringLiteral("Tui rejected the request (422)"); break;
                case 400: msg = QStringLiteral("request too large for the context "
                                               "window (400)"); break;
                default:
                    msg = status >= 500
                              ? QStringLiteral("Tui upstream error (%1)").arg(status)
                              : reply->errorString();
            }
            qCWarning(lcOcr) << "tui: request failed:" << msg
                             << "body:" << raw.left(400);
            emit finished(false, QString(), msg);
            return;
        }

        QJsonParseError perr{};
        const QJsonDocument doc = QJsonDocument::fromJson(raw, &perr);
        if (perr.error != QJsonParseError::NoError || !doc.isObject()) {
            emit finished(false, QString(),
                          QStringLiteral("unparseable response: %1")
                              .arg(perr.errorString()));
            return;
        }

        const QJsonArray choices =
            doc.object().value(QStringLiteral("choices")).toArray();
        if (choices.isEmpty()) {
            emit finished(false, QString(),
                          QStringLiteral("response carried no choices"));
            return;
        }
        const QJsonObject msgObj = choices.at(0).toObject()
                                       .value(QStringLiteral("message"))
                                       .toObject();
        const QString text =
            msgObj.value(QStringLiteral("content")).toString();

        if (text.isEmpty()) {
            // The documented failure mode: reasoning ate the token budget
            // and the server still answers 200. Say so, rather than putting
            // an empty string on the user's clipboard.
            const QString reason = choices.at(0).toObject()
                                       .value(QStringLiteral("finish_reason"))
                                       .toString();
            qCWarning(lcOcr) << "tui: empty content, finish_reason =" << reason;
            emit finished(false, QString(),
                          QStringLiteral("model returned no text (finish_reason=%1)"
                                         " — try a larger max_tokens")
                              .arg(reason.isEmpty() ? QStringLiteral("unset")
                                                    : reason));
            return;
        }

        qCInfo(lcOcr) << "tui: extracted" << text.size() << "chars";
        emit finished(true, text, QString());
    });
}

void TuiClient::cancel() {
    if (QNetworkReply* reply = m_reply) {
        m_reply = nullptr;
        reply->abort();   // `finished` fires with OperationCanceledError
    }
}

}  // namespace glasshouse
