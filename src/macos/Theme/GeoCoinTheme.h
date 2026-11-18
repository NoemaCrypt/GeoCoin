// GeoCoinTheme — native AppKit mirror of src/qt/guiutil.cpp glass theme tokens.
#import <AppKit/AppKit.h>

NS_ASSUME_NONNULL_BEGIN

/// Palette shared by the AppKit window, pages and reusable controls.
typedef struct {
    CGFloat r, g, b, a;
} GeoRGBA;

@interface GeoGlassColors : NSObject
@property (nonatomic, strong) NSColor *page;
@property (nonatomic, strong) NSColor *pageAlt;
@property (nonatomic, strong) NSColor *text;
@property (nonatomic, strong) NSColor *mutedText;
@property (nonatomic, strong) NSColor *panel;
@property (nonatomic, strong) NSColor *panelTop;
@property (nonatomic, strong) NSColor *panelBottom;
@property (nonatomic, strong) NSColor *field;
@property (nonatomic, strong) NSColor *fieldFocus;
@property (nonatomic, strong) NSColor *border;
@property (nonatomic, strong) NSColor *borderLight;
@property (nonatomic, strong) NSColor *borderShadow;
@property (nonatomic, strong) NSColor *button;
@property (nonatomic, strong) NSColor *buttonHover;
@property (nonatomic, strong) NSColor *selection;
@property (nonatomic, strong) NSColor *tabActive;
@property (nonatomic, strong) NSColor *tabInactive;
@end

/// Theme registry. currentTheme key mirrors QSettings 'ui/theme' (dark|light),
/// persisted through NSUserDefaults under key 'ui/theme' (QSettings IniFormat
/// compatible section/key pair: [Ui] theme).
@interface GeoThemeManager : NSObject
@property (class, readonly, nonatomic) GeoThemeManager *shared;
@property (nonatomic, copy) NSString *currentTheme;
@property (nonatomic, assign) BOOL persistsTheme;
- (GeoGlassColors *)colors;
- (void)setTheme:(NSString *)theme;
/// Block-observer (no NSDistributedNotificationCenter): called on main queue.
- (id)addThemeObserver:(void (^)(NSString *theme))block;
- (void)removeThemeObserver:(id)token;
@end

/// Section with a tinted Liquid Glass plate and a title badge.
@interface GeoGlassPanel : NSView
@property (nonatomic, readonly, strong, nullable) NSVisualEffectView *effect;
@property (nonatomic, copy, nullable) NSString *sectionTitle;
- (void)applyTheme;
@end

/// AppKit button used for navigation, menus and page actions.
@interface GeoPillButton : NSButton
@property (nonatomic, assign) BOOL active;
@property (nonatomic, assign) BOOL plainMenu;
@property (nonatomic, assign) BOOL usesSystemGlass;
@property (nonatomic, copy, nullable) void (^geoAction)(void);
- (void)geoPillTapped;
- (void)applyTheme;
@end

/// Field background/border mirroring glassInputStyleSheet (radius 8, padding 6px 8px,
/// focus border GoldLight/#FFEDF3, focus 2px, focus bg fieldFocus).
@interface GeoFieldCell : NSTextFieldCell
@end

@interface GeoField : NSTextField
- (void)applyTheme;
@end

/// Gradient-backed view for the window content (page -> pageAlt vertical).
@interface GeoGradientView : NSView
@property (nonatomic, strong, nullable) NSImageView *logoView;
- (void)applyTheme;
@end

/// Section group box (NSBox titled) mirroring glassPanelStyleSheet group styling.
@interface GeoGroupBox : NSBox
@property (nonatomic, strong, nullable) NSColor *titleColor;
- (void)applyTheme;
@end

/// Small helper: resolve a color for the current theme.
NSColor *GeoThemeColor(NSString *name);

NS_ASSUME_NONNULL_END
