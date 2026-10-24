// Copyright (c) 2011-2014 The Bitcoin developers
// Copyright (c) 2025 The GeoCoin developers
// Distributed under the MIT software license

#include "guiutil.h"
#include "geocoin/utils/secret_file.hpp"

#include <QApplication>
#include <QClipboard>
#include <QFile>
#include <QFileDialog>
#include <QMessageBox>
#include <QInputDialog>
#include <QLineEdit>
#include <QSettings>
#include <QFont>
#include <QFontDatabase>
#include <QDateTime>
#include <QDialog>
#include <QVBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QImage>
#include <QPainter>
#include <QPalette>
#include <QPixmap>
#include <QSaveFile>
#include <QThread>
#include <QColor>
#include <QToolTip>
#include <QWidget>

#include <algorithm>
#include <filesystem>
#include <limits>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#ifdef USE_QRCODE
#include <qrencode.h>
#endif

namespace GUIUtil {

void cleanseQString(QString& value)
{
    if (value.isEmpty()) {
        return;
    }
    value.detach();
    value.fill(QChar(0));
    value.clear();
}

void cleanseString(std::string& value)
{
    geocoin::utils::cleanse_container(value);
}

bool readPrivateKeyFileHex(
    const QString& path,
    QString& privateKeyHex,
    QString& error
) {
    privateKeyHex.clear();
    error.clear();
    if (path.trimmed().isEmpty()) {
        error = QObject::tr("Private key file path is empty");
        return false;
    }

    std::string secret;
    ScopedStringCleanser secretCleanser(secret);
    try {
        secret = geocoin::utils::read_secret_file(
            std::filesystem::path(path.toUtf8().constData()),
            "private key",
            96
        );
    } catch (const std::exception& ex) {
        error = QObject::tr("Cannot read private key file safely: %1")
            .arg(QString::fromUtf8(ex.what()));
        return false;
    }

    QString candidate = QString::fromUtf8(secret.data(), static_cast<int>(secret.size()));
    ScopedQStringCleanser candidateCleanser(candidate);
    if (candidate.startsWith(QStringLiteral("Private Key"), Qt::CaseInsensitive)) {
        const qsizetype separator = candidate.indexOf(QLatin1Char(':'));
        if (separator < 0) {
            error = QObject::tr("Private key file has an invalid label");
            return false;
        }
        candidate = candidate.mid(separator + 1).trimmed();
    }

    if (candidate.size() != 64) {
        error = QObject::tr("Private key file must contain exactly one 32-byte hex key");
        return false;
    }
    for (const QChar ch : candidate) {
        const bool isHex = (ch >= QLatin1Char('0') && ch <= QLatin1Char('9')) ||
            (ch >= QLatin1Char('a') && ch <= QLatin1Char('f')) ||
            (ch >= QLatin1Char('A') && ch <= QLatin1Char('F'));
        if (!isHex) {
            error = QObject::tr("Private key file contains non-hex characters");
            return false;
        }
    }

    privateKeyHex = candidate;
    return true;
}

bool writeFileAtomically(
    const QString& path,
    const QByteArray& data,
    QString& error
) {
    error.clear();
    if (path.trimmed().isEmpty()) {
        error = QObject::tr("File path is empty");
        return false;
    }

    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly)) {
        error = file.errorString();
        return false;
    }

    const auto ownerOnly = QFileDevice::ReadOwner | QFileDevice::WriteOwner;
#ifndef Q_OS_WIN
    if (!file.setPermissions(ownerOnly)) {
        error = QObject::tr("Cannot restrict file permissions: %1").arg(file.errorString());
        file.cancelWriting();
        return false;
    }
#else
    // Qt maps Windows permissions to the read-only attribute rather than a
    // complete ACL.  The wallet itself applies native ACLs to secret files.
    (void)file.setPermissions(ownerOnly);
#endif

    if (file.write(data) != data.size()) {
        error = file.errorString();
        file.cancelWriting();
        return false;
    }
    if (!file.commit()) {
        error = file.errorString();
        return false;
    }
    return true;
}

// Theme colors
namespace Theme {
    const QString ColorBlue = "#6680EB";
    const QString ColorGreen = "#4CAF50";
    const QString ColorRed = "#F44336";
    const QString ColorYellow = "#FFC107";
    const QString ColorGray = "#9E9E9E";
}

