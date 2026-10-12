#pragma once

#include "textextractor.h"

#include <QHash>
#include <QPointer>
#include <QStringList>

class QNetworkReply;

namespace glasshouse {

class PiKvmClient;

// Extraction by the PiKVM's own OCR.
//
// The cheapest provider by a distance: stock firmware ships it enabled
// with `eng` and `osd`, so it needs nothing installed on the host and
// nothing installed on the PiKVM, and — unlike the model-backed
// providers — the image never leaves the LAN. That makes it the right
// default for a public clone and the right answer for anyone who cannot
// send screenshots of a target machine to a third party.
//
// It is Tesseract, so expect Tesseract's quality: good on prose, prone
// to confusing punctuation and single characters in hashes and serial
// numbers. When that matters, a model-backed provider reads screens
// better.
//
// Unlike the others, this one does not send the captured PNG at all.
// The PiKVM OCRs the frame it is serving, so the request is a rectangle
// — which means the result reflects the screen at request time, not at
// the moment the user froze the selection.
class PikvmExtractor : public TextExtractor {
    Q_OBJECT
public:
    // `clients` maps host -> client, because the selection can come from
    // any window and each head is served by its own PiKVM.
    PikvmExtractor(QHash<QString, PiKvmClient*> clients, QStringList langs,
                   int timeoutMs, QObject* parent = nullptr);

    bool    configured(QString* why = nullptr) const override;
    QString describe() const override;
    void    extract(const ExtractRequest& req) override;
    bool    busy() const override { return m_reply != nullptr; }
    void    cancel() override;

private:
    QHash<QString, QPointer<PiKvmClient>> m_clients;
    QStringList                           m_langs;
    int                                   m_timeoutMs;
    QNetworkReply*                        m_reply = nullptr;
};

}  // namespace glasshouse
