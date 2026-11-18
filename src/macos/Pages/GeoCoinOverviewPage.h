#import <AppKit/AppKit.h>
#import "GeoCoinCoreBridge.h"

NS_ASSUME_NONNULL_BEGIN

@interface GeoCoinOverviewPage : NSView
@property (nonatomic, copy, nullable) void (^onNavigate)(NSString* pageId);
- (instancetype)initWithCore:(GeoCoinCoreBridge*)core;
- (void)applyTheme;
- (void)applyLanguage;
- (void)setWalletName:(nullable NSString*)name loaded:(BOOL)loaded;
- (void)setBlockCount:(NSInteger)count;
- (void)setConnectionCount:(NSInteger)count;
@end

NS_ASSUME_NONNULL_END
