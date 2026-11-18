#import "GeoCoinOverviewPage.h"
#import "GeoCoinPageKit.h"

@interface GeoCoinOverviewPage ()
@property (nonatomic, strong) GeoCoinCoreBridge* core;
@property (nonatomic, strong) NSTextField* titleLabel;
@property (nonatomic, strong) GeoGlassPanel* walletPanel;
@property (nonatomic, strong) GeoGlassPanel* balancePanel;
@property (nonatomic, strong) GeoGlassPanel* networkPanel;
@property (nonatomic, strong) NSTextField* walletHint;
@property (nonatomic, strong) NSTextField* addressField;
@property (nonatomic, strong) GeoPillButton* addressCopyButton;
@property (nonatomic, strong) GeoPillButton* sendButton;
@property (nonatomic, strong) GeoPillButton* receiveButton;
@property (nonatomic, strong) NSTextField* blocksLabel;
@property (nonatomic, strong) NSTextField* connectionsLabel;
@property (nonatomic, strong) NSArray<NSTextField*>* balanceValues;
@property (nonatomic, strong) NSArray<NSTextField*>* watchOnlyValues;
@property (nonatomic, strong) NSStackView* watchOnlyGroup;
@property (nonatomic, strong) NSMutableDictionary<NSString*, NSMutableArray<NSTextField*>*>* translatedLabels;
@end

@implementation GeoCoinOverviewPage

- (instancetype)initWithCore:(GeoCoinCoreBridge*)core
{
    if ((self = [super initWithFrame:NSZeroRect])) {
        _core = core;
        self.identifier = @"overview";
        _translatedLabels = [NSMutableDictionary dictionary];

        NSStackView* body;
        NSStackView* content = (NSStackView*)GeoMakePage(@"Overview", &body);
        _titleLabel = (NSTextField*)content.arrangedSubviews.firstObject;
        [self addSubview:content];
        [NSLayoutConstraint activateConstraints:@[
            [content.topAnchor constraintEqualToAnchor:self.topAnchor],
            [content.bottomAnchor constraintEqualToAnchor:self.bottomAnchor],
            [content.leadingAnchor constraintEqualToAnchor:self.leadingAnchor],
            [content.trailingAnchor constraintEqualToAnchor:self.trailingAnchor],
        ]];
        [self addWalletTo:body];
        [self addBalancesTo:body];
        [self addNetworkTo:body];
        [self addActionsTo:body];
        [self applyLanguage];

        __weak GeoCoinOverviewPage* weakSelf = self;
        [core observeBalances:^(double available, double pending, double immature, double watchOnly) {
            [weakSelf updateBalances:available pending:pending immature:immature];
        }];
        [core observeAddress:^(NSString* address) { [weakSelf updateAddress:address]; }];

        [self setWalletName:core.walletName loaded:core.isWalletLoaded];
        [self setBlockCount:core.numBlocks];
        [self setConnectionCount:core.numConnections];
        [self updateBalances:0 pending:0 immature:0];
        [core refreshBalances];
    }
    return self;
}

- (void)addWalletTo:(NSStackView*)body
{
    NSStackView* contents;
    self.walletPanel = GeoMakeGroup(@"Wallet", &contents);
    self.walletPanel.identifier = @"overview-wallet";
    self.walletHint = GeoMakeLabel(@"", nil, 12);
    self.walletHint.maximumNumberOfLines = 0;
    self.walletHint.lineBreakMode = NSLineBreakByWordWrapping;
    [contents addArrangedSubview:self.walletHint];
    GeoFill(self.walletHint);

    NSStackView* addressRow = GeoMakeRow(7);
    self.addressField = GeoMakeField(@"No wallet loaded");
    self.addressField.identifier = @"overview-address";
    self.addressField.editable = NO;
    self.addressField.font = [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightMedium];
    [self.addressField setContentHuggingPriority:NSLayoutPriorityDefaultLow
                                  forOrientation:NSLayoutConstraintOrientationHorizontal];

    __weak GeoCoinOverviewPage* weakSelf = self;
    self.addressCopyButton = GeoMakePill(@"Copy", ^{
        NSString* address = weakSelf.addressField.stringValue;
        if (!address.length) return;
        NSPasteboard* pasteboard = NSPasteboard.generalPasteboard;
        [pasteboard clearContents];
        [pasteboard setString:address forType:NSPasteboardTypeString];
    });
    self.addressCopyButton.identifier = @"overview-copy";
    self.addressCopyButton.usesSystemGlass = YES;
    self.addressCopyButton.enabled = NO;
    [self.addressCopyButton setContentHuggingPriority:NSLayoutPriorityRequired
                                      forOrientation:NSLayoutConstraintOrientationHorizontal];
    [addressRow addArrangedSubview:self.addressField];
    [addressRow addArrangedSubview:self.addressCopyButton];
    [contents addArrangedSubview:addressRow];
    GeoFill(addressRow);

    [body addArrangedSubview:self.walletPanel];
    GeoFill(self.walletPanel);
}

