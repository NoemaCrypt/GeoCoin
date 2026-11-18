#import <AppKit/AppKit.h>

NS_ASSUME_NONNULL_BEGIN

@interface GeoCoinMetalBackdrop : NSView
@property (nonatomic, readonly, getter=isUsingMetal) BOOL usingMetal;

// Insets and title bar metrics are expressed in logical points from the window edges.
- (void)setArtwork:(nullable NSImage*)artwork dark:(BOOL)dark;
- (void)setHeaderHeight:(CGFloat)headerHeight
        artworkTopInset:(CGFloat)topInset
     artworkBottomInset:(CGFloat)bottomInset;
- (void)requestFrame;
@end

NS_ASSUME_NONNULL_END
