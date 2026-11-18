#import "GeoCoinMainWindowController.h"
#import "GeoCoinMetalBackdrop.h"
#import "GeoCoinPageKit.h"
#import "GeoCoinOverviewPage.h"

@interface GeoPageCanvas : NSView
@end

@implementation GeoPageCanvas
- (BOOL)isFlipped { return YES; }
@end

@interface GeoCoinMainWindowController () <NSWindowDelegate, NSMenuDelegate>
@property (nonatomic, strong, readwrite) NSWindow* window;
@property (nonatomic, strong, readwrite) GeoCoinCoreBridge* core;
@property (nonatomic, strong) GeoCoinMetalBackdrop* backdrop;
@property (nonatomic, strong) NSStackView* menus;
@property (nonatomic, strong) NSScrollView* navigation;
@property (nonatomic, strong) NSStackView* status;
@property (nonatomic, strong) GeoPillButton* themeButton;
@property (nonatomic, strong) NSArray<GeoPillButton*>* navigationButtons;
@property (nonatomic, strong) NSArray<NSView*>* pages;
@property (nonatomic, strong) NSArray<NSScrollView*>* pageScrolls;
@property (nonatomic, strong) GeoCoinOverviewPage* overview;
@property (nonatomic, copy) NSString* currentPageId;
@property (nonatomic, strong) NSTextField* connectionsLabel;
@property (nonatomic, strong) NSTextField* blocksLabel;
@property (nonatomic, strong) NSProgressIndicator* progressBar;
@property (nonatomic, strong) NSTextField* progressLabel;
@property (nonatomic, strong) NSTextField* hashrateLabel;
@property (nonatomic, strong) NSMutableDictionary<NSString*, NSImage*>* artwork;
@property (nonatomic, strong) id themeObserver;
@property (nonatomic, assign) NSInteger connectionCount;
@property (nonatomic, assign) NSInteger blockCount;
@property (nonatomic, assign) double cpuHashrate;
@property (nonatomic, assign) double gpuHashrate;
@end

@implementation GeoCoinMainWindowController

- (instancetype)initWithCore:(GeoCoinCoreBridge*)core
{
    if ((self = [super init])) {
        _core = core;
        _artwork = [NSMutableDictionary dictionary];
        if (core.testing) {
            [GeoThemeManager shared].persistsTheme = NO;
            [GeoThemeManager shared].currentTheme = @"dark";
        }
        [self buildWindow];
        [self bindCore];
        [self applyLanguage];
        [self applyTheme];
        __weak GeoCoinMainWindowController* weakSelf = self;
        _themeObserver = [[GeoThemeManager shared] addThemeObserver:^(NSString* theme) {
            [weakSelf applyTheme];
        }];
    }
    return self;
}

- (void)dealloc
{
    if (_themeObserver) [[GeoThemeManager shared] removeThemeObserver:_themeObserver];
}

- (BOOL)isUsingMetal
{
    return self.backdrop.isUsingMetal;
}

