// Copyright (c) 2026 The GeoCoin developers
// Distributed under the MIT software license

#pragma once

#include "nftpage.h"
#include "clientmodel.h"
#include "walletmodel.h"
#include "guiutil.h"

#include "geocoin/state/state.hpp"
#include "geocoin/wallet/wallet.hpp"
#include "geocoin/crypto/geo_area.hpp"
#include "geocoin/consensus/consensus.hpp"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QHeaderView>
#include <QTextEdit>
#include <QInputDialog>
#include <QLineEdit>
#include <QMessageBox>
#include <QDialog>
#include <QFormLayout>
#include <QDialogButtonBox>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QDateTimeEdit>
#include <QCheckBox>
#include <QScrollArea>
#include <QFrame>
#include <QFileDialog>
#include <QFileInfo>
#include <QDir>
#include <QDateTime>
#include <QImage>
#include <QPainter>
#include <QFont>
#include <QFontDatabase>
#include <QColor>
#include <QUrl>
#include <QImageWriter>
#include <QCalendarWidget>
#include <QRegularExpression>
#include <QMap>
#include <QSettings>
#include <QCryptographicHash>

#ifdef USE_QRCODE
#include <qrencode.h>
#endif

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

static QString bytesToHex(const geocoin::bytes& data) {
    return QString::fromStdString(geocoin::crypto::bytes_to_hex(data));
}

static bool isNullHash(const geocoin::Hash256& h) {
    for (auto b : h) {
        if (b != 0) {
            return false;
        }
    }
    return true;
}

static QString resolveImageUriToLocalPath(const QString& imageUri, const QString& dataDir) {
    const QString uri = imageUri.trimmed();
    if (uri.startsWith(QStringLiteral("local://"), Qt::CaseInsensitive)) {
        if (dataDir.isEmpty()) {
            return QString();
        }
        QString rel = uri.mid(QStringLiteral("local://").size());
        while (rel.startsWith('/')) {
            rel.remove(0, 1);
        }
        const QString cleaned = QDir::cleanPath(rel);
        if (cleaned.startsWith("..")) {
            return QString();
        }
        return QDir(dataDir).filePath(cleaned);
    }
    if (uri.startsWith(QStringLiteral("file://"), Qt::CaseInsensitive)) {
        return QUrl(uri).toLocalFile();
    }
    return QString();
}

static bool createStampedCopy(const QString& srcPath, const QString& dstPath, const QString& stampText, const QString& qrData) {
    QImage img(srcPath);
    if (img.isNull()) {
        return false;
    }

    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::TextAntialiasing, true);

    QFont font;
    font.setPointSize(std::max(10, img.width() / 60));
    font.setWeight(QFont::DemiBold);

    // Pick a nicer font if available (fallback to default if not installed)
    {
        const QStringList preferredFamilies = {
            QStringLiteral("Montserrat"),
            QStringLiteral("Inter"),
            QStringLiteral("Noto Sans"),
            QStringLiteral("DejaVu Sans"),
            QStringLiteral("Arial")
        };
        const QStringList available = QFontDatabase().families();
        for (const QString& fam : preferredFamilies) {
            if (available.contains(fam, Qt::CaseInsensitive)) {
                font.setFamily(fam);
                break;
            }
        }
    }

    p.setFont(font);

    const QString text = stampText;
    QFontMetrics fm(font);
    const int padding = 12;
    const QRect textRect = fm.boundingRect(text);
    const int boxW = textRect.width() + padding * 2;
    const int boxH = textRect.height() + padding * 2;

    // Place descriptor at bottom-left. QR stays at bottom-right.
    const int x = 10;
    const int y = img.height() - boxH - 10;
    QRect box(x, y, boxW, boxH);

    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0, 0, 0, 140));
    p.drawRoundedRect(box, 8, 8);

    p.setPen(Qt::white);
    p.setFont(font);
    p.drawText(box.adjusted(padding, padding, -padding, -padding), Qt::AlignLeft | Qt::AlignVCenter, stampText);

