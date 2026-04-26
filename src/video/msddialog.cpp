#include "msddialog.h"

#include "pikvmclient.h"

#include <QCheckBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QProgressDialog>
#include <QPushButton>
#include <QVBoxLayout>

namespace glasshouse {

namespace {

QString formatBytes(qint64 b) {
    if (b < 0) return QStringLiteral("?");
    static const QStringList units{
        QStringLiteral("B"),  QStringLiteral("KB"), QStringLiteral("MB"),
        QStringLiteral("GB"), QStringLiteral("TB")};
    double d = static_cast<double>(b);
    int i = 0;
    while (d >= 1024.0 && i + 1 < units.size()) { d /= 1024.0; ++i; }
    return QStringLiteral("%1 %2")
        .arg(QString::number(d, 'f', i == 0 ? 0 : 2))
        .arg(units[i]);
}

}  // namespace

MsdDialog::MsdDialog(PiKvmClient* master, QWidget* parent)
    : QDialog(parent, Qt::Dialog | Qt::WindowCloseButtonHint),
      m_master(master) {
    setWindowTitle(QStringLiteral("Mass Storage — %1")
                       .arg(master ? master->host() : QString()));

    m_statusLabel  = new QLabel(QStringLiteral("Status: (waiting…)"), this);
    m_storageLabel = new QLabel(QStringLiteral("Storage: -"), this);
    m_activeLabel  = new QLabel(QStringLiteral("Active: -"), this);

    m_imageList = new QListWidget(this);
    m_imageList->setSelectionMode(QAbstractItemView::SingleSelection);

    m_useBtn        = new QPushButton(QStringLiteral("Use Selected"));
    m_deleteBtn     = new QPushButton(QStringLiteral("Delete Selected"));
    m_uploadBtn     = new QPushButton(QStringLiteral("Upload New Image…"));
    m_connectBtn    = new QPushButton(QStringLiteral("Connect to Target"));
    m_disconnectBtn = new QPushButton(QStringLiteral("Disconnect"));

    m_cdromCheck = new QCheckBox(QStringLiteral(
        "Present as CD-ROM (read-only). Untick for read/write disk mode."));

    auto* listButtons = new QHBoxLayout;
    listButtons->addWidget(m_useBtn);
    listButtons->addWidget(m_deleteBtn);
    listButtons->addStretch(1);

    auto* connectButtons = new QHBoxLayout;
    connectButtons->addWidget(m_connectBtn);
    connectButtons->addWidget(m_disconnectBtn);
    connectButtons->addStretch(1);

    auto* closeBtn = new QPushButton(QStringLiteral("Close"));
    closeBtn->setDefault(true);
    auto* footer = new QHBoxLayout;
    footer->addStretch(1);
    footer->addWidget(closeBtn);
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::accept);

    auto* root = new QVBoxLayout(this);
    root->addWidget(m_statusLabel);
    root->addWidget(m_storageLabel);
    root->addWidget(m_activeLabel);
    root->addSpacing(8);
    root->addWidget(new QLabel(QStringLiteral("Stored images:"), this));
    root->addWidget(m_imageList, 1);
    root->addLayout(listButtons);
    root->addSpacing(8);
    root->addWidget(m_cdromCheck);
    root->addWidget(m_uploadBtn);
    root->addSpacing(8);
    root->addLayout(connectButtons);
    root->addSpacing(8);
    root->addLayout(footer);

    resize(560, 480);

    if (m_master) {
        connect(m_master.data(), &PiKvmClient::rawStateEvent,
                this, &MsdDialog::onStateEvent);
        connect(m_master.data(), &PiKvmClient::msdUploadProgress,
                this, &MsdDialog::onUploadProgress);
        connect(m_master.data(), &PiKvmClient::msdUploadFinished,
                this, &MsdDialog::onUploadFinished);
    }