QString currentTheme()
{
    QSettings settings;
    const QString theme = settings.value(QStringLiteral("ui/theme"), QStringLiteral("light")).toString().trimmed().toLower();
    return theme == QStringLiteral("dark") ? QStringLiteral("dark") : QStringLiteral("light");
}

bool isDarkTheme(const QString& theme)
{
    const QString effective = theme.isEmpty() ? currentTheme() : theme.trimmed().toLower();
    return effective == QStringLiteral("dark");
}

namespace {
struct GlassColors {
    QString page;
    QString pageAlt;
    QString text;
    QString mutedText;
    QString panel;
    QString panelTop;
    QString panelBottom;
    QString field;
    QString fieldFocus;
    QString border;
    QString borderLight;
    QString borderShadow;
    QString button;
    QString buttonHover;
    QString selection;
    QString tabActive;
    QString tabInactive;
};

GlassColors glassColors(const QString& theme)
{
    if (isDarkTheme(theme)) {
        return {
            QStringLiteral("#080B10"),
            QStringLiteral("#111722"),
            QStringLiteral("#F8EBD0"),
            QStringLiteral("#B8C0CC"),
            QStringLiteral("rgba(7, 10, 15, 196)"),
            QStringLiteral("rgba(28, 34, 43, 172)"),
            QStringLiteral("rgba(0, 0, 0, 210)"),
            QStringLiteral("rgba(8, 12, 18, 224)"),
            QStringLiteral("rgba(18, 26, 38, 238)"),
            QStringLiteral("rgba(214, 168, 79, 166)"),
            QStringLiteral("rgba(255, 243, 205, 180)"),
            QStringLiteral("rgba(0, 0, 0, 160)"),
            QStringLiteral("rgba(26, 32, 43, 216)"),
            QStringLiteral("rgba(46, 57, 75, 236)"),
            QStringLiteral("#4A90E2"),
            QStringLiteral("rgba(20, 27, 38, 210)"),
            QStringLiteral("rgba(12, 17, 26, 196)")
        };
    }

    return {
        QStringLiteral("#F7E8D5"),
        QStringLiteral("#F0DDC2"),
        QStringLiteral("#17110A"),
        QStringLiteral("#5D625F"),
        QStringLiteral("rgba(255, 255, 255, 82)"),
        QStringLiteral("rgba(255, 255, 255, 188)"),
        QStringLiteral("rgba(214, 168, 79, 50)"),
        QStringLiteral("rgba(255, 255, 255, 166)"),
        QStringLiteral("rgba(255, 252, 242, 218)"),
        QStringLiteral("rgba(151, 126, 83, 156)"),
        QStringLiteral("rgba(255, 255, 255, 210)"),
        QStringLiteral("rgba(115, 83, 35, 65)"),
        QStringLiteral("rgba(255, 248, 232, 128)"),
        QStringLiteral("rgba(255, 239, 198, 178)"),
        QStringLiteral("#6680EB"),
        QStringLiteral("rgba(255, 250, 238, 170)"),
        QStringLiteral("rgba(255, 248, 232, 104)")
    };
}

QString glassBackground(const GlassColors& c)
{
    return QStringLiteral(
        "qlineargradient(x1:0, y1:0, x2:1, y2:1,"
        " stop:0 %1,"
        " stop:0.18 %2,"
        " stop:0.55 %3,"
        " stop:1 %4)"
    ).arg(c.panelTop, c.panel, c.panel, c.panelBottom);
}

QString glassScrollBarStyle(const GlassColors& c)
{
    return QStringLiteral(
        "QScrollBar:vertical { background: transparent; width: 14px; margin: 2px; }"
        "QScrollBar::handle:vertical { background: %1; border: 1px solid %2; border-radius: 6px; min-height: 28px; }"
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0px; background: transparent; }"
        "QScrollBar:horizontal { background: transparent; height: 14px; margin: 2px; }"
        "QScrollBar::handle:horizontal { background: %1; border: 1px solid %2; border-radius: 6px; min-width: 28px; }"
        "QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0px; background: transparent; }"
    ).arg(c.buttonHover, c.border);
}
}

