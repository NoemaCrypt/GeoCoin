// GeoCoinIntroSheet — stable-subset stand-in for src/qt/intro.cpp dialog
// (Intro::pickDataDirectory NSOpenPanel mirror; richer sheet lands with D).
#import <AppKit/AppKit.h>
NS_ASSUME_NONNULL_BEGIN
@interface GeoCoinIntroSheet : NSObject
@property (nonatomic, copy, nullable) NSString* dataDirectory;
- (void)presentAsModalForWindow:(NSWindow*)window;
@end
NS_ASSUME_NONNULL_END