- (NSTextField*)localizedLabel:(NSString*)title
{
    NSTextField* label = GeoMakeLabel(title, nil, 11);
    NSMutableArray<NSTextField*>* labels = self.translatedLabels[title];
    if (!labels) {
        labels = [NSMutableArray array];
        self.translatedLabels[title] = labels;
    }
    [labels addObject:label];
    return label;
}

- (NSTextField*)appendValueWithCaption:(NSString*)caption in:(NSStackView*)parent size:(CGFloat)size color:(NSColor*)color
{
    NSStackView* row = GeoMakeRow(6);
    NSTextField* value = GeoMakeLabel(@"", color, size);
    if (size >= 16) value.font = [NSFont systemFontOfSize:size weight:NSFontWeightBold];
    [row addArrangedSubview:[self localizedLabel:caption]];
    [row addArrangedSubview:value];
    [parent addArrangedSubview:row];
    return value;
}

- (NSArray<NSTextField*>*)addBalanceRows:(NSStackView*)contents
{
    NSTextField* available = [self appendValueWithCaption:@"Available:" in:contents size:16
        color:[NSColor colorWithSRGBRed:76/255.0 green:175/255.0 blue:80/255.0 alpha:1]];
    NSTextField* pending = [self appendValueWithCaption:@"Pending:" in:contents size:12 color:nil];
    NSTextField* immature = [self appendValueWithCaption:@"Immature:" in:contents size:12 color:nil];
    [contents setCustomSpacing:9 afterView:contents.arrangedSubviews.lastObject];
    NSTextField* total = [self appendValueWithCaption:@"Total:" in:contents size:14 color:nil];
    return @[available, pending, immature, total];
}

- (void)addBalancesTo:(NSStackView*)body
{
    NSStackView* contents;
    self.balancePanel = GeoMakeGroup(@"Balances", &contents);
    self.balancePanel.identifier = @"overview-balances";
    self.balanceValues = [self addBalanceRows:contents];
    self.watchOnlyGroup = GeoMakeStack(5);
    [self.watchOnlyGroup addArrangedSubview:[self localizedLabel:@"Watch-only (multisig):"]];
    self.watchOnlyValues = [self addBalanceRows:self.watchOnlyGroup];
    [contents setCustomSpacing:13 afterView:contents.arrangedSubviews.lastObject];
    [contents addArrangedSubview:self.watchOnlyGroup];
    self.watchOnlyGroup.hidden = YES;
    [body addArrangedSubview:self.balancePanel];
    GeoFill(self.balancePanel);
}

- (void)addNetworkTo:(NSStackView*)body
{
    NSStackView* contents;
    self.networkPanel = GeoMakeGroup(@"Network", &contents);
    self.networkPanel.identifier = @"overview-network";
    self.blocksLabel = [self appendValueWithCaption:@"Blocks:" in:contents size:12 color:nil];
    self.connectionsLabel = [self appendValueWithCaption:@"Connections:" in:contents size:12 color:nil];
    [body addArrangedSubview:self.networkPanel];
    GeoFill(self.networkPanel);
}

