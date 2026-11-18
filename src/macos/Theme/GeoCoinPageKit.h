// GeoCoinPageKit — shared layout helpers for the native AppKit pages.
#import <AppKit/AppKit.h>
#import "GeoCoinTheme.h"

NS_ASSUME_NONNULL_BEGIN

/** Page/tab strip stack — the AppKit alias of the Qt QBoxLayout containers
 *  (QVBoxLayout in the pages, QHBoxLayout in GeoCoinGUI's tab strip). */
typedef NSStackView GeoStack;

/** QBoxLayout equivalent: spacing 8px (page/column gap in geo.css metrics),
 *  fill distribution, translatesAutoresizingMaskIntoConstraints = NO. */
GeoStack* GeoMakeStack(CGFloat spacing);
GeoStack* GeoMakeRow(CGFloat spacing);

/** Plain label with the theme text color, 13pt regular. */
NSTextField* GeoMakeLabel(NSString* text, NSColor* _Nullable color, CGFloat size);

/** Single-line input styled with glassInputStyleSheet metrics (field/border/radii). */
NSTextField* GeoMakeField(NSString* _Nullable placeholder);

/** Glass panel with a bold group title on top and a vertical body stack.
 *  Mirrors glassPanelStyleSheet QGroupBox: radius 12, padding 28px 14px 14px 14px. */
GeoGlassPanel* GeoMakeGroup(NSString* _Nullable title, NSStackView* _Nonnull __autoreleasing * _Nullable outBody);

/** Pill button (QToolButton radius 8, min-width 80, height 29) wired to a block. */
GeoPillButton* GeoMakePill(NSString* title, void (^_Nullable action)(void));

/** Vertical page scaffold: title label on top, body stack below. */
NSView* GeoMakePage(NSString* title, NSStackView* _Nonnull __autoreleasing * _Nullable outBody);

/** Full width (QSizePolicy::Expanding equivalent). */
void GeoFill(NSView* view);

/** Apply the current palette to the themed controls in this view tree. */
void GeoApplyThemeToView(NSView* view);

NS_ASSUME_NONNULL_END
