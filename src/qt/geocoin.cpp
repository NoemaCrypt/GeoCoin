// Copyright (c) 2011-2014 The Bitcoin developers
// Copyright (c) 2025 The GeoCoin developers
// Distributed under the MIT software license

#include "geocoinapplication.h"
#include "networkstyle.h"
#include "intro.h"
#include "guiutil.h"
#include "geocoin/wallet/wallet.hpp"

#include <QDebug>
#include <QPalette>
#include <QColor>
#include <QMessageBox>
#include <QInputDialog>
#include <QLineEdit>
#include <QFileDialog>
#include <QLockFile>
#include <QDir>
#include <QTemporaryDir>
#include <QFile>
#include <QFileInfo>
#include <QStyle>
#include <QStyleFactory>
#include <QAbstractItemView>
#include <QAbstractScrollArea>
#include <QEvent>
#include <QPointer>
#include <QTextStream>
#include <QDateTime>
#include <QTimer>
#include <QCoreApplication>
#include <QIcon>
#include <QProcessEnvironment>
#include <memory>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

#ifdef GEOCOIN_QT_STATIC_PLUGINS
#include <QtPlugin>
Q_IMPORT_PLUGIN(QWindowsIntegrationPlugin)
Q_IMPORT_PLUGIN(QOffscreenIntegrationPlugin)
Q_IMPORT_PLUGIN(QICOPlugin)
#endif

// Global pointer for signal handler
static GeoCoinApplication* g_app = nullptr;

