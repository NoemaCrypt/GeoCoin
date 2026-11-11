// Copyright (c) 2011-2014 The Bitcoin developers
// Copyright (c) 2025 The GeoCoin developers
// Distributed under the MIT software license

#ifndef GEOCOIN_QT_GUIUTIL_H
#define GEOCOIN_QT_GUIUTIL_H

#include "geocoin/utils/secure_memory.hpp"

#include <QByteArray>
#include <QMessageBox>
#include <QString>
#include <QFont>
#include <string>

QT_BEGIN_NAMESPACE
class QApplication;
class QWidget;
QT_END_NAMESPACE

/** Utility functions used by the GeoCoin Qt UI */
namespace GUIUtil
{
    /** Best-effort overwrite of a QString that held a secret. */
    void cleanseQString(QString& value);

    /** Ensures a sensitive QString is overwritten on every return path. */
    class ScopedQStringCleanser final
    {
    public:
        explicit ScopedQStringCleanser(QString& value) : value_(value) {}
        ~ScopedQStringCleanser() { cleanseQString(value_); }

        ScopedQStringCleanser(const ScopedQStringCleanser&) = delete;
        ScopedQStringCleanser& operator=(const ScopedQStringCleanser&) = delete;

    private:
        QString& value_;
    };

    /** Best-effort overwrite of a std::string that held a secret. */
    void cleanseString(std::string& value);

    using ScopedStringCleanser = geocoin::utils::ScopedContainerCleanser<std::string>;

    /** Read one 32-byte hex key through the shared owner-only secret-file policy. */
    bool readPrivateKeyFileHex(
        const QString& path,
        QString& privateKeyHex,
        QString& error
    );

    /** Atomically replace a user-owned file or leave the previous file intact. */
    bool writeFileAtomically(
        const QString& path,
        const QByteArray& data,
        QString& error
    );

    /** Parse a non-negative GC decimal into angles exactly (up to 8 decimals). */
    bool parseAmount(const QString& text, qint64& amount);
    /** Add monetary values without signed overflow. */
    bool addAmounts(qint64 lhs, qint64 rhs, qint64& result);

    // Format amounts
    QString formatAmount(qint64 amount, int unit = 0);
    QString formatAmountWithUnit(qint64 amount, int unit = 0);

    // Format addresses
    QString formatAddress(const QString &address);

    // Format hashrate
    QString formatHashrate(double hashrate);

    // CPU
    int availableCpuThreads();

    // Format time
    QString formatDuration(int secs);
    QString formatDateTime(qint64 timestamp);

    // Fonts
    QFont geocoinAddressFont();
    QFont fixedPitchFont();

    // Clipboard
    void setClipboard(const QString &str);

    // File dialogs
    QString getSaveFileName(QWidget *parent, const QString &caption,
                           const QString &dir, const QString &filter,
                           QString *selectedFilter);
    QString getOpenFileName(QWidget *parent, const QString &caption,
                           const QString &dir, const QString &filter,
                           QString *selectedFilter);

    // Message boxes
    void showMessageBox(const QString &title, const QString &text,
                       unsigned int style = 0);
    QMessageBox::StandardButton showStyledMessageBox(
        QWidget *parent,
        QMessageBox::Icon icon,
        const QString &title,
        const QString &text,
        QMessageBox::StandardButtons buttons = QMessageBox::Ok,
        QMessageBox::StandardButton defaultButton = QMessageBox::NoButton
    );

    // QR Code
    void showQRCodeDialog(QWidget *parent, const QString &title,
                         const QString &data, const QString &label = QString());

    // Theme colors
    namespace Theme {
        extern const QString ColorBlue;
        extern const QString ColorGreen;
        extern const QString ColorRed;
        extern const QString ColorYellow;
        extern const QString ColorGray;
    }

    QString currentTheme();
    bool isDarkTheme(const QString& theme = QString());
    void applyApplicationTheme(QApplication* app, const QString& theme);
    void applyNativeWindowTheme(QWidget* widget, const QString& theme);
    QString windowStyleSheet(const QString& theme);
    QString dialogStyleSheet(const QString& theme);
    void applyDialogStyle(QWidget* widget, const QString& theme = QString());
    bool getStyledText(QWidget* parent,
                       const QString& title,
                       const QString& label,
                       QString& text,
                       int echoMode,
                       const QString& initialValue = QString());
    QString messageBoxStyleSheet(const QString& theme);
    QString spinBoxStyleSheet(const QString& theme);
    QString glassInputStyleSheet(const QString& theme);
    QString themedEditableStyleSheet(const QString& theme = QString());
    QString themedMonoEditableStyleSheet(const QString& theme = QString());
    QString themedSuccessEditableStyleSheet(const QString& theme = QString());
    QString glassPanelStyleSheet(const QString& theme);
    QString glassListStyleSheet(const QString& theme);
    QString warningLabelStyleSheet(const QString& theme);
}

#endif // GEOCOIN_QT_GUIUTIL_H