- (void)buildWindow
{
    NSWindow* window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 1180, 760)
        styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable
                  | NSWindowStyleMaskResizable | NSWindowStyleMaskFullSizeContentView
        backing:NSBackingStoreBuffered defer:NO];
    window.delegate = self;
    window.releasedWhenClosed = NO;
    window.titlebarAppearsTransparent = YES;
    window.minSize = NSMakeSize(640, 480);
    [window center];
    self.window = window;

    NSView* root = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 1180, 760)];
    root.wantsLayer = YES;
    window.contentView = root;
    self.backdrop = [[GeoCoinMetalBackdrop alloc] initWithFrame:root.bounds];
    self.backdrop.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    [root addSubview:self.backdrop];

    self.menus = GeoMakeRow(3);
    self.navigation = [self buildNavigation];
    self.status = [self buildStatus];
    NSView* pageContainer = [self buildPageStack];
    [root addSubview:self.menus];
    [root addSubview:self.navigation];
    [root addSubview:pageContainer];
    [root addSubview:self.status];

    NSLayoutGuide* windowContent = window.contentLayoutGuide;
    [NSLayoutConstraint activateConstraints:@[
        [self.menus.topAnchor constraintEqualToAnchor:windowContent.topAnchor constant:1],
        [self.menus.leadingAnchor constraintEqualToAnchor:root.leadingAnchor constant:9],
        [self.navigation.topAnchor constraintEqualToAnchor:self.menus.bottomAnchor constant:2],
        [self.navigation.leadingAnchor constraintEqualToAnchor:root.leadingAnchor constant:9],
        [self.navigation.trailingAnchor constraintEqualToAnchor:root.trailingAnchor constant:-9],
        [self.navigation.heightAnchor constraintEqualToConstant:29],
        [pageContainer.topAnchor constraintEqualToAnchor:self.navigation.bottomAnchor constant:3],
        [pageContainer.leadingAnchor constraintEqualToAnchor:root.leadingAnchor constant:7],
        [pageContainer.trailingAnchor constraintEqualToAnchor:root.trailingAnchor constant:-7],
        [pageContainer.bottomAnchor constraintEqualToAnchor:self.status.topAnchor constant:-3],
        [self.status.bottomAnchor constraintEqualToAnchor:root.bottomAnchor constant:-8],
        [self.status.leadingAnchor constraintEqualToAnchor:root.leadingAnchor constant:14],
        [self.status.trailingAnchor constraintEqualToAnchor:root.trailingAnchor constant:-12],
        [self.status.heightAnchor constraintEqualToConstant:28],
    ]];

    [self restoreUiSettings];
    [self gotoPageById:self.currentPageId ?: @"overview"];
}

- (NSArray<NSArray<NSString*>*>*)navigationSpecs
{
    return @[
        @[@"overview", @"&Overview"], @[@"send", @"&Send"], @[@"receive", @"&Receive"],
        @[@"history", @"&Transactions"], @[@"mining", @"&Mining"], @[@"multisig", @"&Multisig"],
        @[@"corporate", @"&Corporate"], @[@"nft", @"&NFT / Cadastr"],
    ];
}

- (NSScrollView*)transparentScroll
{
    NSScrollView* scroll = [[NSScrollView alloc] initWithFrame:NSZeroRect];
    scroll.translatesAutoresizingMaskIntoConstraints = NO;
    scroll.drawsBackground = NO;
    scroll.contentView.drawsBackground = NO;
    scroll.borderType = NSNoBorder;
    scroll.scrollerStyle = NSScrollerStyleOverlay;
    scroll.autohidesScrollers = YES;
    return scroll;
}

- (NSScrollView*)buildNavigation
{
    NSScrollView* scroll = [self transparentScroll];
    scroll.hasHorizontalScroller = YES;
    NSView* canvas = [GeoPageCanvas new];
    canvas.translatesAutoresizingMaskIntoConstraints = NO;
    scroll.documentView = canvas;

    NSStackView* row = GeoMakeRow(4);
    [canvas addSubview:row];
    [NSLayoutConstraint activateConstraints:@[
        [canvas.widthAnchor constraintGreaterThanOrEqualToAnchor:scroll.contentView.widthAnchor],
        [canvas.heightAnchor constraintEqualToAnchor:scroll.contentView.heightAnchor],
        [row.leadingAnchor constraintEqualToAnchor:canvas.leadingAnchor],
        [row.centerYAnchor constraintEqualToAnchor:canvas.centerYAnchor],
        [row.trailingAnchor constraintLessThanOrEqualToAnchor:canvas.trailingAnchor],
    ]];
    NSLayoutConstraint* fitWidth = [canvas.widthAnchor constraintEqualToAnchor:scroll.contentView.widthAnchor];
    fitWidth.priority = NSLayoutPriorityDefaultLow;
    fitWidth.active = YES;

    NSMutableArray<GeoPillButton*>* buttons = [NSMutableArray array];
    __weak GeoCoinMainWindowController* weakSelf = self;
    for (NSArray<NSString*>* spec in [self navigationSpecs]) {
        NSString* pageId = spec[0];
        GeoPillButton* button = GeoMakePill(spec[1], ^{ [weakSelf gotoPageById:pageId]; });
        button.identifier = pageId;
        button.usesSystemGlass = YES;
        button.toolTip = nil;
        [button setContentHuggingPriority:NSLayoutPriorityRequired
                            forOrientation:NSLayoutConstraintOrientationHorizontal];
        [button.heightAnchor constraintEqualToConstant:27].active = YES;
        [row addArrangedSubview:button];
        [buttons addObject:button];
    }
    self.navigationButtons = buttons;
    return scroll;
}