#ifdef USE_QRCODE
    if (!qrData.trimmed().isEmpty()) {
        QRcode *code = QRcode_encodeString(qrData.toUtf8().constData(), 0, QR_ECLEVEL_M, QR_MODE_8, 1);
        if (code) {
            const int qrSize = code->width;
            const int qrImageSize = 180;
            const int scale = std::max(1, qrImageSize / std::max(1, qrSize));

            QImage qrImg(qrSize * scale, qrSize * scale, QImage::Format_RGB32);
            qrImg.fill(Qt::white);
            QPainter qp(&qrImg);
            qp.setPen(Qt::NoPen);
            qp.setBrush(Qt::black);
            for (int y = 0; y < qrSize; ++y) {
                for (int x = 0; x < qrSize; ++x) {
                    if (code->data[y * qrSize + x] & 1) {
                        qp.drawRect(x * scale, y * scale, scale, scale);
                    }
                }
            }
            qp.end();
            QRcode_free(code);

            const int margin = 16;
            QRect qrRect(img.width() - qrImg.width() - margin,
                         img.height() - qrImg.height() - margin,
                         qrImg.width(),
                         qrImg.height());

            // White frame behind QR for readability
            QRect frame = qrRect.adjusted(-8, -8, 8, 8);
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(255, 255, 255, 230));
            p.drawRoundedRect(frame, 6, 6);
            p.drawImage(qrRect.topLeft(), qrImg);
        }
    }
#endif
    p.end();

    QDir().mkpath(QFileInfo(dstPath).absolutePath());
    return img.save(dstPath);
}

#ifdef USE_QRCODE
static void drawQrCode(QPainter& p, const QRect& targetRect, const QString& qrData) {
    if (qrData.trimmed().isEmpty()) {
        return;
    }
    QRcode *code = QRcode_encodeString(qrData.toUtf8().constData(), 0, QR_ECLEVEL_M, QR_MODE_8, 1);
    if (!code) {
        return;
    }

    const int qrSize = code->width;
    const int scale = std::max(1, targetRect.width() / std::max(1, qrSize));
    QImage qrImg(qrSize * scale, qrSize * scale, QImage::Format_RGB32);
    qrImg.fill(Qt::white);
    QPainter qp(&qrImg);
    qp.setPen(Qt::NoPen);
    qp.setBrush(Qt::black);
    for (int y = 0; y < qrSize; ++y) {
        for (int x = 0; x < qrSize; ++x) {
            if (code->data[y * qrSize + x] & 1) {
                qp.drawRect(x * scale, y * scale, scale, scale);
            }
        }
    }
    qp.end();
    QRcode_free(code);

    // Center QR in target rect and add a light frame
    const QPoint topLeft(
        targetRect.x() + (targetRect.width() - qrImg.width()) / 2,
        targetRect.y() + (targetRect.height() - qrImg.height()) / 2
    );
    QRect frame = QRect(topLeft, qrImg.size()).adjusted(-8, -8, 8, 8);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(255, 255, 255, 230));
    p.drawRoundedRect(frame, 6, 6);
    p.drawImage(topLeft, qrImg);
}
#endif

static QString elideMiddle(const QFontMetrics& fm, const QString& text, int maxWidth) {
    if (fm.horizontalAdvance(text) <= maxWidth) {
        return text;
    }
    const QString ell = QStringLiteral("…");
    int left = 0;
    int right = text.size();
    QString best = text;
    while (left < right) {
        int keep = (right - left) / 2;
        const int leftCount = keep / 2;
        const int rightCount = keep - leftCount;
        const QString candidate = text.left(leftCount) + ell + text.right(rightCount);
        if (fm.horizontalAdvance(candidate) > maxWidth) {
            right = keep - 1;
        } else {
            best = candidate;
            left = keep + 1;
        }
    }
    return best;
}

