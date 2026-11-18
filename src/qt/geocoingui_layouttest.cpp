#include "geocoingui.h"
#include "geocoinapplication.h"
#include "walletframe.h"
#include "walletmodel.h"
#include "overviewpage.h"
#include "nftpage.h"
#include "optionsmodel.h"
#include "intro.h"
#include "miningpage.h"

#include <QAbstractButton>
#include <QAbstractScrollArea>
#include <QAbstractSpinBox>
#include <QApplication>
#include <QButtonGroup>
#include <QDir>
#include <QDateTime>
#include <QElapsedTimer>
#include <QDebug>
#include <QDialog>
#include <QFile>
#include <QFileDialog>
#include <QFrame>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLayout>
#include <QLineEdit>
#include <QScrollArea>
#include <QScrollBar>
#include <QStackedWidget>
#include <QStatusBar>
#include <QToolBar>
#include <QToolButton>
#include <QTimer>
#include <QTabWidget>
#include <QThread>

bool GeoCoinGUI::runLayoutChecks(QString *error)
{
    auto* app = qobject_cast<GeoCoinApplication*>(qApp);
    if (!app || !walletFrame || !walletFrame->hasRequiredPages()) {
        if (error) *error = QStringLiteral("Layout test requires the complete wallet UI");
        return false;
    }
    const QString output = qEnvironmentVariable("GEOCOIN_UI_TEST_OUTPUT",
        QDir::current().filePath(QStringLiteral("out/ui-layout-test/%1-%2")
            .arg(QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd-HHmmss-zzz")))
            .arg(QCoreApplication::applicationPid())));
    if (!QDir().mkpath(output)) {
        if (error) *error = QStringLiteral("Cannot create layout artifact directory: ") + output;
        return false;
    }
    qInfo().noquote() << "GeoCoin UI layout artifacts:" << output;
    QJsonArray failures;
    QJsonArray cases;
    auto record = [&](const QString& context, const QString& message) {
        failures.append(QJsonObject{{QStringLiteral("case"), context},
                                    {QStringLiteral("error"), message}});
    };
    auto settle = []() {
        for (int i = 0; i < 3; ++i) {
            QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
        }
    };
    {
        Intro intro;
        const QString selected = QDir(output).filePath(QStringLiteral("chosen folder"));
        intro.setDataDirectory(selected);
        if (intro.getDataDirectory() != selected)
            record(QStringLiteral("intro"), QStringLiteral("Chosen directory was changed"));
#ifdef Q_OS_LINUX
        QDir().mkpath(selected);
        intro.show();
        bool selectionQueued = false;
        bool timedOut = false;
        int heartbeats = 0;
        auto ownedDialog = [&intro](QObject* dialog) {
            for (QObject* parent = dialog->parent(); parent; parent = parent->parent())
                if (parent == &intro) return true;
            return false;
        };
        QTimer picker;
        picker.setInterval(10);
        connect(&picker, &QTimer::timeout, &intro, [&] {
            ++heartbeats;
            if (selectionQueued) return;
            for (QWidget* widget : QApplication::topLevelWidgets()) {
                auto* dialog = qobject_cast<QFileDialog*>(widget);
                if (!dialog || !dialog->isVisible() || !ownedDialog(dialog)) continue;
                selectionQueued = true;
                dialog->setDirectory(selected);
                dialog->selectFile(selected);
                QTimer::singleShot(200, dialog, [dialog] {
                    QMetaObject::invokeMethod(dialog, "accept", Qt::QueuedConnection);
                });
                break;
            }
        });
        QTimer watchdog;
        watchdog.setSingleShot(true);
        connect(&watchdog, &QTimer::timeout, &intro, [&] {
            timedOut = true;
            for (QWidget* widget : QApplication::topLevelWidgets()) {
                auto* dialog = qobject_cast<QFileDialog*>(widget);
                if (dialog && ownedDialog(dialog)) dialog->reject();
            }
            if (auto* dialog = qobject_cast<QFileDialog*>(QApplication::activeModalWidget())) dialog->reject();
        });
        picker.start();
        watchdog.start(5000);
        const bool invoked = QMetaObject::invokeMethod(&intro, "on_ellipsisButton_clicked", Qt::DirectConnection);
        picker.stop();
        watchdog.stop();
        intro.hide();
        if (!invoked || !selectionQueued || timedOut || heartbeats < 5 || intro.getDataDirectory() != selected)
            record(QStringLiteral("intro-picker"), QStringLiteral(
                "Directory picker failed: invoked=%1 selected=%2 timeout=%3 heartbeats=%4 exact_path=%5")
                .arg(invoked).arg(selectionQueued).arg(timedOut).arg(heartbeats)
                .arg(intro.getDataDirectory() == selected));
#endif
    }
    const QString minerFixture = qEnvironmentVariable("GEOCOIN_TEST_GPU_MINER");
    if (!minerFixture.isEmpty()) {
        auto* mining = walletFrame->findChild<MiningPage*>();
        QString detail;
        if (!mining || !mining->runProcessLifecycleChecks(minerFixture, &detail))
            record(QStringLiteral("mining-process"), detail);
    }
    auto inspect = [&](QWidget* content, const QString& context) {
        for (QWidget* widget : content->findChildren<QWidget*>()) {
            if (!widget->isVisibleTo(content) || widget->isWindow()) continue;
            bool internalViewport = false;
            for (QWidget* ancestor = widget->parentWidget(); ancestor && ancestor != content;
                 ancestor = ancestor->parentWidget()) {
                if (qobject_cast<QAbstractScrollArea*>(ancestor)) internalViewport = true;
            }
            if (!internalViewport) {
                const QRect bounds(widget->mapTo(content, QPoint()), widget->size());
                if (bounds.left() < -1 || bounds.top() < -1 ||
                    bounds.right() >= content->width() + 1 || bounds.bottom() >= content->height() + 1) {
                    record(context, QStringLiteral("Widget exceeds scrollable content: %1/%2 [%3,%4 %5x%6] in %7x%8")
                        .arg(QString::fromLatin1(widget->metaObject()->className()), widget->objectName())
                        .arg(bounds.x()).arg(bounds.y()).arg(bounds.width()).arg(bounds.height())
                        .arg(content->width()).arg(content->height()));
                }
            }
            if (auto* label = qobject_cast<QLabel*>(widget)) {
                if (label->text().isEmpty()) continue;
                const int needed = label->wordWrap()
                    ? label->heightForWidth(label->width()) : label->minimumSizeHint().height();
                if (needed > 0 && label->height() + 1 < needed) {
                    record(context, QStringLiteral("Label clipped vertically: %1 (%2 < %3)")
                        .arg(label->text().left(100)).arg(label->height()).arg(needed));
                }
            } else if (qobject_cast<QAbstractButton*>(widget) || qobject_cast<QLineEdit*>(widget)) {
                if (qobject_cast<QAbstractSpinBox*>(widget->parentWidget())) continue;
                const QSize needed = widget->minimumSizeHint();
                if (widget->height() + 1 < needed.height() || widget->width() + 1 < needed.width()) {
                    record(context, QStringLiteral("Control clipped: %1/%2 %3x%4 < %5x%6")
                        .arg(QString::fromLatin1(widget->metaObject()->className()), widget->objectName())
                        .arg(widget->width()).arg(widget->height()).arg(needed.width()).arg(needed.height()));
                }
            }
        }
    };
    const QStringList pages = {QStringLiteral("overview"), QStringLiteral("send"),
        QStringLiteral("receive"), QStringLiteral("history"), QStringLiteral("mining"),
        QStringLiteral("multisig"), QStringLiteral("corporate"), QStringLiteral("nft")};
    const QList<QSize> sizes = {QSize(640, 480), QSize(800, 600), QSize(960, 640), QSize(1280, 800), QSize(640, 480)};
    OptionsModel options;
    showNormal();
    for (const QString& language : {QStringLiteral("en"), QStringLiteral("ru")}) {
        if (!app->setLanguage(language)) {
            record(language, QStringLiteral("Translation failed to load"));
            continue;
        }
        options.setLanguage(language);
        retranslateUi();
        walletFrame->rebuildForLanguageChange();
        for (const QString& theme : {QStringLiteral("light"), QStringLiteral("dark")}) {
            options.setTheme(theme);
            applyCurrentTheme();
            themeToggleButton->click();
            if (options.getTheme() == theme)
                record(language + QLatin1Char('-') + theme, QStringLiteral("Theme button did not update settings"));
            themeToggleButton->click();
            if (options.getTheme() != theme)
                record(language + QLatin1Char('-') + theme, QStringLiteral("Theme button did not restore theme"));
            for (const QSize& requested : sizes) {
                resize(requested);
                setNumConnections(128);
                setNumBlocks(123456789);
                updateMiningStatus(false, 0, 0.0, 0);
                setSyncProgress(330.0 / 575.0, 330, 575);
                settle();
#ifdef Q_OS_LINUX
                const QString borderContext = language + QLatin1Char('-') + theme;
                if (!clientAreaFrame_ || clientAreaFrame_->geometry() != rect() ||
                    !clientAreaFrame_->testAttribute(Qt::WA_TransparentForMouseEvents)) {
                    record(borderContext, QStringLiteral("Client border geometry or mouse passthrough is invalid"));
                } else {
                    const QImage border = clientAreaFrame_->grab().toImage();
                    const QColor expected(theme == QLatin1String("dark") ? QStringLiteral("#D6A84F") : QStringLiteral("#977E53"));
                    if (border.isNull() || border.pixelColor(border.width() / 2, 0) != expected)
                        record(borderContext, QStringLiteral("Client border did not render its theme color"));
                }
#endif
                for (const QString& pageId : pages) {
                    const QString context = QStringLiteral("%1-%2-%3x%4-%5")
                        .arg(language, theme).arg(requested.width()).arg(requested.height()).arg(pageId);
                    gotoPageById(pageId);
                    settle();
                    if (size() != requested) record(context, QStringLiteral("Window cannot resize to requested client size"));
                    auto* stack = walletFrame->findChild<QStackedWidget*>(QStringLiteral("walletStack"));
                    auto* scroll = stack ? qobject_cast<QScrollArea*>(stack->currentWidget()) : nullptr;
                    if (!scroll || !scroll->widget()) {
                        record(context, QStringLiteral("Active page has no complete scroll container"));
                        continue;
                    }
                    QWidget* content = scroll->widget();
                    inspect(content, context);
                    scroll->verticalScrollBar()->setValue(0);
                    scroll->horizontalScrollBar()->setValue(0);
                    settle();
                    if (!grab().save(QDir(output).filePath(context + QStringLiteral(".png"))))
                        record(context, QStringLiteral("Cannot save screenshot"));
                    scroll->verticalScrollBar()->setValue(scroll->verticalScrollBar()->maximum());
                    scroll->horizontalScrollBar()->setValue(scroll->horizontalScrollBar()->maximum());
                    settle();
                    for (auto* button : content->findChildren<QAbstractButton*>()) {
                        if (!button->isVisibleTo(content)) continue;
                        scroll->ensureWidgetVisible(button);
                        settle();
                        const QRect rect(button->mapTo(scroll->viewport(), QPoint()), button->size());
                        if (!rect.intersects(scroll->viewport()->rect()))
                            record(context, QStringLiteral("Action is unreachable by scrolling: ") + button->text());
                    }
                    const int lineHeight = labelConnectionsText->fontMetrics().height();
                    if (statusBar()->height() < lineHeight || labelConnectionsText->height() < lineHeight)
                        record(context, QStringLiteral("Status bar clips its text"));
#ifdef GEOCOIN_NATIVE_MACOS
                    // cocoa/offscreen render 10pt at 96/72 = 1.333x the xcb logical px,
                    // so the compact status string is elided in the middle ("GPU" -> "PU")
                    // at narrow widths. The canonical (un-elided) string is kept in the
                    // label tooltip's first line; check the tokens there and check the
                    // visible elided text only for fitting its own width.
                    const QString fullMiningRate = labelHashrateStatus->toolTip().isEmpty()
                        ? labelHashrateStatus->text()
                        : labelHashrateStatus->toolTip().section(QLatin1Char('\n'), 0, 0);
                    if (!fullMiningRate.contains(QStringLiteral("CPU")) ||
                        !fullMiningRate.contains(QStringLiteral("GPU")) ||
                        labelHashrateStatus->fontMetrics().horizontalAdvance(labelHashrateStatus->text()) > labelHashrateStatus->contentsRect().width() + 1)
                        record(context, QStringLiteral("Total/CPU/GPU mining rates are clipped: %1/%2 needs %3px has %4px text=%5 full=%6")
                            .arg(QString::fromLatin1(labelHashrateStatus->metaObject()->className()),
                                 labelHashrateStatus->objectName())
                            .arg(labelHashrateStatus->fontMetrics().horizontalAdvance(labelHashrateStatus->text()))
                            .arg(labelHashrateStatus->contentsRect().width())
                            .arg(labelHashrateStatus->text(), fullMiningRate));
#else
                    if (!labelHashrateStatus->text().contains(QStringLiteral("CPU")) ||
                        !labelHashrateStatus->text().contains(QStringLiteral("GPU")) ||
                        labelHashrateStatus->fontMetrics().horizontalAdvance(labelHashrateStatus->text()) > labelHashrateStatus->contentsRect().width())
                        record(context, QStringLiteral("Total/CPU/GPU mining rates are clipped"));
#endif
                    if (!themeToggleButton->isVisible() || !statusBar()->rect().contains(themeToggleButton->geometry()))
                        record(context, QStringLiteral("Theme button is inaccessible"));
                    auto* toolbar = findChild<QToolBar*>(QStringLiteral("tabsToolbar"));
                    if (!toolbar || toolbar->actions().size() != 8)
                        record(context, QStringLiteral("Navigation lost an action"));
                    if (toolbar) {
                        bool hasHiddenAction = false;
                        for (auto* action : toolbar->actions()) {
                            QWidget* button = toolbar->widgetForAction(action);
                            if (button && !button->isVisible()) hasHiddenAction = true;
                            if (button && button->isVisible() && button->width() + 1 < button->sizeHint().width())
                                record(context, QStringLiteral("Navigation text clipped: ") + action->text());
                        }
                        auto* overflow = toolbar->findChild<QToolButton*>(QStringLiteral("qt_toolbar_ext_button"));
                        if (hasHiddenAction && (!overflow || !overflow->isVisible()))
                            record(context, QStringLiteral("Hidden navigation actions have no overflow button"));
                        if (overflow && overflow->isVisible() && !toolbar->rect().contains(overflow->geometry()))
                            record(context, QStringLiteral("Navigation overflow button is clipped: %1,%2 %3x%4 in %5x%6")
                                .arg(overflow->x()).arg(overflow->y()).arg(overflow->width()).arg(overflow->height())
                                .arg(toolbar->width()).arg(toolbar->height()));
                    }
                    cases.append(QJsonObject{{QStringLiteral("case"), context},
                        {QStringLiteral("content_width"), content->width()},
                        {QStringLiteral("content_height"), content->height()},
                        {QStringLiteral("scroll_height"), scroll->verticalScrollBar()->maximum()}});
                    if (auto* roles = content->findChild<QButtonGroup*>(QStringLiteral("roleSelector"))) {
                        const auto buttons = roles->buttons();
                        for (int role = 0; role < buttons.size(); ++role) {
                            buttons[role]->click();
                            settle();
                            const QString roleCase = context + QStringLiteral("-role-%1").arg(role);
                            inspect(content, roleCase);
                            scroll->verticalScrollBar()->setValue(0);
                            if (!grab().save(QDir(output).filePath(roleCase + QStringLiteral(".png"))))
                                record(roleCase, QStringLiteral("Cannot save role screenshot"));
                        }
                        if (!buttons.isEmpty()) buttons.first()->click();
                    }
                    for (auto* tabs : content->findChildren<QTabWidget*>()) {
                        if (!tabs->isVisibleTo(content)) continue;
                        const int previous = tabs->currentIndex();
                        for (int tab = 0; tab < tabs->count(); ++tab) {
                            tabs->setCurrentIndex(tab);
                            settle();
                            const QString tabCase = context + QStringLiteral("-tab-%1").arg(tab);
                            inspect(content, tabCase);
                            scroll->verticalScrollBar()->setValue(0);
                            if (!grab().save(QDir(output).filePath(tabCase + QStringLiteral(".png"))))
                                record(tabCase, QStringLiteral("Cannot save tab screenshot"));
                        }
                        tabs->setCurrentIndex(previous);
                    }
                }
            }
        }
    }

    WalletModel fixture;
    fixture.setDataDirectory(dataDirectory);
    const QString walletPath = QDir(dataDirectory).filePath(QStringLiteral("layout.wallet"));
    if (!fixture.createWallet(walletPath, QStringLiteral("layout-test-password"))) {
        record(QStringLiteral("wallet"), QStringLiteral("Cannot create isolated wallet fixture"));
    } else {
        // Manual long-value fixtures must not race the model's async refresh.
        fixture.beginQuiesce();
        QElapsedTimer drain;
        drain.start();
        while (!fixture.isQuiesced() && drain.elapsed() < 5000) {
            QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
            QThread::msleep(1);
        }
        if (!fixture.isQuiesced())
            record(QStringLiteral("wallet"), QStringLiteral("Cannot drain isolated wallet refresh"));
        WalletModel* original = walletModel;
        setWalletModel(&fixture);
        auto checkDialog = [&](const QString& name, const auto& open) {
            bool found = false;
            QTimer::singleShot(0, this, [&]() {
                for (QWidget* topLevel : QApplication::topLevelWidgets()) {
                    auto* dialog = qobject_cast<QDialog*>(topLevel);
                    if (!dialog || !dialog->isVisible()) continue;
                    found = true;
                    dialog->resize(640, 480);
                    settle();
                    if (dialog->width() > 640 || dialog->height() > 480)
                        record(name, QStringLiteral("Dialog cannot fit 640x480"));
                    inspect(dialog, name);
                    if (!dialog->grab().save(QDir(output).filePath(name + QStringLiteral(".png"))))
                        record(name, QStringLiteral("Cannot save dialog screenshot"));
                    dialog->reject();
                }
            });
            open();
            if (!found) record(name, QStringLiteral("Dialog did not open"));
        };
        for (const QString& language : {QStringLiteral("en"), QStringLiteral("ru")}) {
            app->setLanguage(language);
            options.setLanguage(language);
            retranslateUi();
            walletFrame->rebuildForLanguageChange();
            settle();
            for (const QString& theme : {QStringLiteral("light"), QStringLiteral("dark")}) {
                options.setTheme(theme);
                applyCurrentTheme();
                const QString context = language + QLatin1Char('-') + theme;
                gotoOverviewPage();
                if (auto* overview = walletFrame->findChild<OverviewPage*>()) {
                    overview->setBalance(123456789012345678LL, 42, 100000000);
                    overview->setWatchOnlyBalance(123456789012345678LL, 42, 100000000);
                    resize(640, 480);
                    settle();
                    inspect(overview, context + QStringLiteral("-loaded-wallet-watch-only"));
                    if (!grab().save(QDir(output).filePath(context + QStringLiteral("-loaded-wallet-watch-only.png"))))
                        record(context, QStringLiteral("Cannot save wallet screenshot"));
                } else {
                    record(context, QStringLiteral("Overview page missing"));
                }
                checkDialog(context + QStringLiteral("-options-dialog"), [&]() { optionsClicked(); });
                gotoNftPage();
                if (auto* nft = walletFrame->findChild<NFTPage*>()) {
                    checkDialog(context + QStringLiteral("-create-nft-dialog"), [&]() {
                        if (!QMetaObject::invokeMethod(nft, "createNftClicked", Qt::DirectConnection))
                            record(context, QStringLiteral("Cannot invoke creation dialog"));
                    });
                }
            }
        }
        setWalletModel(original);
    }
    QFile report(QDir(output).filePath(QStringLiteral("layout-results.json")));
    if (!report.open(QIODevice::WriteOnly)) {
        if (error) *error = QStringLiteral("Cannot save layout results");
        return false;
    }
    report.write(QJsonDocument(QJsonObject{{QStringLiteral("cases"), cases},
        {QStringLiteral("failures"), failures},
        {QStringLiteral("scale"), qEnvironmentVariable("QT_SCALE_FACTOR", "1")},
        {QStringLiteral("device_pixel_ratio"), devicePixelRatioF()},
        {QStringLiteral("platform"), QGuiApplication::platformName()}}).toJson());
    if (!failures.isEmpty()) {
        if (error) *error = QStringLiteral("%1 layout failures; see %2. First: %3")
            .arg(failures.size()).arg(report.fileName(),
                failures.first().toObject().value(QStringLiteral("error")).toString());
        return false;
    }
    qInfo("GeoCoin UI layout test passed");
    return true;
}