- (NSStackView*)buildStatus
{
    NSStackView* row = GeoMakeRow(8);
    NSColor* blue = [NSColor colorWithSRGBRed:102/255.0 green:128/255.0 blue:235/255.0 alpha:1];
    self.connectionsLabel = GeoMakeLabel(@"0 peers", blue, 11);
    self.blocksLabel = GeoMakeLabel(@"0 blocks", blue, 11);
    self.hashrateLabel = GeoMakeLabel(@"", [GeoThemeManager shared].colors.mutedText, 11);
    self.hashrateLabel.identifier = @"mining-rates-status";
    self.progressBar = [[NSProgressIndicator alloc] initWithFrame:NSZeroRect];
    self.progressBar.style = NSProgressIndicatorStyleBar;
    self.progressBar.indeterminate = NO;
    self.progressBar.controlSize = NSControlSizeSmall;
    self.progressBar.maxValue = 1;
    self.progressBar.hidden = YES;
    [self.progressBar.widthAnchor constraintEqualToConstant:64].active = YES;
    self.progressLabel = GeoMakeLabel(@"", nil, 11);
    self.progressLabel.hidden = YES;

    NSView* spacer = [NSView new];
    [spacer setContentHuggingPriority:NSLayoutPriorityDefaultLow
                       forOrientation:NSLayoutConstraintOrientationHorizontal];
    GeoThemeManager* theme = [GeoThemeManager shared];
    self.themeButton = GeoMakePill(@"", ^{
        theme.currentTheme = [theme.currentTheme isEqualToString:@"dark"] ? @"light" : @"dark";
    });
    self.themeButton.identifier = @"theme-toggle";
    self.themeButton.imagePosition = NSImageOnly;
    [self.themeButton.widthAnchor constraintEqualToConstant:28].active = YES;
    [self.themeButton.heightAnchor constraintEqualToConstant:26].active = YES;
    [row addArrangedSubview:self.connectionsLabel];
    [row addArrangedSubview:self.blocksLabel];
    [row addArrangedSubview:self.hashrateLabel];
    [row addArrangedSubview:self.progressBar];
    [row addArrangedSubview:self.progressLabel];
    [row addArrangedSubview:spacer];
    [row addArrangedSubview:self.themeButton];
    return row;
}

