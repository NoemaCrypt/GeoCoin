#import "GeoCoinSendCoinsPage.h"
#import "GeoCoinPageKit.h"

@interface GeoCoinSendCoinsPage ()
@property (nonatomic, weak) GeoCoinCoreBridge* core;
@property (nonatomic, strong) NSTextField* payToField;
@property (nonatomic, strong) NSTextField* amountField;
@property (nonatomic, strong) NSTextField* feeField;
@property (nonatomic, strong) NSTextField* balance;
@property (nonatomic, strong) GeoPillButton* sendButton;
@end

@implementation GeoCoinSendCoinsPage

- (instancetype)initWithCore:(GeoCoinCoreBridge*)core
{
    if ((self = [super initWithFrame:NSZeroRect])) {
        _core = core;
        self.identifier = @"send";
        NSStackView* body;
        NSView* page = GeoMakePage([core localized:@"Send Coins" context:@"SendCoinsPage"], &body);
        [self addSubview:page];
        [NSLayoutConstraint activateConstraints:@[
            [page.topAnchor constraintEqualToAnchor:self.topAnchor],
            [page.bottomAnchor constraintEqualToAnchor:self.bottomAnchor],
            [page.leadingAnchor constraintEqualToAnchor:self.leadingAnchor],
            [page.trailingAnchor constraintEqualToAnchor:self.trailingAnchor]
        ]];

        GeoGlassColors* colors = [GeoThemeManager shared].colors;
        _balance = GeoMakeLabel(@"", colors.text, 13);
        [body addArrangedSubview:_balance];

        NSStackView* fields;
        GeoGlassPanel* card = GeoMakeGroup([core localized:@"Payment Details" context:@"SendCoinsPage"],
                                           &fields);
        _payToField = GeoMakeField(@"GEO…");
        _amountField = GeoMakeField(@"0.00000000");
        _feeField = GeoMakeField(@"0.00000000");
        _feeField.stringValue = [core standardFee];
        NSArray* names = @[@"Pay To:", @"Amount:", @"Fee:"];
        NSArray* controls = @[_payToField, _amountField, _feeField];
        for (NSUInteger index = 0; index < names.count; ++index) {
            NSTextField* field = controls[index];
            NSString* name = [core localized:names[index] context:@"SendCoinsPage"];
            [field setAccessibilityLabel:name];
            NSStackView* row = GeoMakeRow(10);
            NSTextField* label = GeoMakeLabel(name, colors.text, 13);
            [label.widthAnchor constraintEqualToConstant:100].active = YES;
            [row addArrangedSubview:label];
            [row addArrangedSubview:field];
            [fields addArrangedSubview:row];
        }
        _payToField.nextKeyView = _amountField;
        _amountField.nextKeyView = _feeField;

        __weak GeoCoinSendCoinsPage* weakPage = self;
        _sendButton = GeoMakePill([core localized:@"Send" context:@"SendCoinsPage"], ^{
            GeoCoinSendCoinsPage* page = weakPage;
            [page.core confirmSendCoinsTo:page.payToField.stringValue
                                   amount:page.amountField.stringValue fee:page.feeField.stringValue
                                   window:page.window completion:^{
                weakPage.payToField.stringValue = @"";
                weakPage.amountField.stringValue = @"";
            }];
        });
        _feeField.nextKeyView = _sendButton;
        [fields addArrangedSubview:_sendButton];
        [body addArrangedSubview:card];

        [core observeBalances:^(double available, double pending, double immature, double watch) {
            (void)pending;
            (void)immature;
            (void)watch;
            GeoCoinSendCoinsPage* page = weakPage;
            NSString* caption = [page.core localized:@"Available Balance:" context:@"SendCoinsPage"];
            page.balance.stringValue = [NSString stringWithFormat:@"%@ %.8f GC", caption, available];
            page.sendButton.enabled = page.core.isWalletLoaded;
        }];
    }
    return self;
}

- (void)applyTheme {
    self.balance.textColor = [GeoThemeManager shared].colors.text;
}

@end