namespace {
QPalette createGeoCoinLightPalette()
{
    QPalette palette;
    const QColor window("#F5F5DC");
    const QColor base("#FFFFFF");
    const QColor button("#E0E0E0");
    const QColor text("#000000");
    const QColor disabledText("#606060");
    const QColor highlight("#4A90E2");

    for (QPalette::ColorGroup group : {QPalette::Active, QPalette::Inactive, QPalette::Disabled}) {
        const QColor groupText = (group == QPalette::Disabled) ? disabledText : text;
        palette.setColor(group, QPalette::Window, window);
        palette.setColor(group, QPalette::WindowText, groupText);
        palette.setColor(group, QPalette::Base, base);
        palette.setColor(group, QPalette::AlternateBase, window);
        palette.setColor(group, QPalette::Text, groupText);
        palette.setColor(group, QPalette::Button, button);
        palette.setColor(group, QPalette::ButtonText, groupText);
        palette.setColor(group, QPalette::BrightText, QColor("#FFFFFF"));
        palette.setColor(group, QPalette::Highlight, highlight);
        palette.setColor(group, QPalette::HighlightedText, QColor("#FFFFFF"));
        palette.setColor(group, QPalette::Link, highlight);
        palette.setColor(group, QPalette::LinkVisited, QColor("#9B59B6"));
    }

    return palette;
}

#ifndef _WIN32
QString geoCoinLightFileDialogStyleSheet()
{
    return QStringLiteral(
        "QFileDialog, QFileDialog * { background-color: #F5F5DC; color: #000000; }"
        "QFileDialog QFrame, QFileDialog QSplitter, QFileDialog QStackedWidget, QFileDialog QScrollArea, QFileDialog QWidget { background-color: #F5F5DC; color: #000000; }"
        "QFileDialog QSplitter::handle { background-color: #D8D2C5; }"
        "QFileDialog QAbstractScrollArea { background-color: #FFFFFF; color: #000000; border: 1px solid #999999; }"
        "QFileDialog QAbstractItemView, QFileDialog QListView, QFileDialog QTreeView, QFileDialog QTableView { background-color: #FFFFFF; color: #000000; alternate-background-color: #FFFFFF; selection-background-color: #4A90E2; selection-color: #FFFFFF; }"
        "QFileDialog QAbstractItemView::item, QFileDialog QListView::item, QFileDialog QTreeView::item, QFileDialog QTableView::item { background-color: #FFFFFF; color: #000000; }"
        "QFileDialog QAbstractItemView::item:selected, QFileDialog QListView::item:selected, QFileDialog QTreeView::item:selected, QFileDialog QTableView::item:selected { background-color: #4A90E2; color: #FFFFFF; }"
        "QFileDialog QHeaderView, QFileDialog QHeaderView::section { background-color: #E0E0E0; color: #000000; border: 1px solid #999999; padding: 4px; }"
        "QFileDialog QLineEdit, QFileDialog QComboBox { background-color: #FFFFFF; color: #000000; border: 1px solid #999999; padding: 3px; border-radius: 3px; }"
        "QFileDialog QComboBox QAbstractItemView { background-color: #FFFFFF; color: #000000; selection-background-color: #4A90E2; selection-color: #FFFFFF; }"
        "QFileDialog QPushButton, QFileDialog QToolButton { background-color: #E0E0E0; color: #000000; border: 1px solid #999999; padding: 3px 8px; border-radius: 3px; }"
        "QFileDialog QPushButton:hover, QFileDialog QToolButton:hover { background-color: #D0D0D0; }"
        "QFileDialog QPushButton:pressed, QFileDialog QToolButton:pressed { background-color: #C0C0C0; }"
        "QFileDialog QDialogButtonBox { background-color: #F5F5DC; }"
        "QFileDialog QScrollBar:vertical { background-color: #F0F0F0; width: 12px; }"
        "QFileDialog QScrollBar::handle:vertical { background-color: #C0C0C0; border-radius: 6px; }"
        "QFileDialog QScrollBar:horizontal { background-color: #F0F0F0; height: 12px; }"
        "QFileDialog QScrollBar::handle:horizontal { background-color: #C0C0C0; border-radius: 6px; }"
    );
}

void applyLightPalette(QWidget* widget, const QPalette& palette)
{
    if (!widget) {
        return;
    }
    widget->setPalette(palette);
    widget->setAutoFillBackground(true);
    widget->setAttribute(Qt::WA_StyledBackground, true);
}

void applyLightFileDialogTheme(QWidget* root)
{
    if (!root) {
        return;
    }

    const QPalette lightPalette = createGeoCoinLightPalette();
    root->setStyleSheet(geoCoinLightFileDialogStyleSheet());
    applyLightPalette(root, lightPalette);

    const QList<QWidget*> widgets = root->findChildren<QWidget*>();
    for (QWidget* widget : widgets) {
        applyLightPalette(widget, lightPalette);
    }

    const QList<QAbstractScrollArea*> scrollAreas = root->findChildren<QAbstractScrollArea*>();
    for (QAbstractScrollArea* area : scrollAreas) {
        applyLightPalette(area, lightPalette);
        if (QWidget* viewport = area->viewport()) {
            applyLightPalette(viewport, lightPalette);
            viewport->setStyleSheet(QStringLiteral("background-color: #FFFFFF; color: #000000;"));
        }
    }

    const QList<QAbstractItemView*> itemViews = root->findChildren<QAbstractItemView*>();
    for (QAbstractItemView* view : itemViews) {
        applyLightPalette(view, lightPalette);
        view->setStyleSheet(QStringLiteral(
            "QAbstractItemView { background-color: #FFFFFF; color: #000000; alternate-background-color: #FFFFFF; selection-background-color: #4A90E2; selection-color: #FFFFFF; }"
            "QAbstractItemView::item { background-color: #FFFFFF; color: #000000; }"
            "QAbstractItemView::item:selected { background-color: #4A90E2; color: #FFFFFF; }"
        ));
        if (QWidget* viewport = view->viewport()) {
            applyLightPalette(viewport, lightPalette);
            viewport->setStyleSheet(QStringLiteral("background-color: #FFFFFF; color: #000000;"));
        }
    }
}

void applyToolTipTheme(QWidget* tooltip)
{
    if (!tooltip) {
        return;
    }

    const bool dark = GUIUtil::isDarkTheme();
    const QColor background(dark ? QStringLiteral("#1A202B") : QStringLiteral("#FFF8E8"));
    const QColor foreground(dark ? QStringLiteral("#F8EBD0") : QStringLiteral("#17110A"));
    const QColor border(dark ? QStringLiteral("#D6A84F") : QStringLiteral("#977E53"));
    QPalette palette = tooltip->palette();
    palette.setColor(QPalette::Window, background);
    palette.setColor(QPalette::WindowText, foreground);
    palette.setColor(QPalette::ToolTipBase, background);
    palette.setColor(QPalette::ToolTipText, foreground);
    tooltip->setPalette(palette);
    tooltip->setStyleSheet(QStringLiteral(
        "background-color: %1; color: %2; border: 1px solid %3; padding: 5px; border-radius: 6px;")
        .arg(background.name(), foreground.name(), border.name()));
    tooltip->setAttribute(Qt::WA_StyledBackground, true);
}

class LightFileDialogThemeFilter final : public QObject
{
public:
    explicit LightFileDialogThemeFilter(QObject* parent = nullptr)
        : QObject(parent)
    {
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (!event || (event->type() != QEvent::Show &&
                       event->type() != QEvent::Polish &&
                       event->type() != QEvent::ChildAdded)) {
            return QObject::eventFilter(watched, event);
        }

        QWidget* widget = qobject_cast<QWidget*>(watched);
        if (!widget) {
            return QObject::eventFilter(watched, event);
        }

        QWidget* window = widget->window();
        if (!window) {
            return QObject::eventFilter(watched, event);
        }

        if (window->windowType() == Qt::ToolTip) {
            applyToolTipTheme(window);
            return QObject::eventFilter(watched, event);
        }

        if (!qobject_cast<QFileDialog*>(window)) {
            return QObject::eventFilter(watched, event);
        }

        if (window->property("geocoinLightFileDialogScheduled").toBool()) {
            return QObject::eventFilter(watched, event);
        }

        window->setProperty("geocoinLightFileDialogScheduled", true);
        QPointer<QWidget> dialog(window);
        QTimer::singleShot(0, window, [dialog]() {
            if (!dialog) {
                return;
            }
            dialog->setProperty("geocoinLightFileDialogScheduled", false);
            applyLightFileDialogTheme(dialog);
        });

        return QObject::eventFilter(watched, event);
    }
};
#endif
}

