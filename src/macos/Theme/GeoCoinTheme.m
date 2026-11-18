// AppKit color and control styles derived from GeoCoin Core's Qt theme.
#import "GeoCoinTheme.h"
#import <QuartzCore/QuartzCore.h>

static GeoThemeManager *gGeoThemeManager = nil;

@interface GeoGlassColors ()
@end

@implementation GeoGlassColors
@end

static NSColor *GeoHex(NSUInteger hex, CGFloat alpha) {
    return [NSColor colorWithSRGBRed:((hex >> 16) & 0xFF) / 255.0
                               green:((hex >> 8) & 0xFF) / 255.0
                                blue:(hex & 0xFF) / 255.0
                               alpha:alpha];
}

static GeoGlassColors *GeoMakeColors(BOOL dark) {
    GeoGlassColors *c = [GeoGlassColors new];
    if (dark) {
        c.page         = GeoHex(0x080A0D, 1.0);
        c.pageAlt      = GeoHex(0x111722, 1.0);
        c.text         = GeoHex(0xF8EBD0, 1.0);
        c.mutedText    = GeoHex(0xB8C0CC, 1.0);
        c.panel        = GeoHex(0x05070B, 197/255.0);
        c.panelTop     = GeoHex(0x0C1118, 173/255.0);
        c.panelBottom  = GeoHex(0x000000, 222/255.0);
        c.field        = GeoHex(0x080C12, 224/255.0);
        c.fieldFocus   = GeoHex(0x121A26, 238/255.0);
        c.border       = GeoHex(0xD6A84F, 166/255.0);
        c.borderLight  = GeoHex(0xFFF3CD, 180/255.0);
        c.borderShadow = GeoHex(0x000000, 160/255.0);
        c.button       = GeoHex(0x1A202B, 216/255.0);
        c.buttonHover  = GeoHex(0x2E394B, 236/255.0);
        c.selection    = GeoHex(0x4A90E2, 1.0);
        c.tabActive    = GeoHex(0x141B26, 210/255.0);
        c.tabInactive  = GeoHex(0x0C111A, 196/255.0);
    } else {
        // guiutil.cpp glassColors(light) — unchanged from guiutil.cpp.
        c.page        = GeoHex(0xF7E8D5, 1.0);
        c.pageAlt     = GeoHex(0xF0DDC2, 1.0);
        c.text        = GeoHex(0x17110A, 1.0);
        c.mutedText   = GeoHex(0x5D625F, 1.0);
        c.panel       = [NSColor colorWithSRGBRed:1 green:1 blue:1 alpha:82/255.0];
        c.panelTop    = [NSColor colorWithSRGBRed:1 green:1 blue:1 alpha:188/255.0];
        c.panelBottom = [NSColor colorWithSRGBRed:214/255.0 green:168/255.0 blue:79/255.0 alpha:50/255.0];
        c.field       = [NSColor colorWithSRGBRed:1 green:1 blue:1 alpha:166/255.0];
        c.fieldFocus  = [NSColor colorWithSRGBRed:252/255.0 green:252/255.0 blue:242/255.0 alpha:218/255.0];
        c.border      = [NSColor colorWithSRGBRed:151/255.0 green:126/255.0 blue:83/255.0 alpha:156/255.0];
        c.borderLight = [NSColor colorWithSRGBRed:1 green:1 blue:1 alpha:210/255.0];
        c.borderShadow= [NSColor colorWithSRGBRed:115/255.0 green:83/255.0 blue:35/255.0 alpha:65/255.0];
        c.button      = [NSColor colorWithSRGBRed:248/255.0 green:248/255.0 blue:232/255.0 alpha:128/255.0];
        c.buttonHover = [NSColor colorWithSRGBRed:239/255.0 green:198/255.0 blue:178/255.0 alpha:178/255.0];
        c.selection   = GeoHex(0x6680EB, 1.0);
        c.tabActive   = [NSColor colorWithSRGBRed:250/255.0 green:238/255.0 blue:170/255.0 alpha:170/255.0];
        c.tabInactive = [NSColor colorWithSRGBRed:248/255.0 green:232/255.0 blue:232/255.0 alpha:104/255.0];
    }
    return c;
}

