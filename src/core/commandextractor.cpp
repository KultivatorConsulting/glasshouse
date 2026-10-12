#include "commandextractor.h"

#include "logging.h"

#include <QProcess>
#include <QTimer>

namespace glasshouse {

CommandExtractor::CommandExtractor(QStringList argv, int timeoutMs,
                                   QObject* parent)
    : TextExtractor(parent), m_argv(std::move(argv)), m_timeoutMs(timeoutMs) {}

CommandExtractor::~CommandExtractor() {
    if (m_proc) {
        m_proc->kill();
        m_proc->waitForFinished(1000);
    }
}

bool CommandExtractor::configured(QString* why) const {
    if (m_argv.isEmpty()) {
        if (why) {
            *why = QStringLiteral("ocr.command is empty");
        }
        return false;
    }
    return true;
}

QString CommandExtractor::describe() const {
    return QStringLiteral("command (%1)").arg(m_argv.join(QLatin1Char(' ')));
}

void CommandExtractor::cleanUp() {
    if (!m_proc) return;
    m_proc->deleteLater();
    m_proc = nullptr;
}

void CommandExtractor::extract(const ExtractRequest& req) {
    if (busy()) {
        emit finished(false, QString(),
                      QStringLiteral("an extraction is already running"));
        return;
    }
    if (QString why; !configured(&why)) {
        emit finished(false, QString(), why);
        return;
    }
    if (req.png.isEmpty()) {
        emit finished(false, QString(), QStringLiteral("empty image"));
        return;
    }

    m_cancelled = false;
    auto* proc  = new QProcess(this);
    m_proc      = proc;

    const QString program = m_argv.first();
    const QStringList args = m_argv.mid(1);

    qCInfo(lcOcr) << "command:" << program << args << "with" << req.png.size()
                  << "byte PNG on stdin";

    // errorOccurred fires for FailedToStart without ever reaching
    // QProcess::finished, so both paths have to report.
    connect(proc, &QProcess::errorOccurred, this,
            [this, proc](QProcess::ProcessError e) {
        if (m_proc != proc) return;   // already reported and cleaned up
        if (m_cancelled) return;      // cancel() reports for itself
        const QString msg =
            e == QProcess::FailedToStart
                ? QStringLiteral("could not run '%1' — is it installed?")
                      .arg(m_argv.first())
                : QStringLiteral("extraction process error: %1")
                      .arg(proc->errorString());
        qCWarning(lcOcr) << "command:" << msg;
        cleanUp();
        emit finished(false, QString(), msg);
    });

    connect(proc, &QProcess::finished, this,
            [this, proc](int code, QProcess::ExitStatus status) {
        if (m_proc != proc) return;
        const QByteArray out = proc->readAllStandardOutput();
        const QByteArray err = proc->readAllStandardError();
        const bool cancelled = m_cancelled;
        cleanUp();

        if (cancelled) {
            emit finished(false, QString(), QStringLiteral("cancelled"));
            return;
        }
        if (status != QProcess::NormalExit) {
            emit finished(false, QString(),
                          QStringLiteral("extraction process crashed"));
            return;
        }
        if (code != 0) {
            // stderr is where Tesseract explains itself, so pass it on
            // rather than making the user go and run it by hand.
            const QString detail = QString::fromUtf8(err).trimmed();
            qCWarning(lcOcr) << "command: exit" << code << detail;
            emit finished(false, QString(),
                          detail.isEmpty()
                              ? QStringLiteral("extraction exited %1").arg(code)
                              : QStringLiteral("extraction exited %1: %2")
                                    .arg(code).arg(detail.left(200)));
            return;
        }

        // Tesseract pads with trailing blank lines; nobody wants those on
        // their clipboard, but interior blank lines are real structure.
        QString text = QString::fromUtf8(out);
        while (text.endsWith(QLatin1Char('\n')) ||
               text.endsWith(QLatin1Char('\r'))) {
            text.chop(1);
        }
        if (text.isEmpty()) {
            emit finished(false, QString(),
                          QStringLiteral("no text found in the selection"));
            return;
        }
        qCInfo(lcOcr) << "command: extracted" << text.size() << "chars";
        emit finished(true, text, QString());
    });

    proc->start(program, args);
    proc->write(req.png);
    proc->closeWriteChannel();

    if (m_timeoutMs > 0) {
        QTimer::singleShot(m_timeoutMs, proc, [this, proc]() {
            if (m_proc != proc || proc->state() == QProcess::NotRunning) return;
            qCWarning(lcOcr) << "command: timed out after" << m_timeoutMs << "ms";
            m_cancelled = true;   // routes the kill through the cancel path
            proc->kill();
        });
    }
}

void CommandExtractor::cancel() {
    if (!m_proc) return;
    m_cancelled = true;
    m_proc->kill();   // `finished` fires and reports "cancelled"
}

}  // namespace glasshouse
