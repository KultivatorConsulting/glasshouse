#pragma once

#include "textextractor.h"

#include <memory>

class QNetworkAccessManager;
class QNetworkReply;

namespace glasshouse {

// Extraction via an OpenAI-compatible `POST /v1/chat/completions`.
//
// Covers both the `openai` and `tui` providers, which differ only in
// endpoint and in how the key is presented: OpenAI wants
// `Authorization: Bearer`, Tui wants `X-API-Key`. Everything else — a
// single user message carrying a prompt plus one inline PNG — is the
// same wire shape, and the same one Ollama, vLLM and LM Studio accept,
// so pointing `endpoint` at a local model works without new code.
//
// Deliberately not a general chat client: it sends exactly that one
// shape and reads `choices[0].message.content` back.
//
// Takes PNG bytes rather than a QImage so `glasshouse_core` needn't link
// Qt Gui; the caller encodes.
class OpenAiExtractor : public TextExtractor {
    Q_OBJECT
public:
    enum class Auth {
        Bearer,   // Authorization: Bearer <key>  — OpenAI and most clones
        ApiKey,   // X-API-Key: <key>             — Tui
    };

    struct Options {
        QString endpoint;
        QString apiKey;
        Auth    auth             = Auth::Bearer;
        QString label            = QStringLiteral("OpenAI");  // for messages
        QString model            = QStringLiteral("auto");
        QString reasoningEffort  = QStringLiteral("none");
        // Must be generous: reasoning tokens count against this budget and
        // Tui answers 200 with EMPTY content when it runs out, with
        // nothing to say why. 8000 is the documented floor.
        int     maxTokens        = 8000;
        int     timeoutMs        = 60000;
    };

    explicit OpenAiExtractor(Options opts, QObject* parent = nullptr);
    ~OpenAiExtractor() override;

    bool    configured(QString* why = nullptr) const override;
    QString describe() const override;
    void    extract(const ExtractRequest& req) override;
    bool    busy() const override { return m_reply != nullptr; }
    void    cancel() override;

private:
    Options                                m_opts;
    std::unique_ptr<QNetworkAccessManager> m_nam;
    QNetworkReply*                         m_reply = nullptr;
};

}  // namespace glasshouse