@interface GeoThemeManager ()
@property (nonatomic, strong) GeoGlassColors *cached;
@property (nonatomic, strong) NSMutableArray<void (^)(NSString *)> *observers;
@end

@implementation GeoThemeManager

+ (GeoThemeManager *)shared {
    static dispatch_once_t once;
    dispatch_once(&once, ^{ gGeoThemeManager = [GeoThemeManager new]; });
    return gGeoThemeManager;
}

- (instancetype)init {
    if ((self = [super init])) {
        _observers = [NSMutableArray new];
        _persistsTheme = YES;
        NSUserDefaults *d = [NSUserDefaults standardUserDefaults];
        NSString *t = [d stringForKey:@"ui/theme"];
        _currentTheme = [t isEqualToString:@"light"] ? @"light" : @"dark";
        _cached = GeoMakeColors([_currentTheme isEqualToString:@"dark"]);
    }
    return self;
}

- (GeoGlassColors *)colors {
    return self.cached;
}

// Custom setter: the auto-synthesized one left theme observers silent, so a
// pill assigning mgr.currentTheme directly was a visual no-op until restart.
// Persistence key 'ui/theme' mirrors QSettings IniFormat [Ui] theme.
- (void)setCurrentTheme:(NSString *)theme {
    theme = [theme isEqualToString:@"light"] ? @"light" : @"dark";
    if ([_currentTheme isEqualToString:theme]) return;
    _currentTheme = [theme copy];
    _cached = GeoMakeColors([theme isEqualToString:@"dark"]);
    if (self.persistsTheme) [[NSUserDefaults standardUserDefaults] setObject:_currentTheme forKey:@"ui/theme"];
    for (void (^block)(NSString *) in [self.observers copy]) block(_currentTheme);
}

- (void)setTheme:(NSString *)theme {
    if ([self.currentTheme isEqualToString:theme]) return;
    self.currentTheme = theme;  // setter caches, persists 'ui/theme', notifies observers
}

- (id)addThemeObserver:(void (^)(NSString *))block {
    [self.observers addObject:[block copy]];
    return (id)self.observers.lastObject;
}

- (void)removeThemeObserver:(id)token {
    [self.observers removeObject:(id)token];
}

@end

NSColor *GeoThemeColor(NSString *name) {
    GeoGlassColors *c = [GeoThemeManager shared].colors;
    return [c valueForKey:name] ?: [NSColor labelColor];
}

@implementation GeoGradientView
- (instancetype)initWithFrame:(NSRect)f {
    if ((self = [super initWithFrame:f])) {
        self.wantsLayer = YES;
        self.layer.cornerRadius = 6.0;         // window cornerRadius 6 (reference shot)
        self.layer.borderWidth = 1.0;
        _logoView = [NSImageView new];
        _logoView.imageScaling = NSImageScaleProportionallyUpOrDown;
        _logoView.alphaValue = 0.30;           // coin watermark alpha 0.30
        [self addSubview:_logoView];
        [self applyTheme];
    }
    return self;
}
- (void)layout {
    [super layout];
    NSRect b = self.bounds;
    // Watermark coin: center ≈(480,330) top-left coords, diameter ≈470px.
    self.logoView.frame = NSMakeRect(480 - 235, NSMaxY(b) - 330 - 235, 470, 470);
}
- (void)drawBackgroundInRect:(NSRect)rect {
    GeoGlassColors *c = [GeoThemeManager shared].colors;
    // windowStyleSheet GeoCoinGUI: background-color c.page (flat, no gradient stops).
    NSGradient *g = [[NSGradient alloc] initWithStartingColor:c.page endingColor:c.page];
    [g drawInRect:self.bounds angle:90.0];
}
- (void)drawRect:(NSRect)dirtyRect { [self drawBackgroundInRect:dirtyRect]; }
- (void)applyTheme {
    GeoGlassColors *c = [GeoThemeManager shared].colors;
    // Gold frame: 1px #C9A24B runs along the edge of the WHOLE window.
    self.layer.borderColor = c.selection.CGColor;
    [self setNeedsDisplay:YES];
    [self.logoView.image setSize:NSMakeSize(470, 470)];
}
@end

