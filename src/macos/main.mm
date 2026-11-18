// AppKit frontend for the same Core lifecycle used by the Qt desktop targets.
#import <AppKit/AppKit.h>
#import "GeoCoinMainWindowController.h"
#import "GeoCoinTheme.h"

#include "GeoCoinNativeFrontend.h"
#include "geocoinapplication.h"
#include "geocoingui.h"
#include "guiutil.h"
#include "intro.h"
#include "walletframe.h"

#include <QAbstractButton>
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QEvent>
#include <QEventLoop>
#include <QFrame>
#include <QGroupBox>
#include <QLineEdit>
#include <QListWidget>
#include <QPointer>
#include <QProcess>
#include <QScrollArea>
#include <QSettings>
#include <QStackedWidget>
#include <QStyle>
#include <QTabWidget>
#include <QTableView>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QtWidgets/QMacNativeWidget>
#include <utility>

class GeoCoinWorkflowGlass final : public QObject {
public:
    GeoCoinWorkflowGlass(QWidget& host, NSView* overlay)
        : host_(host), overlay_(overlay), clips_([NSMutableArray array]), panels_([NSMutableArray array])
    {
        timer_.setSingleShot(true);
        QObject::connect(&timer_, &QTimer::timeout, this, [this] { update(); });
    }

    void showPage(QScrollArea* area)
    {
        for (const auto& widget : std::as_const(observed_)) {
            if (widget) widget->removeEventFilter(this);
        }
        observed_.clear();
        area_ = area;
        if (area_) observeLayout();
        overlay_.hidden = !area_;
        refresh();
    }

    void refresh()
    {
        if (area_ && !timer_.isActive()) timer_.start(0);
    }

    bool hasVisibleGlass() const
    {
        return !overlay_.hidden && clips_.count > 0 && !clips_.firstObject.hidden;
    }

    void synchronize()
    {
        timer_.stop();
        update();
    }

private:
    static bool isPanel(const QWidget* view)
    {
        return qobject_cast<const QGroupBox*>(view) ||
            (qobject_cast<const QFrame*>(view) && view->objectName() == QLatin1String("glassPanel"));
    }

    void observeLayout()
    {
        const auto children = area_->findChildren<QWidget*>();
        observed_.reserve(children.size() + 1);
        observed_.append(area_.data());
        for (QWidget* child : children) observed_.append(child);
        for (const auto& widget : std::as_const(observed_)) widget->installEventFilter(this);
    }

    bool eventFilter(QObject* view, QEvent* event) override
    {
        switch (event->type()) {
        case QEvent::ChildPolished:
            rescan_ = true;
            Q_FALLTHROUGH();
        case QEvent::LayoutRequest:
        case QEvent::Move:
        case QEvent::Resize:
        case QEvent::Show:
        case QEvent::Hide:
            refresh();
            break;
        default:
            break;
        }
        return QObject::eventFilter(view, event);
    }

    struct PanelGeometry {
        QRect frame;
        QRect clip;
    };

    PanelGeometry panelGeometry(QWidget* view, int topInset = 0) const
    {
        PanelGeometry result;
        result.frame = QRect(view->mapTo(&host_, QPoint(0, 0)), view->size()).adjusted(0, topInset, 0, 0);
        result.clip = result.frame;
        for (QWidget* parent = view->parentWidget(); parent; parent = parent->parentWidget()) {
            result.clip &= QRect(parent->mapTo(&host_, QPoint(0, 0)), parent->size());
            if (result.clip.isEmpty() || parent == &host_) break;
        }
        return result;
    }

    NSRect appKitRect(const QRect& rect) const
    {
        return NSMakeRect(rect.x(), NSHeight(overlay_.bounds) - rect.y() - rect.height(),
                          rect.width(), rect.height());
    }