- (NSView*)buildPageStack
{
    NSView* container = [NSView new];
    container.translatesAutoresizingMaskIntoConstraints = NO;
    self.overview = [[GeoCoinOverviewPage alloc] initWithCore:self.core];
    __weak GeoCoinMainWindowController* weakSelf = self;
    self.overview.onNavigate = ^(NSString* page) { [weakSelf gotoPageById:page]; };

    NSMutableArray<NSView*>* pages = [NSMutableArray array];
    for (NSArray<NSString*>* spec in [self navigationSpecs]) {
        NSString* pageId = spec[0];
        NSView* page = [pageId isEqualToString:@"overview"] ? self.overview : [NSView new];
        page.identifier = pageId;
        [pages addObject:page];
    }
    self.pages = pages;

    NSMutableArray<NSScrollView*>* scrolls = [NSMutableArray array];
    for (NSView* page in self.pages) {
        NSScrollView* scroll = [self transparentScroll];
        const BOOL hasEmbeddedScroll = page != self.overview;
        scroll.hasVerticalScroller = !hasEmbeddedScroll;
        NSView* canvas = [GeoPageCanvas new];
        canvas.translatesAutoresizingMaskIntoConstraints = NO;
        scroll.documentView = canvas;
        page.translatesAutoresizingMaskIntoConstraints = NO;
        [canvas addSubview:page];
        [container addSubview:scroll];
        [NSLayoutConstraint activateConstraints:@[
            [scroll.topAnchor constraintEqualToAnchor:container.topAnchor],
            [scroll.bottomAnchor constraintEqualToAnchor:container.bottomAnchor],
            [scroll.leadingAnchor constraintEqualToAnchor:container.leadingAnchor],
            [scroll.trailingAnchor constraintEqualToAnchor:container.trailingAnchor],
            [canvas.widthAnchor constraintEqualToAnchor:scroll.contentView.widthAnchor],
            [page.topAnchor constraintEqualToAnchor:canvas.topAnchor constant:5],
            [page.bottomAnchor constraintEqualToAnchor:canvas.bottomAnchor
                                                   constant:hasEmbeddedScroll ? -5 : -8],
            [page.leadingAnchor constraintEqualToAnchor:canvas.leadingAnchor constant:9],
            [page.trailingAnchor constraintEqualToAnchor:canvas.trailingAnchor constant:-9],
        ]];
        if (hasEmbeddedScroll) {
            [canvas.heightAnchor constraintEqualToAnchor:scroll.contentView.heightAnchor].active = YES;
        }
        [scrolls addObject:scroll];
    }
    self.pageScrolls = scrolls;
    return container;
}

- (NSString*)localized:(NSString*)source
{
    NSString* text = [self.core localized:source context:@"GeoCoinGUI"];
    return [text stringByReplacingOccurrencesOfString:@"&" withString:@""];
}

- (void)buildMenu:(NSString*)title items:(NSArray<NSArray<NSString*>*>*)commands
{
    GeoPillButton* button = GeoMakePill([self localized:title], nil);
    button.plainMenu = YES;
    button.layer.borderWidth = 0;
    [button applyTheme];
    NSMenu* menu = [[NSMenu alloc] initWithTitle:button.title];
    menu.delegate = self;
    menu.autoenablesItems = NO;
    for (NSArray<NSString*>* command in commands) {
        if (command.count == 0) {
            [menu addItem:NSMenuItem.separatorItem];
        } else if ([command[0] isEqualToString:@"language"]) {
            NSMenuItem* language = [[NSMenuItem alloc] initWithTitle:[self localized:@"&Language"]
                action:NULL keyEquivalent:@""];
            language.submenu = [[NSMenu alloc] initWithTitle:language.title];
            [language.submenu addItem:[self menuItem:@"&English" command:@"language-en"]];
            [language.submenu addItem:[self menuItem:@"&Russian" command:@"language-ru"]];
            [menu addItem:language];
        } else {
            [menu addItem:[self menuItem:command[1] command:command[0]]];
        }
    }
    __weak GeoPillButton* weakButton = button;
    button.geoAction = ^{
        GeoPillButton* anchor = weakButton;
        if (anchor) {
            [menu popUpMenuPositioningItem:nil
                atLocation:NSMakePoint(0, NSMinY(anchor.bounds)) inView:anchor];
        }
    };
    [button.heightAnchor constraintEqualToConstant:20].active = YES;
    [self.menus addArrangedSubview:button];
}

- (NSMenuItem*)menuItem:(NSString*)title command:(NSString*)command
{
    NSMenuItem* item = [[NSMenuItem alloc] initWithTitle:[self localized:title]
        action:@selector(invokeMenuItem:) keyEquivalent:@""];
    item.target = self;
    item.representedObject = command;
    return item;
}