#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 260000
API_AVAILABLE(macos(26.0))
static NSGlassEffectView *GeoGlassView(CGFloat radius, NSView *content)
{
    NSGlassEffectView *effect = [NSGlassEffectView new];
    effect.style = NSGlassEffectViewStyleClear;
    effect.cornerRadius = radius;
    effect.contentView = content;
    return effect;
}
#endif

@interface GeoGlassPanel ()
@property (nonatomic, strong) NSView *plate;
@property (nonatomic, strong) NSTextField *badge;
@property (nonatomic, strong) NSView *overlay;
@property (nonatomic, strong) CAGradientLayer *gradient;
@property (nonatomic, strong) CAShapeLayer *scrimMask;
@property (nonatomic, strong) CAGradientLayer *rimReflection;
@property (nonatomic, strong) CAShapeLayer *rimMask;
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 260000
@property (nonatomic, strong) NSGlassEffectView *glass API_AVAILABLE(macos(26.0));
@property (nonatomic, strong) NSGlassEffectView *badgeGlass API_AVAILABLE(macos(26.0));
#endif
@end

@implementation GeoGlassPanel

- (instancetype)initWithFrame:(NSRect)frame
{
    if ((self = [super initWithFrame:frame])) {
        self.plate = [NSView new];
        self.plate.wantsLayer = YES;
        self.plate.layer.cornerRadius = 10;
        self.plate.layer.masksToBounds = YES;
        self.plate.layer.borderWidth = 1;
        [self addSubview:self.plate];

#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 260000
        if (@available(macOS 26.0, *)) {
            self.glass = GeoGlassView(10, [NSView new]);
            self.glass.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
            [self.plate addSubview:self.glass];
        } else
#endif
        {
            _effect = [NSVisualEffectView new];
            _effect.material = NSVisualEffectMaterialUnderWindowBackground;
            _effect.blendingMode = NSVisualEffectBlendingModeWithinWindow;
            _effect.state = NSVisualEffectStateFollowsWindowActiveState;
            _effect.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
            [self.plate addSubview:_effect];
        }

        self.gradient = [CAGradientLayer layer];
        self.gradient.startPoint = CGPointMake(0, 0);
        self.gradient.endPoint = CGPointMake(1, 1);
        self.gradient.locations = @[@0, @0.18, @0.55, @1];
        self.gradient.actions = @{@"bounds": NSNull.null, @"position": NSNull.null,
                                  @"colors": NSNull.null};
        self.overlay = [NSView new];
        self.overlay.wantsLayer = YES;
        [self.overlay.layer addSublayer:self.gradient];
        [self.plate addSubview:self.overlay positioned:NSWindowAbove relativeTo:nil];

        self.scrimMask = [CAShapeLayer layer];
        self.scrimMask.fillColor = NSColor.blackColor.CGColor;
        self.scrimMask.shadowColor = NSColor.blackColor.CGColor;
        self.scrimMask.shadowOffset = CGSizeZero;
        self.scrimMask.shadowRadius = 4;
        self.scrimMask.shadowOpacity = 1;
        self.scrimMask.actions = @{@"bounds": NSNull.null, @"position": NSNull.null,
                                   @"path": NSNull.null, @"shadowPath": NSNull.null};

        self.rimMask = [CAShapeLayer layer];
        self.rimMask.fillColor = nil;
        self.rimMask.strokeColor = NSColor.blackColor.CGColor;
        self.rimMask.lineWidth = 2;
        self.rimMask.actions = @{@"bounds": NSNull.null, @"position": NSNull.null,
                                 @"path": NSNull.null};
        self.rimReflection = [CAGradientLayer layer];
        self.rimReflection.startPoint = CGPointMake(0, 1);
        self.rimReflection.endPoint = CGPointMake(1, 0);
        self.rimReflection.locations = @[@0, @0.25, @0.58, @0.86, @1];
        self.rimReflection.mask = self.rimMask;
        self.rimReflection.actions = @{@"bounds": NSNull.null, @"position": NSNull.null,
                                       @"colors": NSNull.null};
        [self.plate.layer addSublayer:self.rimReflection];

        self.badge = [NSTextField labelWithString:@""];
        self.badge.wantsLayer = YES;
        self.badge.layer.cornerRadius = 6;
        self.badge.layer.borderWidth = 1;
        self.badge.alignment = NSTextAlignmentCenter;
        self.badge.font = [NSFont systemFontOfSize:11 weight:NSFontWeightMedium];
        self.badge.hidden = YES;
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 260000
        if (@available(macOS 26.0, *)) {
            self.badge.hidden = NO;
            self.badge.layer.borderWidth = 0;
            self.badgeGlass = GeoGlassView(6, self.badge);
            self.badgeGlass.wantsLayer = YES;
            self.badgeGlass.layer.cornerRadius = 6;
            self.badgeGlass.layer.borderWidth = 0.8;
            self.badgeGlass.hidden = YES;
            [self addSubview:self.badgeGlass];
        } else
#endif
        {
            [self addSubview:self.badge];
        }
        [self applyTheme];
    }
    return self;
}

