#include "pikvmextractor.h"

#include "logging.h"
#include "pikvmclient.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>

namespace glasshouse {

namespace {

// kvmd reports its own errors as JSON even on the OCR endpoint, which
// otherwise answers text/plain. Pull the useful half out when it does.
QString kvmdError(const QByteArray& raw, int status) {
    const QJsonDocument doc = QJsonDocument::fromJson(raw);
    if (doc.isObject()) {
        const QJsonObject r =
            doc.object().value(QStringLiteral("result")).toObject();
        const QString msg = r.value(QStringLiteral("error_msg")).toString();
        const QString err = r.value(QStringLiteral("error")).toString();
        if (!msg.isEmpty()) {
            return QStringLiteral("%1 (%2)").arg(msg, err.isEmpty()
                                                          ? QString::number(status)
                                                          : err);
        }
    }
    return {};
}

}  // namespace

PikvmExtractor::PikvmExtractor(QHash<QString, PiKvmClient*> clients,
                               QStringList langs, int timeoutMs,
                               QObject* parent)
    : TextExtractor(parent), m_langs(std::move(langs)), m_timeoutMs(timeoutMs) {
    for (auto it = clients.cbegin(); it != clients.cend(); ++it) {
        m_clients.insert(it.key(), it.value());
    }
}

bool PikvmExtractor::configured(QString* why) const {
    if (m_clients.isEmpty()) {
        if (why) {
            *why = QStringLiteral("no PiKVM clients are running");
        }
        return false;
    }
    if (m_langs.isEmpty()) {
        if (why) {
            *why = QStringLiteral("ocr.langs is empty");
        }
        return false;
    }
    return true;
}

QString PikvmExtractor::describe() const {
    return QStringLiteral("PiKVM OCR (%1)").arg(m_langs.join(QLatin1Char(',')));
}

void PikvmExtractor::extract(const ExtractRequest& req) {
    if (busy()) {
        emit finished(false, QString(),
                      QStringLiteral("an extraction is already running"));
        return;
    }
    if (QString why; !configured(&why)) {
        emit finished(false, QString(), why);
        return;
    }

    PiKvmClient* client = m_clients.value(req.host).data();
    if (!client) {
        emit finished(false, QString(),
                      QStringLiteral("no client for %1").arg(req.host));
        return;
    }
    if (!req.frameRect.isValid() || req.frameRect.isEmpty()) {
        emit finished(false, QString(), QStringLiteral("empty selection"));
        return;
    }

    qCInfo(lcOcr) << "pikvm:" << req.host << "OCR of" << req.frameRect
                  << "langs" << m_langs;

    QNetworkReply* reply = client->ocrRegion(req.frameRect, m_langs,
                                             m_timeoutMs);
    if (!reply) {
        emit finished(false, QString(),
                      QStringLiteral("%1 is not authenticated yet")
                          .arg(req.host));
        return;
    }
    m_reply = reply;

    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        if (m_reply == reply) m_reply = nullptr;

        if (reply->error() == QNetworkReply::OperationCanceledError) {
            qCInfo(lcOcr) << "pikvm: OCR cancelled or timed out";
            emit finished(false, QString(), QStringLiteral("cancelled"));
            return;
        }

        const int status = reply->attribute(
                                   QNetworkRequest::HttpStatusCodeAttribute)
                               .toInt();
        const QByteArray raw = reply->readAll();

        if (reply->error() != QNetworkReply::NoError) {
            QString msg = kvmdError(raw, status);
            if (msg.isEmpty()) {
                switch (status) {
                    case 400:
                        msg = QStringLiteral("the PiKVM rejected the request "
                                             "(400) — check ocr.langs against "
                                             "what /api/streamer/ocr reports");
                        break;
                    case 404:
                        msg = QStringLiteral("this PiKVM has no OCR endpoint "
                                             "(404) — its firmware predates "
                                             "the feature");
                        break;
                    case 503:
                        msg = QStringLiteral("OCR is disabled on this PiKVM "
                                             "(503)");
                        break;
                    case 504:
                        // The measured failure: a big region outlives the
                        // nginx timeout sitting in front of kvmd.
                        msg = QStringLiteral("the PiKVM took too long (504) — "
                                             "select a smaller region, or use "
                                             "a faster provider");
                        break;
                    default:
                        msg = reply->errorString();
                }
            }
            qCWarning(lcOcr) << "pikvm: OCR failed:" << msg
                             << "body:" << raw.left(300);
            emit finished(false, QString(), msg);
            return;
        }

        // Trailing newlines are Tesseract's, not the user's.
        QString text = QString::fromUtf8(raw);
        while (text.endsWith(QLatin1Char('\n')) ||
               text.endsWith(QLatin1Char('\r'))) {
            text.chop(1);
        }
        if (text.isEmpty()) {
            emit finished(false, QString(),
                          QStringLiteral("no text found in the selection"));
            return;
        }
        qCInfo(lcOcr) << "pikvm: extracted" << text.size() << "chars";
        emit finished(true, text, QString());
    });
}

void PikvmExtractor::cancel() {
    if (QNetworkReply* reply = m_reply) {
        m_reply = nullptr;
        reply->abort();   // `finished` fires with OperationCanceledError
    }
}

}  // namespace glasshouse
