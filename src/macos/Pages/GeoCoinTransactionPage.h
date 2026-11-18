// GeoCoinTransaction — native mirror of src/qt/transactionpage (Workstream D).
#import <AppKit/AppKit.h>
#import "GeoCoinCoreBridge.h"
NS_ASSUME_NONNULL_BEGIN
@interface GeoCoinTransactionPage : NSView
- (instancetype)initWithCore:(GeoCoinCoreBridge*)core;
- (void)applyTheme;
@end
NS_ASSUME_NONNULL_END