    void update()
    {
        if (!area_ || !area_->widget()) return;
        if (std::exchange(rescan_, false)) {
            for (const auto& widget : std::as_const(observed_)) {
                if (widget) widget->removeEventFilter(this);
            }
            observed_.clear();
            observeLayout();
        }

        QList<PanelGeometry> geometry;
        QWidget* page = area_->widget();
        for (QWidget* widget : page->findChildren<QWidget*>()) {
            if (!widget->isVisibleTo(&host_)) continue;
            if (auto* tabs = qobject_cast<QTabWidget*>(widget)) {
                if (QWidget* pane = tabs->currentWidget()) {
                    const auto shape = panelGeometry(pane);
                    if (!shape.clip.isEmpty()) geometry.append(shape);
                }
                continue;
            }
            if (!isPanel(widget)) continue;
            bool insidePanel = false;
            for (QWidget* parent = widget->parentWidget(); parent && parent != page;
                 parent = parent->parentWidget()) {
                if (isPanel(parent) || qobject_cast<QTabWidget*>(parent)) {
                    insidePanel = true;
                    break;
                }
            }
            if (insidePanel) continue;
            if (!widget->property("geoNativeGlass").toBool()) {
                widget->setProperty("geoNativeGlass", true);
                widget->style()->unpolish(widget);
                widget->style()->polish(widget);
            }
            const auto shape = panelGeometry(widget, qobject_cast<QGroupBox*>(widget) ? 26 : 0);
            if (!shape.clip.isEmpty()) geometry.append(shape);
        }
        if (geometry.isEmpty()) geometry.append(panelGeometry(page, 35));

        while (panels_.count < static_cast<NSUInteger>(geometry.size())) {
            NSView* clip = [NSView new];
            clip.wantsLayer = YES;
            clip.layer.masksToBounds = YES;
            GeoGlassPanel* pane = [[GeoGlassPanel alloc] initWithFrame:NSZeroRect];
            [clip addSubview:pane];
            [overlay_ addSubview:clip];
            [clips_ addObject:clip];
            [panels_ addObject:pane];
        }
        NSUInteger index = 0;
        for (const auto& shape : geometry) {
            NSView* clip = clips_[index];
            GeoGlassPanel* pane = panels_[index++];
            clip.hidden = shape.clip.isEmpty();
            if (clip.hidden) continue;
            clip.frame = appKitRect(shape.clip);
            pane.frame = NSOffsetRect(appKitRect(shape.frame), -NSMinX(clip.frame), -NSMinY(clip.frame));
        }
        while (index < clips_.count) clips_[index++].hidden = YES;
    }

    QWidget& host_;
    NSView* overlay_;
    NSMutableArray<NSView*>* clips_;
    NSMutableArray<GeoGlassPanel*>* panels_;
    QPointer<QScrollArea> area_;
    QList<QPointer<QWidget>> observed_;
    QTimer timer_;
    bool rescan_ = false;
};

static void showDirectoryError(NSString* path)
{
    NSAlert* alert = [NSAlert new];
    alert.alertStyle = NSAlertStyleCritical;
    alert.messageText = @"GeoCoin";
    alert.informativeText = [NSString stringWithFormat:@"Cannot create the data directory at %@.", path];
    [alert runModal];
}