QString glassPanelStyleSheet(const QString& theme)
{
    const GlassColors c = glassColors(theme);
    const QString glass = glassBackground(c);
    const QString titleBg = isDarkTheme(theme)
        ? QStringLiteral("rgba(9, 13, 19, 235)")
        : QStringLiteral("rgba(255, 252, 242, 235)");
    return QStringLiteral(
        "QGroupBox, QFrame#glassPanel {"
        "  background: %1;"
        "  color: %2;"
        "  border: 1px solid %3;"
        "  border-top-color: %4;"
        "  border-left-color: %4;"
        "  border-right-color: %5;"
        "  border-bottom-color: %5;"
        "  border-radius: 12px;"
        "  margin-top: 26px;"
        "  padding: 28px 14px 14px 14px;"
        "}"
        "QGroupBox::title {"
        "  subcontrol-origin: margin;"
        "  subcontrol-position: top left;"
        "  left: 16px;"
        "  top: 1px;"
        "  padding: 3px 12px;"
        "  color: %2;"
        "  background-color: %6;"
        "  border: 1px solid %3;"
        "  border-radius: 8px;"
        "}"
    ).arg(glass, c.text, c.border, c.borderLight, c.borderShadow, titleBg);
}

QString glassInputStyleSheet(const QString& theme)
{
    const GlassColors c = glassColors(theme);
    return QStringLiteral(
        "QLineEdit, QTextEdit, QPlainTextEdit, QComboBox, QDateTimeEdit {"
        "  background-color: %1;"
        "  color: %2;"
        "  selection-background-color: %3;"
        "  selection-color: #FFFFFF;"
        "  border: 1px solid %4;"
        "  border-top-color: %5;"
        "  border-left-color: %5;"
        "  border-radius: 8px;"
        "  padding: 6px 8px;"
        "}"
        "QLineEdit:focus, QTextEdit:focus, QPlainTextEdit:focus, QComboBox:focus, QDateTimeEdit:focus {"
        "  background-color: %6;"
        "  border: 1px solid %7;"
        "}"
        "QComboBox::drop-down, QDateTimeEdit::drop-down {"
        "  width: 24px;"
        "  border-left: 1px solid %4;"
        "  background-color: %8;"
        "  border-top-right-radius: 8px;"
        "  border-bottom-right-radius: 8px;"
        "}"
        "QComboBox QAbstractItemView {"
        "  background-color: %9;"
        "  color: %2;"
        "  border: 1px solid %4;"
        "  selection-background-color: %3;"
        "  selection-color: #FFFFFF;"
        "}"
    ).arg(c.field, c.text, c.selection, c.border, c.borderLight, c.fieldFocus, c.borderLight, c.button, c.pageAlt);
}

QString glassListStyleSheet(const QString& theme)
{
    const GlassColors c = glassColors(theme);
    return QStringLiteral(
        "QTableWidget, QTableView, QTreeView, QListView, QListWidget {"
        "  background: %1;"
        "  color: %2;"
        "  gridline-color: %3;"
        "  border: 1px solid %3;"
        "  border-top-color: %4;"
        "  border-left-color: %4;"
        "  border-radius: 10px;"
        "  padding: 5px;"
        "  alternate-background-color: %5;"
        "}"
        "QTableWidget::item, QTableView::item, QTreeView::item, QListView::item, QListWidget::item {"
        "  background-color: transparent;"
        "  color: %2;"
        "  padding: 4px 6px;"
        "}"
        "QTableWidget::item:selected, QTableView::item:selected, QTreeView::item:selected, QListView::item:selected, QListWidget::item:selected {"
        "  background-color: rgba(74, 144, 226, 145);"
        "  color: #FFFFFF;"
        "}"
        "QHeaderView::section {"
        "  background: %6;"
        "  color: %2;"
        "  padding: 6px;"
        "  border: 1px solid %3;"
        "  font-weight: 700;"
        "}"
    ).arg(c.panel, c.text, c.border, c.borderLight, c.panelBottom, c.button);
}