- (void)setSectionTitle:(NSString *)title
{
    _sectionTitle = [title copy];
    self.badge.stringValue = title ?: @"";
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 260000
    if (@available(macOS 26.0, *)) self.badgeGlass.hidden = title.length == 0;
    else
#endif
        self.badge.hidden = title.length == 0;
    self.needsLayout = YES;
}

- (void)layout
{
    [super layout];
    const NSRect bounds = self.bounds;
    CGFloat header = self.sectionTitle.length ? 20 : 0;
    self.plate.frame = NSMakeRect(0, 0, bounds.size.width, MAX(0, bounds.size.height - header));
    self.overlay.frame = self.plate.bounds;
    self.gradient.frame = self.overlay.bounds;
    const NSRect plateBounds = self.plate.bounds;
    const CGFloat inset = MIN(8, MIN(NSWidth(plateBounds), NSHeight(plateBounds)) / 4);
    self.scrimMask.frame = plateBounds;
    // Leave a clear rim to refract the backdrop, then feather into the dark center.
    CGPathRef scrim = CGPathCreateWithRoundedRect(NSInsetRect(plateBounds, inset, inset),
                                                 5, 5, NULL);
    self.scrimMask.path = scrim;
    self.scrimMask.shadowPath = scrim;
    CGPathRelease(scrim);

    self.rimReflection.frame = plateBounds;
    self.rimMask.frame = self.rimReflection.bounds;
    if (NSWidth(plateBounds) > 4 && NSHeight(plateBounds) > 4) {
        CGPathRef rim = CGPathCreateWithRoundedRect(NSInsetRect(plateBounds, 2, 2), 8, 8, NULL);
        self.rimMask.path = rim;
        CGPathRelease(rim);
    } else {
        self.rimMask.path = nil;
    }
    NSRect badgeFrame = NSMakeRect(15, bounds.size.height - 18,
                                   self.badge.intrinsicContentSize.width + 18, 17);
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 260000
    if (@available(macOS 26.0, *)) {
        self.glass.frame = self.plate.bounds;
        self.badgeGlass.frame = badgeFrame;
    } else
#endif
    {
        _effect.frame = self.plate.bounds;
        self.badge.frame = badgeFrame;
    }
}