QString geoCoinPickNativeDataDirectory()
{
    QSettings settings(QStringLiteral("GeoCoin"), QStringLiteral("GeoCoin-Qt"));
    const QString saved = settings.value(QStringLiteral("dataDir")).toString();
    if (!saved.isEmpty() && QDir(saved).exists()) return saved;

    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    [NSApp activateIgnoringOtherApps:YES];
    const QString defaultDirectory = Intro::getDefaultDataDirectory();
    NSAlert* welcome = [NSAlert new];
    welcome.messageText = @"Welcome to GeoCoin";
    welcome.informativeText = [NSString stringWithFormat:
        @"GeoCoin stores the blockchain on this Mac. It needs at least 10 GB of disk space.\n\n%@",
        defaultDirectory.toNSString()];
    [welcome addButtonWithTitle:@"Use Default"];
    [welcome addButtonWithTitle:@"Choose Folder…"];
    [welcome addButtonWithTitle:@"Cancel"];
    QString directory = defaultDirectory;
    switch ([welcome runModal]) {
    case NSAlertFirstButtonReturn:
        break;
    case NSAlertSecondButtonReturn: {
        NSOpenPanel* panel = [NSOpenPanel openPanel];
        panel.canChooseDirectories = YES;
        panel.canChooseFiles = NO;
        panel.canCreateDirectories = YES;
        if ([panel runModal] != NSModalResponseOK) return {};
        directory = QString::fromNSString(panel.URL.path);
        break;
    }
    default:
        return {};
    }
    if (!QDir().mkpath(directory)) {
        showDirectoryError(directory.toNSString());
        return {};
    }
    settings.setValue(QStringLiteral("dataDir"), directory);
    return directory;
}

struct GeoCoinNativeFrontend::Impl {
    GeoCoinCoreBridge* bridge;
    GeoCoinMainWindowController* shell;
    GeoCoinGUI* workflows = nullptr;
    std::unique_ptr<QMacNativeWidget> embeddedWorkflows;
    std::unique_ptr<GeoCoinWorkflowGlass> workflowGlass;
    NSView* hostedView;
    NSView* glassHost;
    id themeObserver;
    QMetaObject::Connection pagesRebuilt;

    ~Impl()
    {
        [bridge setWalletModel:nullptr];
        [bridge setClientModel:nullptr];
        [bridge setMiningController:nullptr];
        QObject::disconnect(pagesRebuilt);
        if (themeObserver) [[GeoThemeManager shared] removeThemeObserver:themeObserver];
        shell.onMenuAction = nil;
        shell.onPageChanged = nil;
        workflowGlass.reset();
        [glassHost removeFromSuperview];
        [hostedView removeFromSuperview];
        if (embeddedWorkflows) {
            auto* frame = workflows->nativeWalletFrame();
            embeddedWorkflows->layout()->removeWidget(frame);
            workflows->setCentralWidget(frame);
        }
        [shell.window orderOut:nil];
    }

    void showWorkflow(NSString* page)
    {
        WalletFrame* frame = workflows->nativeWalletFrame();
        if ([page isEqualToString:@"send"]) frame->gotoSendCoinsPage();
        else if ([page isEqualToString:@"receive"]) frame->gotoReceiveCoinsPage();
        else if ([page isEqualToString:@"history"]) frame->gotoHistoryPage();
        else if ([page isEqualToString:@"mining"]) frame->gotoMiningPage();
        else if ([page isEqualToString:@"multisig"]) frame->gotoMultisigPage();
        else if ([page isEqualToString:@"corporate"]) frame->gotoCorporatePage();
        else if ([page isEqualToString:@"nft"]) frame->gotoNftPage();
        else {
            if (workflowGlass) workflowGlass->showPage(nullptr);
            return;
        }

        NSView* container = [shell pageById:page];
        if (!embeddedWorkflows) {
            embeddedWorkflows = std::make_unique<QMacNativeWidget>();
            embeddedWorkflows->setAttribute(Qt::WA_TranslucentBackground);
            auto* layout = new QVBoxLayout(embeddedWorkflows.get());
            layout->setContentsMargins(0, 0, 0, 0);
            workflows->takeCentralWidget();
            layout->addWidget(frame);
            hostedView = (__bridge NSView*)reinterpret_cast<void*>(embeddedWorkflows->winId());
            hostedView.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
            glassHost = [NSView new];
            glassHost.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
            glassHost.wantsLayer = YES;
            glassHost.layer.masksToBounds = YES;
            workflowGlass = std::make_unique<GeoCoinWorkflowGlass>(*embeddedWorkflows, glassHost);
        }
        [glassHost removeFromSuperview];
        [hostedView removeFromSuperview];
        glassHost.frame = container.bounds;
        hostedView.frame = container.bounds;
        [container addSubview:glassHost];
        [container addSubview:hostedView];
        frame->show();
        embeddedWorkflows->show();
        workflowGlass->showPage(frame->nativeCurrentScrollArea());
    }
};