    connect(m_uploadBtn,     &QPushButton::clicked, this, &MsdDialog::onUploadClicked);
    connect(m_useBtn,        &QPushButton::clicked, this, &MsdDialog::onUseSelectedClicked);
    connect(m_deleteBtn,     &QPushButton::clicked, this, &MsdDialog::onDeleteSelectedClicked);
    connect(m_connectBtn,    &QPushButton::clicked, this, &MsdDialog::onConnectClicked);
    connect(m_disconnectBtn, &QPushButton::clicked, this, &MsdDialog::onDisconnectClicked);
    connect(m_cdromCheck,    &QCheckBox::toggled,   this, &MsdDialog::onCdromToggled);

    // Disabled until the first msd_state event lands.
    m_useBtn->setEnabled(false);
    m_deleteBtn->setEnabled(false);
    m_connectBtn->setEnabled(false);
    m_disconnectBtn->setEnabled(false);
}

MsdDialog::~MsdDialog() = default;

void MsdDialog::toggle() {
    if (isVisible()) {
        hide();
    } else {
        show();
        raise();
        activateWindow();
    }
}

void MsdDialog::onStateEvent(const QString& type, const QJsonObject& event) {
    if (type != QLatin1String("msd_state")) return;

    const bool enabled = event.value(QStringLiteral("enabled")).toBool();
    const bool online  = event.value(QStringLiteral("online")).toBool();
    const bool busy    = event.value(QStringLiteral("busy")).toBool();
    m_statusLabel->setText(QStringLiteral("Status: %1, %2%3")
        .arg(enabled ? QStringLiteral("enabled") : QStringLiteral("disabled"),
             online  ? QStringLiteral("online")  : QStringLiteral("offline"),
             busy    ? QStringLiteral(", busy")  : QString()));

    const auto storage = event.value(QStringLiteral("storage")).toObject();
    const auto sz   = static_cast<qint64>(storage.value(QStringLiteral("size")).toDouble());
    const auto free = static_cast<qint64>(storage.value(QStringLiteral("free")).toDouble());
    const bool uploading = storage.value(QStringLiteral("uploading")).toBool();
    m_storageLabel->setText(QStringLiteral("Storage: %1 free / %2 total%3")
        .arg(formatBytes(free), formatBytes(sz),
             uploading ? QStringLiteral(" (uploading…)") : QString()));

    // Stored-image list. Preserve selection across refreshes when possible.
    const QString prevSelected = m_imageList->currentItem()
        ? m_imageList->currentItem()->data(Qt::UserRole).toString()
        : QString();
    m_imageList->clear();
    const auto images = storage.value(QStringLiteral("images")).toObject();
    for (auto it = images.constBegin(); it != images.constEnd(); ++it) {
        const auto info = it.value().toObject();
        const auto isz  = static_cast<qint64>(info.value(QStringLiteral("size")).toDouble());
        auto* item = new QListWidgetItem(
            QStringLiteral("%1   —   %2").arg(it.key(), formatBytes(isz)));
        item->setData(Qt::UserRole, it.key());
        m_imageList->addItem(item);
        if (it.key() == prevSelected) m_imageList->setCurrentItem(item);
    }

    const auto drive = event.value(QStringLiteral("drive")).toObject();
    m_currentImage       = drive.value(QStringLiteral("image")).toString();
    m_currentlyConnected = drive.value(QStringLiteral("connected")).toBool();
    m_currentCdrom       = drive.value(QStringLiteral("cdrom")).toBool();
    m_activeLabel->setText(QStringLiteral(
        "Active: %1   (cdrom: %2, connected: %3)")
        .arg(m_currentImage.isEmpty() ? QStringLiteral("(none)") : m_currentImage,
             m_currentCdrom       ? QStringLiteral("yes") : QStringLiteral("no"),
             m_currentlyConnected ? QStringLiteral("yes") : QStringLiteral("no")));

    // Sync the cdrom checkbox without re-emitting toggled() while we're
    // just reflecting server state.
    QSignalBlocker blocker(m_cdromCheck);
    m_cdromCheck->setChecked(m_currentCdrom);

    const bool hasImages = m_imageList->count() > 0;
    m_useBtn->setEnabled(hasImages && !m_currentlyConnected);
    m_deleteBtn->setEnabled(hasImages && !m_currentlyConnected);
    m_connectBtn->setEnabled(!m_currentImage.isEmpty() && !m_currentlyConnected);
    m_disconnectBtn->setEnabled(m_currentlyConnected);
    // Disable the upload button while the server reports it's already
    // accepting an upload — kvmd serialises uploads.
    m_uploadBtn->setEnabled(!uploading);
}