- (void)applyTheme
{
    GeoThemeManager *manager = [GeoThemeManager shared];
    GeoGlassColors *c = manager.colors;
    const BOOL dark = [manager.currentTheme isEqualToString:@"dark"];
    BOOL solid = NSWorkspace.sharedWorkspace.accessibilityDisplayShouldReduceTransparency;
    self.plate.layer.borderColor = [c.borderLight colorWithAlphaComponent:dark ? 0.54 : 0.76].CGColor;
    self.rimReflection.hidden = solid;
    self.rimReflection.colors = @[
        (id)[c.borderLight colorWithAlphaComponent:dark ? 0.38 : 0.61].CGColor,
        (id)[c.borderLight colorWithAlphaComponent:dark ? 0.14 : 0.21].CGColor,
        (id)NSColor.clearColor.CGColor,
        (id)[c.border colorWithAlphaComponent:dark ? 0.20 : 0.19].CGColor,
        (id)[c.borderLight colorWithAlphaComponent:dark ? 0.26 : 0.45].CGColor
    ];
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 260000
    if (@available(macOS 26.0, *)) {
        self.glass.tintColor = [c.page colorWithAlphaComponent:dark ? 0.08 : 0.16];
        self.glass.alphaValue = dark ? 1 : 0.86;
        self.glass.hidden = solid;
        self.badgeGlass.tintColor = [c.page colorWithAlphaComponent:dark ? 0.24 : 0.16];
        self.badgeGlass.layer.borderColor = [c.borderLight colorWithAlphaComponent:dark ? 0.59 : 0.76].CGColor;
    }
#endif
    _effect.hidden = solid;
    self.overlay.layer.mask = dark && !solid ? self.scrimMask : nil;
    NSColor *top = solid ? [c.panelTop colorWithAlphaComponent:1] : c.panelTop;
    NSColor *mid = solid ? [c.panel colorWithAlphaComponent:1] : c.panel;
    NSColor *bottom = solid ? [c.panelBottom colorWithAlphaComponent:1] : c.panelBottom;
    self.gradient.colors = @[(id)top.CGColor, (id)mid.CGColor, (id)mid.CGColor, (id)bottom.CGColor];
    self.badge.textColor = c.text;
    self.badge.layer.backgroundColor = (dark ? GeoHex(0x090D13, 160/255.0) : GeoHex(0xFFFCF2, 135/255.0)).CGColor;
    self.badge.layer.borderColor = c.border.CGColor;
}

@end

@implementation GeoPillButton {
    NSTrackingArea *_tracking;
    BOOL _hovered;
}
- (instancetype)initWithFrame:(NSRect)frame
{
    if ((self = [super initWithFrame:frame])) {
        self.wantsLayer = YES;
        self.bordered = NO;
        self.translatesAutoresizingMaskIntoConstraints = NO;
        self.layer.cornerRadius = 7;
        self.layer.borderWidth = 1;
        self.font = [NSFont systemFontOfSize:12 weight:NSFontWeightSemibold];
        self.target = self;
        self.action = @selector(geoPillTapped);
        [self setContentCompressionResistancePriority:NSLayoutPriorityRequired
                                        forOrientation:NSLayoutConstraintOrientationHorizontal];
        [self applyTheme];
    }
    return self;
}

- (NSSize)intrinsicContentSize
{
    NSSize original = [super intrinsicContentSize];
    CGFloat padding = self.usesSystemGlass ? 0 : (self.title.length ? 15 : 4);
    return NSMakeSize(MAX(25, original.width + padding), MAX(27, original.height + 7));
}

- (void)setTitle:(NSString *)title
{
    [super setTitle:title];
    [self applyTheme];
    [self invalidateIntrinsicContentSize];
}

- (void)setActive:(BOOL)active
{
    _active = active;
    [self applyTheme];
}

- (void)setEnabled:(BOOL)enabled
{
    [super setEnabled:enabled];
    [self applyTheme];
}

- (void)setUsesSystemGlass:(BOOL)usesSystemGlass
{
    _usesSystemGlass = usesSystemGlass;
    [self applyTheme];
    [self invalidateIntrinsicContentSize];
}

- (void)updateTrackingAreas
{
    [super updateTrackingAreas];
    if (_tracking) [self removeTrackingArea:_tracking];
    _tracking = [[NSTrackingArea alloc] initWithRect:NSZeroRect
        options:NSTrackingMouseEnteredAndExited | NSTrackingActiveInActiveApp | NSTrackingInVisibleRect
        owner:self userInfo:nil];
    [self addTrackingArea:_tracking];
}

- (void)mouseEntered:(NSEvent *)event { _hovered = YES; [self applyTheme]; }
- (void)mouseExited:(NSEvent *)event { _hovered = NO; [self applyTheme]; }

