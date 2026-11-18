// GeoCoinCorporatePage — stable-subset placeholder page (full mirror lands with Workstream D).
#import <AppKit/AppKit.h>
#import "GeoCoinCoreBridge.h"
NS_ASSUME_NONNULL_BEGIN
@interface GeoCoinCorporatePage : NSView
- (instancetype)initWithCore:(GeoCoinCoreBridge*)core;
- (void)applyTheme;
@end
NS_ASSUME_NONNULL_END
