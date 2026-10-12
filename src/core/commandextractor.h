#pragma once

#include "textextractor.h"

#include <QStringList>

class QProcess;

namespace glasshouse {

// Extraction by running a host program: PNG on stdin, text on stdout.
//
// This is what "the host's OCR" means on Linux. There is no system OCR
// service to call the way macOS and Windows have one, so the general
// form of local OCR is a subprocess — which also means it covers any
// tool the user already trusts, not just the one we thought of.
//
// Tesseract is the obvious fit and reads stdin to stdout natively:
//
//     command: ["tesseract", "-", "-"]
//     command: ["tesseract", "-", "-", "-l", "eng", "--psm", "6"]
//
// Nothing is linked against it, so an install that uses a different
// provider carries no dependency on it at all.
class CommandExtractor : public TextExtractor {
    Q_OBJECT
public:
    // `argv` is the program and its arguments; `timeoutMs` kills a run
    // that hangs, so a mistyped command cannot wedge the hotkey forever.
    CommandExtractor(QStringList argv, int timeoutMs,
                     QObject* parent = nullptr);
    ~CommandExtractor() override;

    bool    configured(QString* why = nullptr) const override;
    QString describe() const override;
    void    extract(const ExtractRequest& req) override;
    bool    busy() const override { return m_proc != nullptr; }
    void    cancel() override;

private:
    void cleanUp();

    QStringList m_argv;
    int         m_timeoutMs;
    QProcess*   m_proc = nullptr;
    bool        m_cancelled = false;
};

}  // namespace glasshouse