QString spinBoxStyleSheet(const QString& theme)
{
    const GlassColors c = glassColors(theme);
    const bool dark = isDarkTheme(theme);
    const QString upArrow = dark
        ? QStringLiteral("url(:/icons/spin_up_dark)")
        : QStringLiteral("url(:/icons/spin_up_light)");
    const QString downArrow = dark
        ? QStringLiteral("url(:/icons/spin_down_dark)")
        : QStringLiteral("url(:/icons/spin_down_light)");
    return QStringLiteral(
        "QSpinBox, QDoubleSpinBox {"
        "  min-width: 120px;"
        "  min-height: 32px;"
        "  font-size: 12pt;"
        "  font-weight: 700;"
        "  padding: 4px 28px 4px 10px;"
        "  border: 1px solid %1;"
        "  border-top-color: %2;"
        "  border-left-color: %2;"
        "  border-radius: 8px;"
        "  background-color: %3;"
        "  color: %4;"
        "}"
        "QSpinBox::up-button, QSpinBox::down-button,"
        "QDoubleSpinBox::up-button, QDoubleSpinBox::down-button {"
        "  width: 24px;"
        "  border-left: 1px solid %1;"
        "  background-color: %5;"
        "  subcontrol-origin: border;"
        "}"
        "QSpinBox::up-button, QDoubleSpinBox::up-button {"
        "  subcontrol-position: top right;"
        "  border-top-right-radius: 8px;"
        "}"
        "QSpinBox::down-button, QDoubleSpinBox::down-button {"
        "  subcontrol-position: bottom right;"
        "  border-bottom-right-radius: 8px;"
        "}"
        "QSpinBox::up-button:hover, QSpinBox::down-button:hover,"
        "QDoubleSpinBox::up-button:hover, QDoubleSpinBox::down-button:hover {"
        "  background-color: %6;"
        "}"
        "QSpinBox::up-arrow, QDoubleSpinBox::up-arrow {"
        "  image: %7;"
        "  width: 11px;"
        "  height: 8px;"
        "}"
        "QSpinBox::down-arrow, QDoubleSpinBox::down-arrow {"
        "  image: %8;"
        "  width: 11px;"
        "  height: 8px;"
        "}"
    ).arg(c.border, c.borderLight, c.field, c.text, c.button, c.buttonHover, upArrow, downArrow);
}

QString warningLabelStyleSheet(const QString& theme)
{
    const bool dark = isDarkTheme(theme);
    return QStringLiteral(
        "QLabel {"
        "  color: %1;"
        "  font-weight: 800;"
        "  background-color: %2;"
        "  border: 1px solid %3;"
        "  border-radius: 5px;"
        "  padding: 8px;"
        "}"
    ).arg(dark ? QStringLiteral("#FF766D") : QStringLiteral("#B71C1C"),
          dark ? QStringLiteral("rgba(90, 20, 20, 120)") : QStringLiteral("rgba(255, 235, 238, 180)"),
          dark ? QStringLiteral("#C7443E") : QStringLiteral("#EF9A9A"));
}

QString dialogStyleSheet(const QString& theme)
{
    const GlassColors c = glassColors(theme);
    return QStringLiteral(
        "QDialog, QMessageBox, QInputDialog { background-color: %1; color: %2; }"
        "QDialog *, QMessageBox *, QInputDialog * { color: %2; }"
        "QLabel { color: %2; background-color: transparent; }"
        "QPushButton { background-color: %3; color: %2; border: 1px solid %4; padding: 7px 18px; border-radius: 8px; min-width: 80px; font-weight: 700; }"
        "QPushButton:hover { background-color: %5; }"
    ).arg(c.pageAlt, c.text, c.button, c.border, c.buttonHover)
        + glassPanelStyleSheet(theme)
        + glassInputStyleSheet(theme)
        + glassListStyleSheet(theme)
        + spinBoxStyleSheet(theme)
        + glassScrollBarStyle(c);
}

void applyDialogStyle(QWidget* widget, const QString& theme)
{
    if (!widget) {
        return;
    }
    const QString effectiveTheme = theme.isEmpty() ? currentTheme() : theme;
    widget->setStyleSheet(dialogStyleSheet(effectiveTheme));
}

bool getStyledText(QWidget* parent,
                   const QString& title,
                   const QString& label,
                   QString& text,
                   int echoMode,
                   const QString& initialValue)
{
    QInputDialog dialog(parent);
    dialog.setWindowTitle(title);
    dialog.setLabelText(label);
    dialog.setTextEchoMode(static_cast<QLineEdit::EchoMode>(echoMode));
    dialog.setTextValue(initialValue);
    applyDialogStyle(&dialog);
    if (dialog.exec() != QDialog::Accepted) {
        return false;
    }
    text = dialog.textValue();
    return true;
}

QString messageBoxStyleSheet(const QString& theme)
{
    return dialogStyleSheet(theme);
}

QString themedEditableStyleSheet(const QString& theme)
{
    const QString effectiveTheme = theme.isEmpty() ? currentTheme() : theme;
    return glassInputStyleSheet(effectiveTheme);
}

QString themedMonoEditableStyleSheet(const QString& theme)
{
    return themedEditableStyleSheet(theme)
        + QStringLiteral("QLineEdit, QTextEdit, QPlainTextEdit { font-family: monospace; }");
}