GeoCoinNativeFrontend::GeoCoinNativeFrontend(GeoCoinApplication& app, GeoCoinGUI& workflows,
                                             const QString& dataDirectory, const QString& network)
    : impl_(std::make_unique<Impl>())
{
    impl_->bridge = [GeoCoinCoreBridge new];
    impl_->bridge.dataDirectory = dataDirectory.toNSString();
    impl_->bridge.selectedNetwork = network.toNSString();
    impl_->bridge.testing = app.isSmokeTestMode() || app.isGuiE2eTestMode();
    GeoThemeManager* theme = [GeoThemeManager shared];
    if (!impl_->bridge.testing && ![[NSUserDefaults standardUserDefaults] objectForKey:@"ui/theme"] &&
        QSettings().contains(QStringLiteral("ui/theme"))) {
        theme.currentTheme = GUIUtil::currentTheme().toNSString();
    }
    impl_->shell = [[GeoCoinMainWindowController alloc] initWithCore:impl_->bridge];
    impl_->workflows = &workflows;
    GeoCoinMainWindowController* shell = impl_->shell;
    shell.titleAddText = network == QStringLiteral("degree") ? @"" : network.toNSString();

    GeoCoinApplication* application = &app;
    GeoCoinGUI* workflowWindow = &workflows;
    __weak GeoCoinMainWindowController* weakShell = shell;
    __weak GeoCoinCoreBridge* weakBridge = impl_->bridge;
    workflows.syncNativeTheme(QString::fromNSString(theme.currentTheme));
    impl_->themeObserver = [theme addThemeObserver:^(NSString* selected) {
        workflowWindow->syncNativeTheme(QString::fromNSString(selected));
    }];
    impl_->pagesRebuilt = QObject::connect(workflows.nativeWalletFrame(), &WalletFrame::nativePagesRebuilt,
        &workflows, [weakBridge, workflowWindow, frontend = impl_.get()]() {
            [weakBridge setMiningController:workflowWindow->nativeMiningController()];
            if (frontend->workflowGlass && !frontend->glassHost.enclosingScrollView.hidden) {
                frontend->workflowGlass->showPage(workflowWindow->nativeWalletFrame()->nativeCurrentScrollArea());
            }
        });
    [impl_->bridge observeGpuStatus:^(NSDictionary* status) {
        [weakShell updateGpuHashrate:[status[@"hashrate"] doubleValue]];
    }];
    Impl* frontend = impl_.get();
    shell.onPageChanged = ^(NSString* page) { frontend->showWorkflow(page); };
    shell.onMenuAction = ^(NSString* action) {
        if ([action isEqualToString:@"quit"]) {
            application->requestShutdown();
            return;
        }
        if (application->isShuttingDown()) return;
        if ([action hasPrefix:@"language-"]) {
            const QString language = QString::fromNSString([action substringFromIndex:9]);
            if (application->setLanguage(language)) {
                if (!application->isSmokeTestMode() && !application->isGuiE2eTestMode()) {
                    QSettings().setValue(QStringLiteral("ui/language"), language);
                }
                workflowWindow->refreshNativeLanguage();
                [weakShell applyLanguage];
            }
            return;
        }
        workflowWindow->invokeNativeMenuAction(QString::fromNSString(action));
        if ([action isEqualToString:@"options"] && !application->isShuttingDown()) {
            theme.currentTheme = GUIUtil::currentTheme().toNSString();
            [weakShell applyLanguage];
        }
    };

    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    [shell show];
    for (NSString* page in @[@"send", @"receive", @"history", @"mining", @"multisig", @"corporate", @"nft"]) {
        NSView* view = [shell pageById:page];
        if (view && !view.enclosingScrollView.hidden) impl_->showWorkflow(page);
    }
    [NSApp activateIgnoringOtherApps:YES];
}

