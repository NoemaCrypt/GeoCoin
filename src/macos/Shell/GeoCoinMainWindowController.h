#import <AppKit/AppKit.h>
#import "GeoCoinCoreBridge.h"

NS_ASSUME_NONNULL_BEGIN

@interface GeoCoinMainWindowController : NSObject
@property (nonatomic, strong, readonly) NSWindow* window;
@property (nonatomic, strong, readonly) GeoCoinCoreBridge* core;
@property (nonatomic, copy, nullable) NSString* titleAddText;
@property (nonatomic, copy, nullable) void (^onMenuAction)(NSString* command);
@property (nonatomic, copy, nullable) void (^onPageChanged)(NSString* pageId);
@property (nonatomic, readonly, getter=isUsingMetal) BOOL usingMetal;

- (instancetype)initWithCore:(GeoCoinCoreBridge*)core;
- (void)show;
- (nullable NSView*)pageById:(NSString*)pageId;
- (void)gotoPageById:(NSString*)pageId;
- (void)applyTheme;
- (void)applyLanguage;
- (void)updateGpuHashrate:(double)hashrate;
@end

NS_ASSUME_NONNULL_END