static QString formatPolygonCompact(const std::vector<geocoin::nft::NFTMetadata::GeoPoint>& polygon, int maxPoints) {
    if (polygon.empty()) {
        return QString();
    }
    const int count = static_cast<int>(polygon.size());
    const int take = std::min(maxPoints, count);
    QStringList parts;
    for (int i = 0; i < take; ++i) {
        const auto& p = polygon[static_cast<size_t>(i)];
        const double lat = static_cast<double>(p.lat_ndeg) * 1e-9;
        const double lon = static_cast<double>(p.lon_ndeg) * 1e-9;
        parts << (QString::number(lat, 'f', 9) + QStringLiteral(",") + QString::number(lon, 'f', 9));
    }
    if (count > take) {
        parts << QStringLiteral("...");
    }
    return parts.join(QStringLiteral("; "));
}

static bool createCadastralCertificate(
    const QString& templatePath,
    const QString& dstPath,
    const QMap<QString, QString>& fields,
    const QString& qrData
) {
    QImage img(templatePath);
    if (img.isNull()) {
        return false;
    }

    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::TextAntialiasing, true);

    QFont font;
    font.setPointSize(std::max(10, img.height() / 95));
    font.setWeight(QFont::Normal);
    {
        const QStringList preferredFamilies = {
            QStringLiteral("Cinzel"),
            QStringLiteral("Garamond"),
            QStringLiteral("Times New Roman"),
            QStringLiteral("Georgia"),
            QStringLiteral("DejaVu Serif")
        };
        const QStringList available = QFontDatabase().families();
        for (const QString& fam : preferredFamilies) {
            if (available.contains(fam, Qt::CaseInsensitive)) {
                font.setFamily(fam);
                break;
            }
        }
    }
    p.setFont(font);
    p.setPen(QColor(60, 40, 20));

    const int W = img.width();
    const int H = img.height();
    auto px = [&](double nx) { return int(nx * W); };
    auto py = [&](double ny) { return int(ny * H); };

    auto draw = [&](const QString& key, double nx, double ny, int maxW) {
        const QString value = fields.value(key);
        if (value.isEmpty()) {
            return;
        }
        const QString text = elideMiddle(QFontMetrics(font), value, maxW);
        p.drawText(QPoint(px(nx), py(ny)), text);
    };

    // Header (top-right)
    draw("issue_date", 0.70, 0.105, px(0.25));
    draw("issuer",     0.70, 0.125, px(0.25));

    // Cadastral data (left column)
    draw("cadastral_center", 0.13, 0.275, px(0.35));
    draw("land_registry",    0.13, 0.300, px(0.35));
    draw("parcel_id",        0.13, 0.325, px(0.35));
    draw("area",             0.13, 0.350, px(0.35));
    draw("location_name",    0.13, 0.375, px(0.35));

    // Cadastral data (right column)
    draw("owner_address", 0.62, 0.275, px(0.30));
    draw("owner_name",    0.62, 0.300, px(0.30));
    draw("building_info", 0.62, 0.325, px(0.30));

    // Geometry
    draw("coordinates", 0.13, 0.505, px(0.75));
    draw("area_wgs84",   0.13, 0.530, px(0.75));
    draw("map_data",     0.13, 0.555, px(0.75));

    // Document hashes
    draw("doc_hash",      0.13, 0.645, px(0.75));
    draw("metadata_hash", 0.13, 0.670, px(0.75));
    draw("photo_hash",    0.13, 0.695, px(0.75));

    // Blockchain proof
    draw("block_id",     0.13, 0.770, px(0.45));
    draw("blockchain_por",   0.13, 0.795, px(0.75));
    draw("network",      0.62, 0.770, px(0.30));
    draw("timestamp",    0.62, 0.795, px(0.30));
    draw("doc_hash_proof", 0.13, 0.820, px(0.75));

#ifdef USE_QRCODE
    const QRect qrRect(px(0.08), py(0.865), px(0.16), px(0.16));
    drawQrCode(p, qrRect, qrData);
#endif

    p.end();

    QDir().mkpath(QFileInfo(dstPath).absolutePath());
    return img.save(dstPath);
}

static QString hashToHex(const geocoin::Hash256& hash) {
    return bytesToHex(geocoin::bytes(hash.begin(), hash.end()));
}