- (void)invokeMenuItem:(NSMenuItem*)item
{
    if (self.onMenuAction) self.onMenuAction(item.representedObject);
}

- (void)menuWillOpen:(NSMenu*)menu
{
    NSArray<NSString*>* walletCommands = @[@"close-wallet", @"backup-wallet", @"export-private-key",
        @"import-private-key", @"encrypt-wallet", @"change-passphrase"];
    for (NSMenuItem* item in menu.itemArray) {
        if (item.representedObject && [walletCommands containsObject:item.representedObject]) {
            item.enabled = self.core.isWalletLoaded;
        }
    }
}

- (void)applyLanguage
{
    for (NSView* view in self.menus.arrangedSubviews.copy) {
        [self.menus removeArrangedSubview:view];
        [view removeFromSuperview];
    }
    [self buildMenu:@"&File" items:@[
        @[@"create-wallet", @"&Create Wallet..."], @[@"load-wallet", @"&Load Wallet..."],
        @[@"close-wallet", @"&Close Wallet"], @[], @[@"backup-wallet", @"&Backup Wallet..."],
        @[@"export-private-key", @"&Export Private Key..."], @[@"import-private-key", @"&Import Private Key..."],
        @[], @[@"quit", @"E&xit"],
    ]];
    [self buildMenu:@"&Settings" items:@[
        @[@"encrypt-wallet", @"&Encrypt Wallet..."], @[@"change-passphrase", @"&Change Passphrase..."],
        @[], @[@"language"], @[@"options", @"&Options..."],
    ]];
    [self buildMenu:@"&Help" items:@[@[@"about", @"&About GeoCoin Core"]]];

    NSArray<NSArray<NSString*>*>* specs = [self navigationSpecs];
    for (NSUInteger i = 0; i < self.navigationButtons.count; ++i) {
        self.navigationButtons[i].title = [self localized:specs[i][1]];
    }
    for (id page in self.pages) {
        if ([page respondsToSelector:@selector(applyLanguage)]) [page applyLanguage];
    }
    [self updateWindowTitle];
    [self updateStatus];
    [self updateThemeButton];
}

- (void)setTitleAddText:(NSString*)titleAddText
{
    _titleAddText = [titleAddText copy];
    [self updateWindowTitle];
}

- (void)updateWindowTitle
{
    NSString* name = self.core.isWalletLoaded ? self.core.walletName : nil;
    NSString* suffix = self.titleAddText ?: self.core.selectedNetwork;
    NSMutableArray<NSString*>* parts = [NSMutableArray arrayWithObject:[self localized:@"GeoCoin Core"]];
    if (name.length) [parts addObject:name];
    if (suffix.length && ![suffix isEqualToString:@"degree"]) [parts addObject:suffix];
    self.window.title = [parts componentsJoinedByString:@" - "];
}

- (void)restoreUiSettings
{
    self.currentPageId = @"overview";
    if (self.core.testing) return;
    NSUserDefaults* defaults = NSUserDefaults.standardUserDefaults;
    BOOL restore = ![defaults objectForKey:@"ui/restore_last_page"] ||
        [defaults boolForKey:@"ui/restore_last_page"];
    NSString* key = restore ? @"ui/last_page" : @"ui/startup_page";
    NSString* requestedPage = [defaults stringForKey:key];
    if (requestedPage && [self pageById:requestedPage]) self.currentPageId = requestedPage;

    id stored = [defaults objectForKey:@"ui/main_window_geometry"];
    if ([stored isKindOfClass:NSData.class]) {
        stored = [[NSString alloc] initWithData:stored encoding:NSUTF8StringEncoding];
    }
    if (![stored isKindOfClass:NSString.class]) return;
    NSRect frame = NSRectFromString(stored);
    if (NSIsEmptyRect(frame)) return;
    for (NSScreen* screen in NSScreen.screens) {
        NSRect visible = NSIntersectionRect(screen.visibleFrame, frame);
        if (visible.size.width >= 100 && visible.size.height >= 100) {
            [self.window setFrame:[self.window constrainFrameRect:frame toScreen:screen] display:NO];
            break;
        }
    }
}