GeoCoinNativeFrontend::~GeoCoinNativeFrontend() = default;

void GeoCoinNativeFrontend::setModels(ClientModel* client, WalletModel* wallet)
{
    [impl_->bridge setClientModel:client];
    [impl_->bridge setWalletModel:wallet];
    [impl_->bridge setMiningController:impl_->workflows->nativeMiningController()];
}

void GeoCoinNativeFrontend::setSyncProgress(double fraction, int current, int target)
{
    [impl_->bridge updateSyncProgress:fraction current:current target:target];
}

void GeoCoinNativeFrontend::beginShutdown()
{
    [impl_->shell.window orderOut:nil];
}

static bool hasQtWorkflowControls(QWidget* page, NSString* identifier)
{
    if ([identifier isEqualToString:@"send"]) return page->findChildren<QLineEdit*>().size() >= 3;
    if ([identifier isEqualToString:@"receive"]) return page->findChildren<QGroupBox*>().size() >= 2;
    if ([identifier isEqualToString:@"history"]) {
        return page->findChild<QTableView*>() && page->findChild<QComboBox*>() && page->findChild<QLineEdit*>();
    }
    if ([identifier isEqualToString:@"mining"]) {
        auto* modes = page->findChild<QTabWidget*>(QStringLiteral("miningModeTabs"));
        return modes && modes->count() == 2 && modes->widget(1)->findChild<QListWidget*>() &&
            modes->widget(1)->findChild<QCheckBox*>(QStringLiteral("gpuBackendAuto")) &&
            modes->widget(1)->findChild<QCheckBox*>(QStringLiteral("gpuBackendMetal")) &&
            modes->widget(1)->findChild<QCheckBox*>(QStringLiteral("gpuBackendOpenCL"));
    }
    if ([identifier isEqualToString:@"nft"]) return page->findChild<QTableWidget*>();
    auto* roles = page->findChild<QButtonGroup*>(QStringLiteral("roleSelector"));
    return roles && roles->buttons().size() == ([identifier isEqualToString:@"corporate"] ? 3 : 2);
}

static bool visitQtWorkflowModes(QWidget* page, NSString* identifier)
{
    if ([identifier isEqualToString:@"mining"]) {
        auto* modes = page->findChild<QTabWidget*>(QStringLiteral("miningModeTabs"));
        modes->setCurrentIndex(1);
        const bool gpuVisible = modes->widget(1)->isVisible();
        modes->setCurrentIndex(0);
        return gpuVisible && modes->widget(0)->isVisible();
    }
    auto* roles = page->findChild<QButtonGroup*>(QStringLiteral("roleSelector"));
    if (!roles) return true;
    QStackedWidget* roleContent = nullptr;
    for (QStackedWidget* stack : page->findChildren<QStackedWidget*>()) {
        if (stack->count() == roles->buttons().size()) {
            roleContent = stack;
            break;
        }
    }
    if (!roleContent) return false;
    for (QAbstractButton* button : roles->buttons()) {
        button->click();
        if (roleContent->currentIndex() != roles->id(button) || !roleContent->currentWidget()->isVisible()) {
            return false;
        }
    }
    roles->button(0)->click();
    return true;
}

static bool captureWindow(NSWindow* window, const QDir& directory, const QString& name)
{
    [window.contentView layoutSubtreeIfNeeded];
    [window displayIfNeeded];
    QEventLoop compositor;
    QTimer::singleShot(450, &compositor, &QEventLoop::quit);
    compositor.exec();
    return QProcess::execute(QStringLiteral("/usr/sbin/screencapture"),
        {QStringLiteral("-l"), QString::number(window.windowNumber),
         QStringLiteral("-x"), QStringLiteral("-o"), directory.filePath(name)}) == 0;
}