static QString buildNftQrDataFromWallet(const std::shared_ptr<geocoin::wallet::Wallet>& wallet,
                                       const QString& preferTxid,
                                       const QString& preferImageUri,
                                       const QString& preferName)
{
    if (!wallet) {
        return QString();
    }

    const auto txs = wallet->get_transactions("", 0);

    // 1) Prefer exact txid match (the one we just created)
    for (const auto& wtx : txs) {
        geocoin::bytes hb(wtx.tx_hash.begin(), wtx.tx_hash.end());
        const QString wtxid = QString::fromStdString(geocoin::crypto::bytes_to_hex(hb));
        if (!preferTxid.isEmpty() && wtxid != preferTxid) {
            continue;
        }
        if (wtx.tx.type != geocoin::core::TransactionType::NFT_CREATE || !wtx.tx.nft_create.has_value()) {
            continue;
        }
        const QString nftIdHex = hashToHex(wtx.tx.nft_create->nft_id);
        const QString ch = QString::fromStdString(wtx.tx.nft_create->metadata.content_hash);
        if (!nftIdHex.isEmpty() && !ch.isEmpty()) {
            return QStringLiteral("geocoin:nft:") + nftIdHex + QStringLiteral("?ch=") + ch;
        }
    }

    // 2) Fallback: best-effort match by metadata fields for pending tx
    const geocoin::wallet::WalletTransaction* best = nullptr;
    uint64_t bestTs = 0;
    for (const auto& wtx : txs) {
        if (wtx.block_height != 0) {
            continue;
        }
        if (wtx.tx.type != geocoin::core::TransactionType::NFT_CREATE || !wtx.tx.nft_create.has_value()) {
            continue;
        }

        const auto& md = wtx.tx.nft_create->metadata;
        const QString img = QString::fromStdString(md.image_uri);
        const QString nm = QString::fromStdString(md.name);

        bool matches = false;
        if (!preferImageUri.isEmpty() && img == preferImageUri) {
            matches = true;
        }
        if (!preferName.isEmpty() && nm == preferName) {
            matches = true;
        }
        if (!matches) {
            continue;
        }
        if (wtx.timestamp >= bestTs) {
            bestTs = wtx.timestamp;
            best = &wtx;
        }
    }

    // 3) Fallback: just take the most recent pending NFT_CREATE
    if (!best) {
        for (const auto& wtx : txs) {
            if (wtx.block_height != 0) {
                continue;
            }
            if (wtx.tx.type != geocoin::core::TransactionType::NFT_CREATE || !wtx.tx.nft_create.has_value()) {
                continue;
            }
            if (wtx.timestamp >= bestTs) {
                bestTs = wtx.timestamp;
                best = &wtx;
            }
        }
    }

    if (!best || !best->tx.nft_create.has_value()) {
        return QString();
    }

    const QString nftIdHex = hashToHex(best->tx.nft_create->nft_id);
    const QString ch = QString::fromStdString(best->tx.nft_create->metadata.content_hash);
    if (nftIdHex.isEmpty() || ch.isEmpty()) {
        return QString();
    }
    return QStringLiteral("geocoin:nft:") + nftIdHex + QStringLiteral("?ch=") + ch;
}