- (void)persistUiSettings
{
    if (self.core.testing) return;
    NSUserDefaults* defaults = NSUserDefaults.standardUserDefaults;
    [defaults setObject:self.currentPageId forKey:@"ui/last_page"];
    [defaults setObject:NSStringFromRect(self.window.frame) forKey:@"ui/main_window_geometry"];
}

- (void)show
{
    [self.window makeKeyAndOrderFront:nil];
    [self updateBackdropGeometry];
    [self.backdrop requestFrame];
}

- (nullable NSView*)pageById:(NSString*)pageId
{
    for (NSView* page in self.pages) {
        if ([page.identifier isEqualToString:pageId]) return page;
    }
    return nil;
}

- (void)gotoPageById:(NSString*)pageId
{
    if (![self pageById:pageId]) return;
    self.currentPageId = pageId;
    for (NSUInteger i = 0; i < self.pages.count; ++i) {
        self.pageScrolls[i].hidden = ![self.pages[i].identifier isEqualToString:pageId];
    }
    for (GeoPillButton* button in self.navigationButtons) {
        button.active = [button.identifier isEqualToString:pageId];
    }
    [self persistUiSettings];
    if (self.onPageChanged) self.onPageChanged(pageId);
}

- (void)bindCore
{
    __weak GeoCoinMainWindowController* weakSelf = self;
    self.core.onConnections = ^(NSInteger count) {
        weakSelf.connectionCount = count;
        [weakSelf.overview setConnectionCount:count];
        [weakSelf updateStatus];
    };
    self.core.onBlocks = ^(NSInteger count) {
        weakSelf.blockCount = count;
        [weakSelf.overview setBlockCount:count];
        [weakSelf updateStatus];
    };
    self.core.onSyncProgress = ^(double progress) {
        weakSelf.progressBar.hidden = weakSelf.progressLabel.hidden = progress >= 1;
        weakSelf.progressBar.doubleValue = MAX(0, MIN(1, progress));
        weakSelf.progressLabel.stringValue = [weakSelf localized:@"Synchronizing with the network"];
    };
    self.core.onMining = ^(BOOL enabled, NSInteger threads, double hashrate, NSInteger accepted, NSInteger rejected) {
        weakSelf.cpuHashrate = enabled ? hashrate : 0;
        [weakSelf updateStatus];
    };
    self.core.onWalletInfo = ^(NSString* name, NSInteger walletId, BOOL loaded, BOOL pruned) {
        [weakSelf updateWindowTitle];
        [weakSelf.overview setWalletName:name loaded:loaded];
    };
    self.core.onMessage = ^(NSString* title, NSString* message, NSUInteger style) {
        NSWindow* window = weakSelf.window;
        if (!window.isVisible) return;
        NSAlert* alert = [NSAlert new];
        alert.messageText = title;
        alert.informativeText = message;
        alert.alertStyle = style == 2 ? NSAlertStyleWarning : NSAlertStyleInformational;
        [alert beginSheetModalForWindow:window completionHandler:nil];
    };
}

static NSString* GeoHashrate(double rate)
{
    if (!isfinite(rate) || rate < 0) rate = 0;
    NSArray<NSString*>* units = @[@"H/s", @"kH/s", @"MH/s", @"GH/s", @"TH/s"];
    NSUInteger index = 0;
    while (rate >= 1000 && index + 1 < units.count) {
        rate /= 1000;
        ++index;
    }
    return [NSString stringWithFormat:@"%.2f %@", rate, units[index]];
}