QString themedSuccessEditableStyleSheet(const QString& theme)
{
    const QString effectiveTheme = theme.isEmpty() ? currentTheme() : theme;
    const bool dark = isDarkTheme(effectiveTheme);
    return themedEditableStyleSheet(effectiveTheme)
        + QStringLiteral("QLineEdit { border: 1px solid %1; border-top-color: %2; border-left-color: %2; }")
              .arg(dark ? QStringLiteral("#36D987") : QStringLiteral("#27AE60"),
                   dark ? QStringLiteral("#7FF0B2") : QStringLiteral("#5BD98E"));
}

QString windowStyleSheet(const QString& theme)
{
    const GlassColors c = glassColors(theme);
    const bool dark = isDarkTheme(theme);
    const QString bgImage = dark ? QStringLiteral(":/images/background_dark") : QStringLiteral(":/images/background");
    return QStringLiteral(
        "GeoCoinGUI {"
        "  background-image: url(%1);"
        "  background-position: center;"
        "  background-repeat: no-repeat;"
        "  background-color: %2;"
        "}"
        "QMenuBar, QToolBar, QStatusBar { background-color: %2; color: %3; border-color: %4; }"
        "QMenuBar::item { background-color: transparent; color: %3; padding: 5px 10px; font-weight: bold; }"
        "QMenuBar::item:selected { background-color: %5; border-radius: 6px; }"
        "QToolButton, QPushButton { background-color: %5; color: %3; border: 1px solid %4; border-top-color: %6; border-left-color: %6; border-radius: 8px; padding: 7px 16px; font-weight: bold; }"
        "QToolButton:hover, QPushButton:hover { background-color: %7; }"
        "QLabel, QCheckBox, QRadioButton { color: %3; background-color: transparent; font-weight: bold; }"
        "QToolTip { background-color: %5; color: %3; border: 1px solid %4; padding: 5px; border-radius: 6px; }"
    ).arg(bgImage, c.page, c.text, c.border, c.button, c.borderLight, c.buttonHover)
        + glassPanelStyleSheet(theme)
        + glassInputStyleSheet(theme)
        + glassListStyleSheet(theme)
        + spinBoxStyleSheet(theme)
        + glassScrollBarStyle(c);
}

void applyApplicationTheme(QApplication* app, const QString& theme)
{
    if (!app) {
        return;
    }
    const GlassColors c = glassColors(theme);
    const bool dark = isDarkTheme(theme);
    QPalette palette;
    palette.setColor(QPalette::Window, QColor(c.page));
    palette.setColor(QPalette::WindowText, QColor(c.text));
    palette.setColor(QPalette::Base, dark ? QColor("#121722") : QColor("#FFF9EC"));
    palette.setColor(QPalette::AlternateBase, QColor(c.pageAlt));
    palette.setColor(QPalette::Text, QColor(c.text));
    palette.setColor(QPalette::Button, QColor(dark ? "#202633" : "#EFE4CF"));
    palette.setColor(QPalette::ButtonText, QColor(c.text));
    palette.setColor(QPalette::Highlight, QColor("#4A90E2"));
    palette.setColor(QPalette::HighlightedText, QColor("#FFFFFF"));
    palette.setColor(QPalette::ToolTipBase,
                     dark ? QColor("#1A202B") : QColor("#FFF8E8"));
    palette.setColor(QPalette::ToolTipText, QColor(c.text));
    app->setPalette(palette);
    QToolTip::setPalette(palette);

    app->setStyleSheet(QStringLiteral(
        "* { color: %1; }"
        "QWidget { background-color: transparent; color: %1; }"
        "QDialog, QMessageBox, QInputDialog { background-color: %2; color: %1; }"
        "QLabel { background-color: transparent; color: %1; }"
        "QPushButton { background-color: %3; color: %1; border: 1px solid %4; border-top-color: %5; border-left-color: %5; padding: 7px 18px; border-radius: 8px; font-weight: 700; }"
        "QPushButton:hover { background-color: %6; }"
        "QToolTip { background-color: %3; color: %1; border: 1px solid %4; padding: 5px; border-radius: 6px; }"
        "QMenu { background-color: %2; color: %1; border: 1px solid %4; padding: 3px; }"
        "QMenu::item { background-color: transparent; color: %1; padding: 5px 22px; }"
        "QMenu::item:selected { background-color: %7; color: #FFFFFF; border-radius: 5px; }"
        "QTabWidget::pane { background: transparent; border: 1px solid %4; border-radius: 10px; }"
        "QTabBar::tab { background-color: %8; color: %1; padding: 7px 16px; border: 1px solid %4; border-top-left-radius: 8px; border-top-right-radius: 8px; font-weight: 700; }"
        "QTabBar::tab:selected { background-color: %9; border-top-color: %5; }"
    ).arg(c.text, c.pageAlt, c.button, c.border, c.borderLight, c.buttonHover, c.selection, c.tabInactive, c.tabActive)
        + glassPanelStyleSheet(theme)
        + glassInputStyleSheet(theme)
        + glassListStyleSheet(theme)
        + spinBoxStyleSheet(theme)
        + glassScrollBarStyle(c));
}