bool GeoCoinNativeFrontend::runSmokeChecks(bool layout, QString* error)
{
    NSWindow* window = impl_->shell.window;
    if (!window.isVisible || !window.contentView || !impl_->shell.isUsingMetal) {
        *error = QStringLiteral("The AppKit Core window or its Metal backdrop did not initialize");
        return false;
    }
    for (NSWindow* other in NSApp.windows) {
        if (other != window && other.isVisible && other.canBecomeMainWindow) {
            *error = QStringLiteral("A second Core window became visible");
            return false;
        }
    }
    const NSArray<NSString*>* pages = @[@"overview", @"send", @"receive", @"history",
                                      @"mining", @"multisig", @"corporate", @"nft"];
    const NSSize original = window.contentView.bounds.size;
    const NSSize sizes[] = { layout ? NSMakeSize(640, 480) : original, NSMakeSize(1280, 814) };
    for (const auto size : sizes) {
        [window setContentSize:size];
        for (NSString* page in pages) {
            [impl_->shell gotoPageById:page];
            [window.contentView layoutSubtreeIfNeeded];
            NSView* view = [impl_->shell pageById:page];
            if (!view || view.enclosingScrollView.hidden || NSIsEmptyRect(view.frame)) {
                *error = QStringLiteral("AppKit page does not fit or was not activated: %1")
                    .arg(QString::fromNSString(page));
                return false;
            }
            if (![page isEqualToString:@"overview"]) {
                QCoreApplication::processEvents();
                auto* scroll = impl_->workflows->nativeWalletFrame()->nativeCurrentScrollArea();
                const QString identifier = QString::fromNSString(page);
                impl_->workflowGlass->synchronize();
                if (!scroll || scroll->objectName() != QStringLiteral("pageScroll_") + identifier ||
                    !scroll->widget()->isVisible() || !hasQtWorkflowControls(scroll->widget(), page) ||
                    !visitQtWorkflowModes(scroll->widget(), page) ||
                    !impl_->workflowGlass->hasVisibleGlass()) {
                    *error = QStringLiteral("Qt controls or Liquid Glass did not load in %1").arg(identifier);
                    return false;
                }
            }
        }
        if (!layout) break;
    }
    if (layout) {
        GeoThemeManager* theme = [GeoThemeManager shared];
        const QString output = qEnvironmentVariable("GEOCOIN_UI_TEST_OUTPUT");
        if (!output.isEmpty() && !QDir().mkpath(output)) {
            *error = QStringLiteral("Cannot create the AppKit screenshot directory");
            return false;
        }
        for (NSString* appearance in @[@"dark", @"light"]) {
            theme.currentTheme = appearance;
            if (output.isEmpty()) continue;
            const QDir directory(output);
            for (NSString* page in pages) {
                [impl_->shell gotoPageById:page];
                const QString suffix = QString::fromNSString(appearance);
                auto capture = [&](NSString* identifier) {
                    const QString filename = QStringLiteral("appkit-%1-%2.png")
                        .arg(QString::fromNSString(identifier), suffix);
                    if (captureWindow(window, directory, filename)) return true;
                    *error = QStringLiteral("Cannot capture AppKit page %1").arg(QString::fromNSString(identifier));
                    return false;
                };
                if (!capture(page)) return false;
                if (![page isEqualToString:@"mining"]) continue;
                auto* scroll = impl_->workflows->nativeWalletFrame()->nativeCurrentScrollArea();
                auto* modes = scroll->widget()->findChild<QTabWidget*>(QStringLiteral("miningModeTabs"));
                modes->setCurrentIndex(1);
                if (!capture(@"mining-gpu")) return false;
                modes->setCurrentIndex(0);
            }
        }
        theme.currentTheme = @"dark";
    }
    [impl_->shell gotoPageById:@"overview"];
    [window setContentSize:original];
    return true;
}
