// GeoCoinReceiveCoins — native mirror of src/qt/receivecoinspage (Workstream D).
#import <AppKit/AppKit.h>
#import "GeoCoinCoreBridge.h"
NS_ASSUME_NONNULL_BEGIN
@interface GeoCoinReceiveCoinsPage : NSView
- (instancetype)initWithCore:(GeoCoinCoreBridge*)core;
- (void)applyTheme;
@end
NS_ASSUME_NONNULL_END
