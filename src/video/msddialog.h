#pragma once

#include <QDialog>
#include <QPointer>

class QCheckBox;
class QFile;
class QJsonObject;
class QLabel;
class QListWidget;
class QProgressDialog;
class QPushButton;

namespace glasshouse {

class PiKvmClient;

// Modal dialog for the HID master's Mass Storage Device.
//
// Subscribes to PiKvmClient::rawStateEvent and watches for `msd_state`
// frames; refreshes the UI on every update so the dialog stays in sync
// with the server's view (other clients can also be modifying state via
// the kvmd web UI).
//
// Actions go straight to the master PiKvmClient; we don't bounce them
// through InputRouter because MSD is intrinsically per-device — the
// "route to whichever PiKVM" abstraction doesn't apply when only one
// PiKVM has the MSD hardware wired up.
class MsdDialog : public QDialog {
    Q_OBJECT
public:
    MsdDialog(PiKvmClient* master, QWidget* parent = nullptr);
    ~MsdDialog() override;

public slots:
    void toggle();

private slots:
    void onStateEvent(const QString& type, const QJsonObject& event);
    void onUploadProgress(qint64 sent, qint64 total);
    void onUploadFinished(bool ok, const QString& reason);

    void onUploadClicked();
    void onUseSelectedClicked();
    void onDeleteSelectedClicked();
    void onConnectClicked();
    void onDisconnectClicked();
    void onCdromToggled(bool cdrom);

private:
    QPointer<PiKvmClient> m_master;

    QLabel*       m_statusLabel  = nullptr;
    QLabel*       m_storageLabel = nullptr;
    QLabel*       m_activeLabel  = nullptr;
    QListWidget*  m_imageList    = nullptr;
    QCheckBox*    m_cdromCheck   = nullptr;
    QPushButton*  m_uploadBtn    = nullptr;
    QPushButton*  m_useBtn       = nullptr;
    QPushButton*  m_deleteBtn    = nullptr;
    QPushButton*  m_connectBtn   = nullptr;
    QPushButton*  m_disconnectBtn= nullptr;

    QProgressDialog* m_uploadProgress = nullptr;
    QFile*           m_uploadFile     = nullptr;

    // Cached drive state for action enablement / cdrom-toggle behaviour.
    QString m_currentImage;
    bool    m_currentlyConnected = false;
    bool    m_currentCdrom       = true;
};

}  // namespace glasshouse