void applyNativeWindowTheme(QWidget* widget, const QString& theme)
{
    if (!widget) {
        return;
    }

#ifdef _WIN32
    using DwmSetWindowAttributeFn = HRESULT (WINAPI *)(HWND, DWORD, LPCVOID, DWORD);

    HMODULE dwmapi = LoadLibraryW(L"dwmapi.dll");
    if (!dwmapi) {
        return;
    }

    auto* setWindowAttribute = reinterpret_cast<DwmSetWindowAttributeFn>(
        GetProcAddress(dwmapi, "DwmSetWindowAttribute"));
    if (!setWindowAttribute) {
        FreeLibrary(dwmapi);
        return;
    }

    const bool dark = isDarkTheme(theme);
    const BOOL useDark = dark ? TRUE : FALSE;
    const HWND hwnd = reinterpret_cast<HWND>(widget->winId());

    // Windows 10/11 dark caption. Attribute 20 is current, 19 is the older
    // pre-20H1 value kept as a fallback for compatibility.
    constexpr DWORD DWMWA_USE_IMMERSIVE_DARK_MODE = 20;
    constexpr DWORD DWMWA_USE_IMMERSIVE_DARK_MODE_OLD = 19;
    setWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &useDark, sizeof(useDark));
    setWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE_OLD, &useDark, sizeof(useDark));

    // Windows 11 caption colors. Older Windows versions ignore these.
    constexpr DWORD DWMWA_BORDER_COLOR = 34;
    constexpr DWORD DWMWA_CAPTION_COLOR = 35;
    constexpr DWORD DWMWA_TEXT_COLOR = 36;
    const COLORREF borderColor = dark ? RGB(214, 168, 79) : RGB(214, 168, 79);
    const COLORREF captionColor = dark ? RGB(10, 14, 20) : RGB(242, 226, 201);
    const COLORREF textColor = dark ? RGB(247, 232, 201) : RGB(20, 20, 20);
    setWindowAttribute(hwnd, DWMWA_BORDER_COLOR, &borderColor, sizeof(borderColor));
    setWindowAttribute(hwnd, DWMWA_CAPTION_COLOR, &captionColor, sizeof(captionColor));
    setWindowAttribute(hwnd, DWMWA_TEXT_COLOR, &textColor, sizeof(textColor));

    FreeLibrary(dwmapi);
#else
    Q_UNUSED(theme);
#endif
}

bool parseAmount(const QString& text, qint64& amount)
{
    amount = 0;
    const QString value = text.trimmed();
    if (value.isEmpty() || value.size() > 32) return false;

    const int decimalPoint = value.indexOf(QLatin1Char('.'));
    if (decimalPoint != value.lastIndexOf(QLatin1Char('.'))) return false;

    const QString wholeText = decimalPoint < 0 ? value : value.left(decimalPoint);
    const QString fractionText = decimalPoint < 0 ? QString() : value.mid(decimalPoint + 1);
    if (wholeText.isEmpty() || (decimalPoint >= 0 && fractionText.isEmpty()) ||
        fractionText.size() > 8) {
        return false;
    }

    constexpr quint64 kAnglesPerCoin = 100000000ULL;
    constexpr quint64 kMaxAmount = static_cast<quint64>(std::numeric_limits<qint64>::max());
    constexpr quint64 kMaxWhole = kMaxAmount / kAnglesPerCoin;

    quint64 whole = 0;
    for (const QChar ch : wholeText) {
        if (ch < QLatin1Char('0') || ch > QLatin1Char('9')) return false;
        const quint64 digit = static_cast<quint64>(ch.unicode() - QLatin1Char('0').unicode());
        if (whole > (kMaxWhole - digit) / 10ULL) return false;
        whole = whole * 10ULL + digit;
    }

    quint64 fraction = 0;
    for (const QChar ch : fractionText) {
        if (ch < QLatin1Char('0') || ch > QLatin1Char('9')) return false;
        fraction = fraction * 10ULL +
            static_cast<quint64>(ch.unicode() - QLatin1Char('0').unicode());
    }
    for (int i = fractionText.size(); i < 8; ++i) fraction *= 10ULL;

    if (whole == kMaxWhole && fraction > kMaxAmount % kAnglesPerCoin) return false;
    amount = static_cast<qint64>(whole * kAnglesPerCoin + fraction);
    return true;
}