- (void)applyTheme
{
    GeoThemeManager *theme = [GeoThemeManager shared];
    GeoGlassColors *c = theme.colors;
    const BOOL dark = [theme.currentTheme isEqualToString:@"dark"];
    const BOOL hovered = _hovered && self.enabled;
    NSColor *background = hovered ? c.buttonHover
        : self.plainMenu ? NSColor.clearColor
        : self.active ? c.tabActive : c.button;
    BOOL showsSystemGlass = NO;
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 260000
    if (@available(macOS 26.0, *)) {
        showsSystemGlass = self.usesSystemGlass;
        if (showsSystemGlass) {
            if (self.bezelStyle != NSBezelStyleGlass) self.bezelStyle = NSBezelStyleGlass;
            if (!self.isBordered) self.bordered = YES;
            self.bezelColor = self.active ? [c.border colorWithAlphaComponent:0.47]
                : hovered ? [c.border colorWithAlphaComponent:0.26]
                : [c.page colorWithAlphaComponent:dark ? 0.22 : 0.17];
        }
    }
#endif
    if (!showsSystemGlass && self.isBordered) self.bordered = NO;
    self.layer.backgroundColor = (showsSystemGlass ? NSColor.clearColor : background).CGColor;
    self.layer.borderWidth = self.plainMenu ? 0 : showsSystemGlass ? 0.85 : 1;
    self.layer.borderColor = [c.borderLight colorWithAlphaComponent:self.active ? 0.92
        : showsSystemGlass && hovered ? 0.78 : 0.55].CGColor;
    NSColor *text = self.enabled ? c.text : c.mutedText;
    self.contentTintColor = text;
    if (self.title.length) {
        NSMutableParagraphStyle *style = [NSMutableParagraphStyle new];
        style.alignment = self.alignment;
        self.attributedTitle = [[NSAttributedString alloc] initWithString:self.title
            attributes:@{NSForegroundColorAttributeName: text,
                         NSFontAttributeName: self.font ?: [NSFont systemFontOfSize:12],
                         NSParagraphStyleAttributeName: style}];
    }
}

- (void)geoPillTapped
{
    if (self.geoAction) self.geoAction();
}
@end

@implementation GeoFieldCell
- (void)drawWithFrame:(NSRect)cellFrame inView:(NSView *)controlView
{
    GeoGlassColors *c = [GeoThemeManager shared].colors;
    NSBezierPath *shape = [NSBezierPath bezierPathWithRoundedRect:NSInsetRect(cellFrame, 0.5, 0.5)
                                                          xRadius:6 yRadius:6];
    [c.field setFill];
    [shape fill];
    [[c.borderLight colorWithAlphaComponent:0.65] setStroke];
    [shape stroke];

    NSRect textFrame = NSInsetRect(cellFrame, 8, 0);
    CGFloat textHeight = MIN(NSHeight(textFrame), self.cellSize.height + 1);
    textFrame.origin.y += (NSHeight(textFrame) - textHeight) * 0.5;
    textFrame.size.height = textHeight;
    [NSGraphicsContext saveGraphicsState];
    [shape addClip];
    [super drawWithFrame:textFrame inView:controlView];
    [NSGraphicsContext restoreGraphicsState];
}
@end

@implementation GeoField
- (instancetype)initWithFrame:(NSRect)f {
    if ((self = [super initWithFrame:f])) {
        GeoFieldCell *cell = [GeoFieldCell new];
        [self setCell:cell];
        self.bezeled = NO;
        self.drawsBackground = NO;
        self.wantsLayer = YES;
        self.layer.cornerRadius = 6.0;           // field radius 6 (reference shot)
        self.font = [NSFont userFixedPitchFontOfSize:10.0]; // monospace 10px
    }
    return self;
}
- (void)applyTheme {
    GeoGlassColors *c = [GeoThemeManager shared].colors;
    self.textColor = c.text;
    self.backgroundColor = c.field;
    self.needsDisplay = YES;
}
@end

@implementation GeoGroupBox
- (instancetype)initWithFrame:(NSRect)f {
    if ((self = [super initWithFrame:f])) {
        self.boxType = NSBoxCustom;
        self.cornerRadius = 8.0;      // card radius 8 (reference shot)
        self.borderWidth = 1.0;
        self.titlePosition = NSAtTop;
        self.contentViewMargins = NSMakeSize(12, 12);   // card padding 12
        [self applyTheme];
    }
    return self;
}
- (void)applyTheme {
    GeoGlassColors *c = [GeoThemeManager shared].colors;
    self.fillColor = c.panel;
    self.borderColor = c.border;
    self.titleFont = [NSFont systemFontOfSize:9.0 weight:NSFontWeightRegular]; // mini-pill 9px
    self.titleColor = c.mutedText;                                              // #9AA3B2
}
@end