static bool argvHasSmokeTest(int argc, char *argv[])
{
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--smoke-test") {
            return true;
        }
    }
    return false;
}

static void writeEarlySmokeTrace(bool enabled, const char* message)
{
    if (!enabled || std::getenv("GEOCOIN_QT_SMOKE_TRACE") == nullptr) {
        return;
    }

    const char* temp = std::getenv("TEMP");
    if (!temp || !*temp) {
        temp = std::getenv("TMP");
    }
    if (!temp || !*temp) {
        return;
    }

    std::ofstream out(std::string(temp) + "\\geocoin-core-smoke.log", std::ios::app);
    if (out) {
        out << "early " << message << '\n';
    }
}

static void writeSmokeTrace(bool enabled, const QString& message)
{
    if (!enabled || !qEnvironmentVariableIsSet("GEOCOIN_QT_SMOKE_TRACE")) {
        return;
    }

    QFile traceFile(QDir::temp().filePath(QStringLiteral("geocoin-core-smoke.log")));
    if (!traceFile.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        return;
    }

    QTextStream out(&traceFile);
    out << QDateTime::currentDateTimeUtc().toString(Qt::ISODate)
        << " pid=" << QCoreApplication::applicationPid()
        << " " << message << '\n';
}

static bool requestStartupWalletPassword(const QString& walletPath, QString& password)
{
    password.clear();
    if (!QFileInfo::exists(walletPath) && !QFileInfo::exists(walletPath + QStringLiteral(".bak"))) {
        return true;
    }

    // Unprotected wallets open without interrupting startup.  A failed empty
    // password probe means either encryption or damage; the real load in the
    // core thread remains authoritative.
    {
        geocoin::wallet::Wallet probe;
        if (probe.load_from_file(walletPath.toStdString(), std::string())) {
            return true;
        }
    }

    QString candidate = qEnvironmentVariable("GEOCOIN_QT_WALLET_PASSWORD");
    GUIUtil::ScopedQStringCleanser candidateCleanser(candidate);
    bool triedEnvironment = !candidate.isEmpty();

    while (true) {
        if (candidate.isEmpty()) {
            QInputDialog dialog;
            dialog.setWindowTitle(QObject::tr("Unlock Wallet"));
            dialog.setLabelText(QObject::tr(
                "wallet.dat is password protected. Enter its passphrase to continue:"));
            dialog.setTextEchoMode(QLineEdit::Password);
            dialog.setStyleSheet(GUIUtil::dialogStyleSheet(GUIUtil::currentTheme()));
            if (dialog.exec() != QDialog::Accepted) {
                return false;
            }
            candidate = dialog.textValue();
        }

        std::string candidateBytes = candidate.toStdString();
        GUIUtil::ScopedStringCleanser candidateBytesCleanser(candidateBytes);
        geocoin::wallet::Wallet probe;
        const bool unlocked = probe.load_from_file(walletPath.toStdString(), candidateBytes);
        GUIUtil::cleanseString(candidateBytes);
        if (unlocked) {
            password = candidate;
            GUIUtil::cleanseQString(candidate);
            return true;
        }

        GUIUtil::cleanseQString(candidate);
        if (triedEnvironment) {
            triedEnvironment = false;
        }
        GUIUtil::showStyledMessageBox(
            nullptr,
            QMessageBox::Warning,
            QObject::tr("Unlock Wallet"),
            QObject::tr("The passphrase is incorrect, or wallet.dat is damaged."));
    }
}