// ---------------------------------------------------------------------------

void MsdDialog::onUploadClicked() {
    if (!m_master) return;
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("Upload image"), QString(),
        QStringLiteral("Disk images (*.iso *.img *.bin);;All files (*)"));
    if (path.isEmpty()) return;

    auto* file = new QFile(path);
    if (!file->open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, QStringLiteral("Upload"),
            QStringLiteral("Cannot open %1: %2").arg(path, file->errorString()));
        delete file;
        return;
    }
    m_uploadFile = file;

    const QString imageName = QFileInfo(path).fileName();
    const qint64 totalBytes = file->size();

    m_uploadProgress = new QProgressDialog(
        QStringLiteral("Uploading %1…").arg(imageName),
        QStringLiteral("Cancel"),
        0, totalBytes > 0 ? int(totalBytes / 1024) : 0,
        this);
    m_uploadProgress->setWindowModality(Qt::WindowModal);
    m_uploadProgress->setMinimumDuration(0);
    m_uploadProgress->show();

    m_master->msdUpload(imageName, file);
}

void MsdDialog::onUploadProgress(qint64 sent, qint64 total) {
    if (!m_uploadProgress) return;
    if (total > 0) {
        m_uploadProgress->setMaximum(int(total / 1024));
    }
    m_uploadProgress->setValue(int(sent / 1024));
}

void MsdDialog::onUploadFinished(bool ok, const QString& reason) {
    if (m_uploadProgress) {
        m_uploadProgress->close();
        m_uploadProgress->deleteLater();
        m_uploadProgress = nullptr;
    }
    if (m_uploadFile) {
        m_uploadFile->close();
        m_uploadFile->deleteLater();
        m_uploadFile = nullptr;
    }
    if (!ok) {
        QMessageBox::warning(this, QStringLiteral("Upload failed"), reason);
    }
}

void MsdDialog::onUseSelectedClicked() {
    if (!m_master) return;
    auto* item = m_imageList->currentItem();
    if (!item) return;
    const QString name = item->data(Qt::UserRole).toString();
    m_master->msdSetParams(name, m_currentCdrom, /*rw=*/!m_currentCdrom);
}

void MsdDialog::onDeleteSelectedClicked() {
    if (!m_master) return;
    auto* item = m_imageList->currentItem();
    if (!item) return;
    const QString name = item->data(Qt::UserRole).toString();
    if (QMessageBox::question(this, QStringLiteral("Delete image"),
            QStringLiteral("Permanently delete '%1' from the PiKVM?").arg(name),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No) == QMessageBox::Yes) {
        m_master->msdRemove(name);
    }
}

void MsdDialog::onConnectClicked() {
    if (m_master) m_master->msdSetConnected(true);
}

void MsdDialog::onDisconnectClicked() {
    if (m_master) m_master->msdSetConnected(false);
}

void MsdDialog::onCdromToggled(bool cdrom) {
    if (!m_master) return;
    if (m_currentImage.isEmpty()) return;  // nothing to set against
    // rw is only meaningful when not in CDROM mode; flip it inversely so
    // a writable disk image isn't accidentally CDROM-only.
    m_master->msdSetParams(m_currentImage, cdrom, /*rw=*/!cdrom);
}

}  // namespace glasshouse