bool addAmounts(qint64 lhs, qint64 rhs, qint64& result)
{
    if ((rhs > 0 && lhs > std::numeric_limits<qint64>::max() - rhs) ||
        (rhs < 0 && lhs < std::numeric_limits<qint64>::min() - rhs)) {
        return false;
    }
    result = lhs + rhs;
    return true;
}

QString formatAmount(qint64 amount, int unit)
{
    Q_UNUSED(unit);
    constexpr quint64 kAnglesPerCoin = 100000000ULL;
    const bool negative = amount < 0;
    const quint64 magnitude = negative
        ? static_cast<quint64>(-(amount + 1)) + 1ULL
        : static_cast<quint64>(amount);
    const quint64 whole = magnitude / kAnglesPerCoin;
    const quint64 fraction = magnitude % kAnglesPerCoin;
    return (negative ? QStringLiteral("-") : QString()) +
        QString::number(whole) + QLatin1Char('.') +
        QString::number(fraction).rightJustified(8, QLatin1Char('0'));
}

QString formatAmountWithUnit(qint64 amount, int unit)
{
    return formatAmount(amount, unit) + " GC";
}

QString formatAddress(const QString &address)
{
    // Format: GEOxxx...xxx (show first 8 and last 8 chars)
    if (address.length() > 20) {
        return address.left(12) + "..." + address.right(8);
    }
    return address;
}

QString formatHashrate(double hashrate)
{
    if (hashrate < 1000.0) {
        return QString::number(hashrate, 'f', 2) + " H/s";
    } else if (hashrate < 1000000.0) {
        return QString::number(hashrate / 1000.0, 'f', 2) + " KH/s";
    } else if (hashrate < 1000000000.0) {
        return QString::number(hashrate / 1000000.0, 'f', 2) + " MH/s";
    } else {
        return QString::number(hashrate / 1000000000.0, 'f', 2) + " GH/s";
    }
}

int availableCpuThreads()
{
#ifdef _WIN32
    DWORD total = 0;
    const WORD groups = GetActiveProcessorGroupCount();
    for (WORD group = 0; group < groups; ++group) {
        total += GetActiveProcessorCount(group);
    }
    if (total > 0) {
        return static_cast<int>(std::min<DWORD>(
            total,
            static_cast<DWORD>(std::numeric_limits<int>::max())
        ));
    }
#endif
    return std::max(1, QThread::idealThreadCount());
}

QString formatDuration(int secs)
{
    if (secs < 60) {
        return QString::number(secs) + " sec";
    } else if (secs < 3600) {
        return QString::number(secs / 60) + " min";
    } else if (secs < 86400) {
        return QString::number(secs / 3600) + " hours";
    } else {
        return QString::number(secs / 86400) + " days";
    }
}

QString formatDateTime(qint64 timestamp)
{
    QDateTime dt = QDateTime::fromSecsSinceEpoch(timestamp);
    return dt.toString("yyyy-MM-dd hh:mm:ss");
}

QFont geocoinAddressFont()
{
    QFont font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    font.setPointSize(font.pointSize() - 1);
    return font;
}

QFont fixedPitchFont()
{
    return QFontDatabase::systemFont(QFontDatabase::FixedFont);
}

void setClipboard(const QString &str)
{
    QApplication::clipboard()->setText(str, QClipboard::Clipboard);
    QApplication::clipboard()->setText(str, QClipboard::Selection);
}

QString getSaveFileName(QWidget *parent, const QString &caption,
                       const QString &dir, const QString &filter,
                       QString *selectedFilter)
{
    return QFileDialog::getSaveFileName(parent, caption, dir, filter, selectedFilter);
}

QString getOpenFileName(QWidget *parent, const QString &caption,
                       const QString &dir, const QString &filter,
                       QString *selectedFilter)
{
    return QFileDialog::getOpenFileName(parent, caption, dir, filter, selectedFilter);
}

