// GeoCoinSendCoins — native mirror of src/qt/sendcoinspage (Workstream D).
#import <AppKit/AppKit.h>
#import "GeoCoinCoreBridge.h"
NS_ASSUME_NONNULL_BEGIN
@interface GeoCoinSendCoinsPage : NSView
- (instancetype)initWithCore:(GeoCoinCoreBridge*)core;
- (void)applyTheme;
@end
NS_ASSUME_NONNULL_END
