// AppKit controls for the Core CPU and GPU mining sessions.
#import <AppKit/AppKit.h>
#import "GeoCoinCoreBridge.h"
NS_ASSUME_NONNULL_BEGIN
@interface GeoCoinMiningPage : NSView
- (instancetype)initWithCore:(GeoCoinCoreBridge*)core;
- (void)applyTheme;
- (void)applyLanguage;
/// Mirrors MiningPage::updateMiningStatus (CPU row of the Qt mining page).
- (void)updateCpuEnabled:(BOOL)enabled threads:(NSInteger)threads;
@end
NS_ASSUME_NONNULL_END