// Signal handler for Ctrl+C (SIGINT) and SIGTERM
static void handleShutdownSignal(int sig)
{
    if (g_app) {
        // Request shutdown asynchronously from signal handler using lambda
        QMetaObject::invokeMethod(g_app, [=]() {
            g_app->requestShutdown();
        }, Qt::QueuedConnection);
    }
}

int main(int argc, char *argv[])
{
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
#endif

    const bool earlySmokeTest = argvHasSmokeTest(argc, argv);
    writeEarlySmokeTrace(earlySmokeTest, "before QApplication");

#ifndef _WIN32
    // Linux fallback keeps bundled dialogs independent from the host OS theme.
    QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
#endif

    // Создаем приложение и запускаем основной GUI-цикл.
    GeoCoinApplication app(argc, argv);
    writeEarlySmokeTrace(earlySmokeTest, "after QApplication");
    g_app = &app;
#ifndef _WIN32
    app.installEventFilter(new LightFileDialogThemeFilter(&app));
#endif
    bool smokeTest = false;
    bool guiE2eTest = false;
    QString explicitDataDir;

    // Setup signal handlers for graceful shutdown
    std::signal(SIGINT, handleShutdownSignal);   // Ctrl+C
    std::signal(SIGTERM, handleShutdownSignal);  // kill command

    // Parse command line arguments
    QStringList args = app.arguments();
    for (int i = 1; i < args.size(); i++) {
        QString arg = args[i];

        // Handle -addnode=<ip:port>
        if (arg.startsWith("-addnode=")) {
            QString node = arg.mid(9); // Skip "-addnode="
            app.addNode(node);
        }
        // Handle -addnode <ip:port> (two arguments)
        else if (arg == "-addnode" && i + 1 < args.size()) {
            QString node = args[++i];
            app.addNode(node);
        }

        // Handle -connect=<ip:port>
        else if (arg.startsWith("-connect=")) {
            QString node = arg.mid(9);
            app.addConnectPeer(node);
        }
        // Handle -connect <ip:port>
        else if (arg == "-connect" && i + 1 < args.size()) {
            QString node = args[++i];
            app.addConnectPeer(node);
        }

        // Handle -no-auto-connect
        else if (arg == "-no-auto-connect") {
            app.setNoAutoConnect(true);
        }
        else if (arg == "--smoke-test") {
            smokeTest = true;
            app.setNoAutoConnect(true);
        }
        else if (arg == "--gui-e2e-test") {
            guiE2eTest = true;
        }
        else if (arg.startsWith("--data-dir=")) {
            explicitDataDir = arg.mid(QStringLiteral("--data-dir=").size());
        }
        else if (arg == "--data-dir" && i + 1 < args.size()) {
            explicitDataDir = args[++i];
        }
        else if (arg.startsWith("--mining-sha256=")) {
            app.setMiningSha256Backend(arg.mid(QStringLiteral("--mining-sha256=").size()));
        }
        else if (arg == "--mining-sha256" && i + 1 < args.size()) {
            app.setMiningSha256Backend(args[++i]);
        }
    }
    const QString envMiningSha256 = QProcessEnvironment::systemEnvironment()
        .value(QStringLiteral("GEOCOIN_MINING_SHA256"));
    if (!envMiningSha256.isEmpty()) {
        app.setMiningSha256Backend(envMiningSha256);
    }

    // Application identification
    QApplication::setOrganizationName("GeoCoin");
    QApplication::setOrganizationDomain("geocoin.org");
    QApplication::setApplicationName("GeoCoin Core");
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/icons/geocoin")));

    if (QStyle* fusionStyle = QStyleFactory::create(QStringLiteral("Fusion"))) {
        app.setStyle(fusionStyle);
    }

    // Apply UI language from settings before creating windows
    app.applyLanguageFromSettings();

    GUIUtil::applyApplicationTheme(&app, GUIUtil::currentTheme());

    app.setSmokeTestMode(smokeTest);
    app.setGuiE2eTestMode(guiE2eTest);

    QTemporaryDir smokeTempDir;
    QString dataDir;
    if (smokeTest || guiE2eTest) {
        if (!explicitDataDir.isEmpty()) {
            dataDir = explicitDataDir;
            QDir().mkpath(dataDir);
        } else {
            smokeTempDir.setAutoRemove(true);
            if (!smokeTempDir.isValid()) {
                qCritical() << "Failed to create temporary data directory for GUI smoke test";
                return 1;
            }
            dataDir = smokeTempDir.path();
        }
    } else if (!explicitDataDir.isEmpty()) {
        QDir dir(explicitDataDir);
        if (!dir.exists() && !dir.mkpath(".")) {
            QMessageBox::critical(
                nullptr,
                QObject::tr("GeoCoin"),
                QObject::tr("Error: Cannot create data directory:\n%1").arg(explicitDataDir)
            );
            return 1;
        }
        dataDir = explicitDataDir;
    } else {
        // Pick data directory (shows dialog on first run)
        dataDir = Intro::pickDataDirectory();
    }
    if (dataDir.isEmpty()) {
        return 0;
    }

    QLockFile instanceLock(dataDir + "/geocoin-core.lock");
    instanceLock.setStaleLockTime(0);
    if (!instanceLock.tryLock(0)) {
        QMessageBox::critical(
            nullptr,
            QObject::tr("GeoCoin"),
            QObject::tr("GeoCoin Core is already running.")
        );
        return 0;
    }

    QString startupWalletPassword;
    GUIUtil::ScopedQStringCleanser startupWalletPasswordCleanser(startupWalletPassword);
    const QString startupWalletPath = QDir(dataDir).filePath(QStringLiteral("wallet.dat"));
    if (!smokeTest && !requestStartupWalletPassword(startupWalletPath, startupWalletPassword)) {
        return 0;
    }
    app.setStartupWalletPassword(startupWalletPassword);
    GUIUtil::cleanseQString(startupWalletPassword);

    app.setDataDirectory(dataDir);

    // Create network style
    std::unique_ptr<NetworkStyle> networkStyle(new NetworkStyle("GeoCoin", 0, 0, ""));

    try {
        // Create main window (no splash screen - sync shown in status bar)
        writeSmokeTrace(smokeTest, QStringLiteral("before createWindow"));
        app.createWindow(networkStyle.get());
        writeSmokeTrace(smokeTest, QStringLiteral("after createWindow"));

        if (smokeTest) {
            QTimer::singleShot(20000, &app, [&app]() {
                if (app.isSmokeTestMode()) {
                    app.exit(3);
                }
            });
            writeSmokeTrace(smokeTest, QStringLiteral("before requestGuiSmokeTest"));
            app.requestGuiSmokeTest();
            writeSmokeTrace(smokeTest, QStringLiteral("after requestGuiSmokeTest"));
        } else {
            // Request initialization (starts background thread)
            app.requestInitialize();
        }

        // Run event loop (will exit when quit() is called after shutdown completes)
        writeSmokeTrace(smokeTest, QStringLiteral("before app.exec"));
        const int eventLoopResult = app.exec();
        writeSmokeTrace(smokeTest, QStringLiteral("after app.exec result=%1").arg(eventLoopResult));
        return eventLoopResult == 0 ? app.getReturnValue() : eventLoopResult;

    } catch (const std::exception& e) {
        app.handleRunawayException(QString::fromStdString(e.what()));
        return 1;
    }
}
