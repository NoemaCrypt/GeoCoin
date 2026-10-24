// Copyright (c) 2011-2014 The Bitcoin developers
// Copyright (c) 2025 The GeoCoin developers
// Distributed under the MIT software license

#include "receivecoinspage.h"
#include "clientmodel.h"
#include "walletmodel.h"
#include "guiutil.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QMessageBox>
#include <QApplication>
#include <QClipboard>
#include <QImage>
#include <QPainter>

#ifdef USE_QRCODE
#include <qrencode.h>
#endif

ReceiveCoinsPage::ReceiveCoinsPage(QWidget *parent) :
    QWidget(parent),
    clientModel(nullptr),
    walletModel(nullptr),
    addressEdit(nullptr),
    labelQR(nullptr),
    infoLabel(nullptr),
    buttonCopy(nullptr)
{
    setupUi();
}

ReceiveCoinsPage::~ReceiveCoinsPage()
{
}

void ReceiveCoinsPage::setupUi()
{
    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->setSpacing(20);

    // Title
    QLabel *titleLabel = new QLabel(tr("Receive Coins"), this);
    QFont titleFont = titleLabel->font();
    titleFont.setPointSize(16);
    titleFont.setBold(true);
    titleLabel->setFont(titleFont);
    layout->addWidget(titleLabel);

    // Address group
    QGroupBox *addressGroup = new QGroupBox(tr("Your Address"), this);
    QVBoxLayout *addressLayout = new QVBoxLayout(addressGroup);

    // Address display
    QHBoxLayout *addressDisplayLayout = new QHBoxLayout();
    addressEdit = new QLineEdit(this);
    addressEdit->setReadOnly(true);
    addressEdit->setFont(GUIUtil::geocoinAddressFont());
    addressEdit->setPlaceholderText(tr("No wallet loaded"));
    addressDisplayLayout->addWidget(addressEdit);

    buttonCopy = new QPushButton(tr("Copy"), this);
    buttonCopy->setMinimumWidth(80);
    connect(buttonCopy, &QPushButton::clicked, this, &ReceiveCoinsPage::copyAddress);
    addressDisplayLayout->addWidget(buttonCopy);

    addressLayout->addLayout(addressDisplayLayout);

    layout->addWidget(addressGroup);

    // QR Code placeholder
    QGroupBox *qrGroup = new QGroupBox(tr("QR Code"), this);
    QVBoxLayout *qrLayout = new QVBoxLayout(qrGroup);

    labelQR = new QLabel(this);
    labelQR->setMinimumSize(256, 256);
    labelQR->setMaximumSize(256, 256);
    labelQR->setAlignment(Qt::AlignCenter);
    labelQR->setStyleSheet("QLabel { background-color: white; border: 1px solid gray; }");
    labelQR->setText(tr("QR code will appear after wallet load"));
    qrLayout->addWidget(labelQR, 0, Qt::AlignCenter);

    layout->addWidget(qrGroup);

    // Info text
    infoLabel = new QLabel(
        tr("<b>Note:</b> Share this address with others to receive GeoCoin payments. "
           "The address is derived from your wallet's private key."),
        this
    );
    infoLabel->setWordWrap(true);
    infoLabel->setStyleSheet(GUIUtil::warningLabelStyleSheet(GUIUtil::currentTheme()));
    layout->addWidget(infoLabel);

    layout->addStretch();
    applyTheme();
}

void ReceiveCoinsPage::applyTheme()
{
    const QString theme = GUIUtil::currentTheme();
    setStyleSheet(QStringLiteral("ReceiveCoinsPage { background-color: transparent; }")
                  + GUIUtil::glassPanelStyleSheet(theme)
                  + GUIUtil::glassInputStyleSheet(theme)
                  + GUIUtil::glassListStyleSheet(theme));
    if (infoLabel) {
        infoLabel->setStyleSheet(GUIUtil::warningLabelStyleSheet(theme));
    }
    if (labelQR) {
        labelQR->setStyleSheet(QStringLiteral("QLabel { background-color: white; color: #111111; border: 1px solid gray; }"));
    }
}

void ReceiveCoinsPage::setClientModel(ClientModel *model)
{
    this->clientModel = model;
}

void ReceiveCoinsPage::setWalletModel(WalletModel *model)
{
    if (walletModel && walletModel != model) {
        disconnect(walletModel, nullptr, this, nullptr);
    }
    this->walletModel = model;
    if (walletModel) {
        connect(walletModel, &WalletModel::walletStateChanged,
                this, &ReceiveCoinsPage::updateAddress,
                Qt::UniqueConnection);
    }
    updateAddress();
}

void ReceiveCoinsPage::updateAddress()
{
    if (walletModel && walletModel->isWalletLoaded()) {
        QString address = walletModel->getAddress();

        addressEdit->setText(address);
        buttonCopy->setEnabled(true);

        // Generate QR code
        generateQRCode(address);
    } else {
        addressEdit->clear();
        addressEdit->setPlaceholderText(tr("No wallet loaded"));
        buttonCopy->setEnabled(false);

        // Clear QR code
        labelQR->clear();
        labelQR->setText(tr("QR Code\n(No wallet loaded)"));
    }
}

void ReceiveCoinsPage::generateQRCode(const QString &address)
{
#ifdef USE_QRCODE
    if (address.isEmpty()) {
        labelQR->clear();
        labelQR->setText(tr("QR Code\n(No address)"));
        return;
    }

    // Generate QR code
    QRcode *code = QRcode_encodeString(address.toUtf8().constData(), 0, QR_ECLEVEL_L, QR_MODE_8, 1);

    if (!code) {
        labelQR->clear();
        labelQR->setText(tr("QR Code\n(Generation failed)"));
        return;
    }

    // Create QImage from QR code
    int qrSize = code->width;
    int qrImageSize = 256;
    int scale = qrImageSize / qrSize;

    QImage qrImage(qrSize * scale, qrSize * scale, QImage::Format_RGB32);
    qrImage.fill(Qt::white);

    QPainter painter(&qrImage);
    painter.setPen(Qt::NoPen);
    painter.setBrush(Qt::black);

    for (int y = 0; y < qrSize; ++y) {
        for (int x = 0; x < qrSize; ++x) {
            if (code->data[y * qrSize + x] & 1) {
                painter.drawRect(x * scale, y * scale, scale, scale);
            }
        }
    }

    QRcode_free(code);

    // Display QR code
    labelQR->setPixmap(QPixmap::fromImage(qrImage));
#else
    labelQR->clear();
    labelQR->setText(tr("QR unavailable:\napplication built without libqrencode"));
#endif
}

void ReceiveCoinsPage::showStyledMessageBox(int icon, const QString &title, const QString &text)
{
    GUIUtil::showStyledMessageBox(this, static_cast<QMessageBox::Icon>(icon), title, text);
}

void ReceiveCoinsPage::copyAddress()
{
    QString address = addressEdit->text();
    if (!address.isEmpty()) {
        GUIUtil::setClipboard(address);
        showStyledMessageBox(1, tr("Address Copied"),
            tr("Address copied to clipboard!"));
    }
}
