#pragma once

#include <QByteArray>
#include <QObject>
#include <QRect>
#include <QString>

namespace glasshouse {

// What the region hotkey hands an extractor.
//
// Carries both forms of the same selection because the providers want
// different things and only the caller has both. `png` is the crop the
// user dragged, already encoded — that is what a model or a host tool
// reads. `frameRect` and `host` say where it came from, which is what
// the PiKVM needs, because its OCR runs against its own live frame and
// takes a rectangle rather than an upload.
//
// One consequence worth knowing: the PiKVM provider OCRs the frame as it
// is when the request lands, while the others OCR the frame as it was
// when the user froze it. On a screen that is changing under the
// selection those can differ.
struct ExtractRequest {
    QByteArray png;
    QRect      frameRect;  // in the source frame's own pixels
    QString    host;       // the PiKVM that served the frame
    QString    prompt;     // only the model-backed providers use this
};

// One way of turning a selected region into text.
//
// The shape is the one TuiClient already had — start, ask if it is
// running, cancel, hear about it once — because that is what the UI
// needs regardless of what is doing the work: every provider here can
// take long enough to need a spinner, including the PiKVM (11 s for an
// 890x320 region on a PiKVM 3).
class TextExtractor : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    ~TextExtractor() override = default;

    // False means the hotkey should say so rather than appear broken.
    // `why` gets a message fit to show the user.
    virtual bool configured(QString* why = nullptr) const = 0;

    // Human-readable, for the startup log: "PiKVM OCR (eng)".
    virtual QString describe() const = 0;

    // `finished` fires exactly once per call, including on error.
    virtual void extract(const ExtractRequest& req) = 0;

    // True from extract() until `finished`. Extraction can take a while,
    // so the UI needs to know when to show progress — and when a second
    // request should mean "stop waiting" instead of "start again".
    virtual bool busy() const = 0;

    // Abandon the request in flight; `finished` still fires, with
    // ok=false and a "cancelled" reason. No-op when idle.
    virtual void cancel() = 0;

signals:
    // `ok` false means `error` is set and `text` is empty.
    void finished(bool ok, const QString& text, const QString& error);
};

}  // namespace glasshouse