- (void)updateStatus
{
    NSString* peers = [NSString stringWithFormat:@"%ld", (long)self.connectionCount];
    NSString* blocks = [NSString stringWithFormat:@"%ld", (long)self.blockCount];
    self.connectionsLabel.stringValue = [[self localized:@"0 peers"] stringByReplacingOccurrencesOfString:@"0" withString:peers];
    self.blocksLabel.stringValue = [[self localized:@"0 blocks"] stringByReplacingOccurrencesOfString:@"0" withString:blocks];
    NSString* rates = [self localized:@"Total: %1 | CPU: %2 | GPU: %3"];
    rates = [rates stringByReplacingOccurrencesOfString:@"%1" withString:GeoHashrate(self.cpuHashrate + self.gpuHashrate)];
    rates = [rates stringByReplacingOccurrencesOfString:@"%2" withString:GeoHashrate(self.cpuHashrate)];
    self.hashrateLabel.stringValue = [rates stringByReplacingOccurrencesOfString:@"%3" withString:GeoHashrate(self.gpuHashrate)];
}

- (void)updateGpuHashrate:(double)hashrate
{
    self.gpuHashrate = hashrate;
    [self updateStatus];
}

- (void)applyTheme
{
    NSString* theme = [GeoThemeManager shared].currentTheme;
    const BOOL dark = [theme isEqualToString:@"dark"];
    self.window.appearance = [NSAppearance appearanceNamed:dark ? NSAppearanceNameDarkAqua : NSAppearanceNameAqua];
    NSImage* image = self.artwork[theme];
    if (!image) {
        image = [self.core backgroundImageForTheme:theme];
        if (image) self.artwork[theme] = image;
    }
    [self.backdrop setArtwork:image dark:dark];
    GeoApplyThemeToView(self.window.contentView);
    [self updateThemeButton];
}

- (void)updateThemeButton
{
    const BOOL dark = [[GeoThemeManager shared].currentTheme isEqualToString:@"dark"];
    NSString* label = [self localized:dark ? @"Light" : @"Dark"];
    self.themeButton.toolTip = label;
    [self.themeButton setAccessibilityLabel:label];
    if (@available(macOS 11.0, *)) {
        self.themeButton.image = [NSImage imageWithSystemSymbolName:dark ? @"sun.max" : @"moon"
            accessibilityDescription:label];
    } else {
        self.themeButton.title = dark ? @"☀" : @"☾";
        self.themeButton.imagePosition = NSNoImage;
    }
}

- (void)updateBackdropGeometry
{
    NSView* root = self.window.contentView;
    [root layoutSubtreeIfNeeded];
    CGFloat height = NSHeight(root.bounds);
    NSRect legalContent = [root convertRect:self.window.contentLayoutRect fromView:nil];
    CGFloat titleHeight = MAX(0, height - NSMaxY(legalContent));
    CGFloat artworkTop = MAX(titleHeight, height - NSMinY(self.navigation.frame));
    CGFloat artworkBottom = NSMaxY(self.status.frame);
    [self.backdrop setHeaderHeight:titleHeight artworkTopInset:artworkTop artworkBottomInset:artworkBottom];
}

- (void)windowDidResize:(NSNotification*)notification
{
    [self updateBackdropGeometry];
}

- (void)windowDidChangeOcclusionState:(NSNotification*)notification
{
    if (self.window.occlusionState & NSWindowOcclusionStateVisible) [self.backdrop requestFrame];
}

- (void)windowDidEnterFullScreen:(NSNotification*)notification
{
    self.backdrop.needsLayout = YES;
}

- (void)windowDidExitFullScreen:(NSNotification*)notification
{
    self.backdrop.needsLayout = YES;
}

- (void)windowDidEndLiveResize:(NSNotification*)notification
{
    [self persistUiSettings];
}

- (BOOL)windowShouldClose:(NSWindow*)sender
{
    [self persistUiSettings];
    void (^requestClose)(NSString*) = self.onMenuAction;
    if (!requestClose) return YES;

    // The application can order this window out during shutdown. Let AppKit
    // finish processing the original close action before that happens.
    dispatch_async(dispatch_get_main_queue(), ^{ requestClose(@"quit"); });
    return NO;
}

@end