- (void)addActionsTo:(NSStackView*)body
{
    NSStackView* actions = GeoMakeRow(15);
    actions.distribution = NSStackViewDistributionFillEqually;
    __weak GeoCoinOverviewPage* weakSelf = self;
    self.sendButton = GeoMakePill(@"Send Coins", ^{
        void (^navigate)(NSString*) = weakSelf.onNavigate;
        if (navigate) navigate(@"send");
    });
    self.receiveButton = GeoMakePill(@"Receive Coins", ^{
        void (^navigate)(NSString*) = weakSelf.onNavigate;
        if (navigate) navigate(@"receive");
    });
    self.sendButton.usesSystemGlass = YES;
    self.receiveButton.usesSystemGlass = YES;
    [self.sendButton.heightAnchor constraintGreaterThanOrEqualToConstant:32].active = YES;
    [self.receiveButton.heightAnchor constraintGreaterThanOrEqualToConstant:32].active = YES;
    [actions addArrangedSubview:self.sendButton];
    [actions addArrangedSubview:self.receiveButton];
    [body addArrangedSubview:actions];
    GeoFill(actions);
}

- (NSString*)localized:(NSString*)source
{
    return [self.core localized:source context:@"OverviewPage"];
}

- (void)applyLanguage
{
    self.titleLabel.stringValue = [self localized:@"Overview"];
    self.walletPanel.sectionTitle = [self localized:@"Wallet"];
    self.balancePanel.sectionTitle = [self localized:@"Balances"];
    self.networkPanel.sectionTitle = [self localized:@"Network"];
    self.walletHint.stringValue = [self localized:
        @"No wallet is loaded. Create or load one from the File menu to receive coins."];
    self.addressField.placeholderString = [self localized:@"No wallet loaded"];
    self.addressCopyButton.title = [self localized:@"Copy"];
    self.sendButton.title = [self localized:@"Send Coins"];
    self.receiveButton.title = [self localized:@"Receive Coins"];
    [self.translatedLabels enumerateKeysAndObjectsUsingBlock:
        ^(NSString* source, NSArray<NSTextField*>* labels, BOOL* stop) {
            for (NSTextField* label in labels) label.stringValue = [self localized:source];
        }];
}

- (void)updateAddress:(NSString*)address
{
    self.addressField.stringValue = address ?: @"";
    self.addressCopyButton.enabled = address.length > 0;
}

- (void)setWalletName:(NSString*)name loaded:(BOOL)loaded
{
    self.walletHint.hidden = loaded;
    [self updateAddress:loaded ? self.core.primaryAddress : nil];
}

- (void)setBlockCount:(NSInteger)count
{
    self.blocksLabel.stringValue = [NSString stringWithFormat:@"%ld", (long)count];
}

- (void)setConnectionCount:(NSInteger)count
{
    self.connectionsLabel.stringValue = [NSString stringWithFormat:@"%ld", (long)count];
}

- (void)updateBalances:(double)available pending:(double)pending immature:(double)immature
{
    NSArray<NSNumber*>* amounts = @[@(available), @(pending), @(immature), @(available + pending + immature)];
    [self updateLabels:self.balanceValues withAmounts:amounts];
    NSArray<NSNumber*>* watch = self.core.watchOnlyBalances;
    self.watchOnlyGroup.hidden = watch.count < 3 ||
        (watch[0].doubleValue == 0 && watch[1].doubleValue == 0 && watch[2].doubleValue == 0);
    if (!self.watchOnlyGroup.hidden) {
        [self updateLabels:self.watchOnlyValues withAmounts:@[watch[0], watch[1], watch[2],
            @([watch[0] doubleValue] + [watch[1] doubleValue] + [watch[2] doubleValue])]];
    }
}

- (void)updateLabels:(NSArray<NSTextField*>*)labels withAmounts:(NSArray<NSNumber*>*)amounts
{
    for (NSUInteger i = 0; i < labels.count; ++i) {
        labels[i].stringValue = [NSString stringWithFormat:@"%.8f GC", [amounts[i] doubleValue]];
    }
}

- (void)applyTheme
{
    GeoApplyThemeToView(self);
}

@end