void showMessageBox(const QString &title, const QString &text, unsigned int style)
{
    (void)style;
    showStyledMessageBox(nullptr, QMessageBox::Information, title, text);
}

QMessageBox::StandardButton showStyledMessageBox(
    QWidget *parent,
    QMessageBox::Icon icon,
    const QString &title,
    const QString &text,
    QMessageBox::StandardButtons buttons,
    QMessageBox::StandardButton defaultButton
) {
    QMessageBox msgBox(parent);
    msgBox.setWindowTitle(title);
    msgBox.setText(text);
    msgBox.setIcon(icon);
    msgBox.setStandardButtons(buttons);
    if (defaultButton != QMessageBox::NoButton) {
        msgBox.setDefaultButton(defaultButton);
    }
    msgBox.setStyleSheet(messageBoxStyleSheet(currentTheme()));
    const QByteArray autoClosePath = qgetenv("GEOCOIN_GUI_TEST_AUTOCLOSE");
    if (!autoClosePath.isEmpty()) {
        QFile record(QString::fromUtf8(autoClosePath));
        if (record.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
            record.write(title.toUtf8());
            record.write("\n");
            record.write(text.toUtf8());
            record.write("\n---\n");
        }
        return defaultButton == QMessageBox::NoButton ? QMessageBox::Ok : defaultButton;
    }
    return static_cast<QMessageBox::StandardButton>(msgBox.exec());
}

void showQRCodeDialog(QWidget *parent, const QString &title,
                     const QString &data, const QString &label)
{
#ifdef USE_QRCODE
    // Создаем диалог
    QDialog dialog(parent);
    dialog.setWindowTitle(title);
    dialog.setMinimumSize(400, 500);

    QVBoxLayout *layout = new QVBoxLayout(&dialog);
    layout->setSpacing(15);
    layout->setContentsMargins(20, 20, 20, 20);

    // Заголовок
    if (!label.isEmpty()) {
        QLabel *labelWidget = new QLabel(label, &dialog);
        labelWidget->setWordWrap(true);
        labelWidget->setStyleSheet("QLabel { font-size: 12pt; font-weight: bold; }");
        layout->addWidget(labelWidget);
    }

    // Генерируем QR-код
    QRcode *code = QRcode_encodeString(data.toUtf8().constData(), 0, QR_ECLEVEL_L, QR_MODE_8, 1);

    if (!code) {
        QLabel *errorLabel = new QLabel(QObject::tr("QR code generation failed"), &dialog);
        errorLabel->setAlignment(Qt::AlignCenter);
        layout->addWidget(errorLabel);
    } else {
        // Создаем изображение QR-кода
        int qrSize = code->width;
        int qrImageSize = 300;
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

        // Показываем QR-код
        QLabel *qrLabel = new QLabel(&dialog);
        qrLabel->setPixmap(QPixmap::fromImage(qrImage));
        qrLabel->setAlignment(Qt::AlignCenter);
        qrLabel->setStyleSheet("QLabel { border: 2px solid #ccc; padding: 10px; background: white; }");
        layout->addWidget(qrLabel);
    }

    // Текстовое поле с данными
    QLabel *dataLabel = new QLabel(data, &dialog);
    dataLabel->setWordWrap(true);
    dataLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    dataLabel->setStyleSheet("QLabel { font-family: monospace; font-size: 9pt; background: #f5f5f5; padding: 10px; border: 1px solid #ddd; }");
    layout->addWidget(dataLabel);

    // Кнопка копирования
    QPushButton *copyButton = new QPushButton(QObject::tr("📋 Copy to clipboard"), &dialog);
    QObject::connect(copyButton, &QPushButton::clicked, [data]() {
        QApplication::clipboard()->setText(data);
    });
    layout->addWidget(copyButton);

    // Кнопка закрытия
    QPushButton *closeButton = new QPushButton(QObject::tr("Close"), &dialog);
    QObject::connect(closeButton, &QPushButton::clicked, &dialog, &QDialog::accept);
    layout->addWidget(closeButton);

    dialog.exec();
#else
    // Если qrencode не доступен
    showStyledMessageBox(
        parent,
        QMessageBox::Information,
        title,
        QObject::tr("QR code unavailable\n\nlibqrencode is not installed.\n\nData:\n%1\n\nCopy the text manually.").arg(data)
    );
#endif
}

} // namespace GUIUtil