static bool hexToBytes(const QString& input, geocoin::bytes& out) {
    QString filtered;
    filtered.reserve(input.size());
    for (QChar ch : input) {
        if (!ch.isSpace()) {
            filtered.append(ch);
        }
    }
    if (filtered.length() % 2 != 0) {
        return false;
    }
    out.clear();
    out.reserve(filtered.length() / 2);
    auto hexValue = [](QChar c) -> int {
        if (c >= '0' && c <= '9') return c.unicode() - '0';
        if (c >= 'a' && c <= 'f') return c.unicode() - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c.unicode() - 'A' + 10;
        return -1;
    };
    for (int i = 0; i < filtered.length(); i += 2) {
        int hi = hexValue(filtered[i]);
        int lo = hexValue(filtered[i + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        out.push_back(static_cast<geocoin::byte>((hi << 4) | lo));
    }
    return true;
}

static bool hexToHash(const QString& hex, geocoin::Hash256& out) {
    geocoin::bytes b;
    if (!hexToBytes(hex, b) || b.size() != out.size()) {
        return false;
    }
    std::copy(b.begin(), b.end(), out.begin());
    return true;
}

static QString promptPasswordIfNeeded(QWidget* parent, WalletModel* walletModel) {
    if (!walletModel) {
        return QString();
    }
    if (!walletModel->isWalletEncrypted()) {
        return QString();
    }
    QInputDialog inputDialog(parent);
    inputDialog.setWindowTitle(QObject::tr("Wallet Password"));
    inputDialog.setLabelText(QObject::tr("Enter wallet password to sign transaction:"));
    inputDialog.setTextEchoMode(QLineEdit::Password);
    GUIUtil::applyDialogStyle(&inputDialog);
    if (inputDialog.exec() != QDialog::Accepted) {
        return QString();
    }
    QString password = inputDialog.textValue();
    if (password.isEmpty()) {
        return QString();
    }
    if (!walletModel->verifyPassword(password)) {
        GUIUtil::showStyledMessageBox(
            parent,
            QMessageBox::Warning,
            QObject::tr("NFT"),
            QObject::tr("Incorrect password.")
        );
        inputDialog.setTextValue(QString());
        GUIUtil::cleanseQString(password);
        return QString();
    }
    inputDialog.setTextValue(QString(password.size(), QChar(0)));
    return password;
}
}

static QString lightDialogStyleSheet() {
    return GUIUtil::dialogStyleSheet(GUIUtil::currentTheme());
}

static bool lightTextPrompt(QWidget* parent,
                            const QString& title,
                            const QString& label,
                            QString& out,
                            const QString& initialValue = QString()) {
    QInputDialog dlg(parent);
    dlg.setWindowTitle(title);
    dlg.setLabelText(label);
    dlg.setTextEchoMode(QLineEdit::Normal);
    dlg.setTextValue(initialValue);
    dlg.setOption(QInputDialog::UseListViewForComboBoxItems, false);
    dlg.setStyleSheet(lightDialogStyleSheet());
    if (dlg.exec() != QDialog::Accepted) {
        return false;
    }
    out = dlg.textValue();
    return true;
}

static void lightWarning(QWidget* parent, const QString& title, const QString& text) {
    GUIUtil::showStyledMessageBox(parent, QMessageBox::Warning, title, text);
}

static void lightInfo(QWidget* parent, const QString& title, const QString& text) {
    GUIUtil::showStyledMessageBox(parent, QMessageBox::Information, title, text);
}

static bool lightConfirm(QWidget* parent, const QString& title, const QString& text) {
    return GUIUtil::showStyledMessageBox(
        parent,
        QMessageBox::Question,
        title,
        text,
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No
    ) == QMessageBox::Yes;
}

static QString statusToMessage(WalletModel::StatusCode status) {
    switch (status) {
        case WalletModel::InvalidAmount: return QObject::tr("Invalid amount.");
        case WalletModel::InvalidAddress: return QObject::tr("Invalid address.");
        case WalletModel::AmountExceedsBalance: return QObject::tr("Amount exceeds balance.");
        case WalletModel::AmountWithFeeExceedsBalance: return QObject::tr("Amount with fee exceeds balance.");
        case WalletModel::TransactionCreationFailed: return QObject::tr("Transaction creation failed.");
        case WalletModel::TransactionCommitFailed: return QObject::tr("Transaction commit failed (mempool reject).");
        case WalletModel::WalletSaveFailed: return QObject::tr(
            "Transaction was broadcast, but the wallet file was not saved. Save the wallet before exiting.");
        case WalletModel::AbsurdFee: return QObject::tr("Fee is too high.");
        case WalletModel::PaymentRequestExpired: return QObject::tr("Payment request expired.");
        default: return QObject::tr("Unknown error.");
    }
}
