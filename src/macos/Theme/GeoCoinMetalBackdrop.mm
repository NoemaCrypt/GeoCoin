#import "GeoCoinMetalBackdrop.h"
#import <Metal/Metal.h>
#import <MetalKit/MetalKit.h>
#import <QuartzCore/QuartzCore.h>

#include <simd/simd.h>

namespace {
struct Scene {
    simd_float2 size;
    float scale;
    float artworkTop;
    float artworkBottom;
    float hasArtwork;
    float goldGain;
    float reserved;
    simd_float4 page;
};

constexpr char shaderSource[] = R"MSL(
#include <metal_stdlib>
using namespace metal;

struct Scene {
    float2 size;
    float scale;
    float artworkTop;
    float artworkBottom;
    float hasArtwork;
    float goldGain;
    float reserved;
    float4 page;
};

vertex float4 backdropVertex(uint vertexID [[vertex_id]]) {
    const float2 positions[] = {float2(-1, -1), float2(3, -1), float2(-1, 3)};
    return float4(positions[vertexID], 0, 1);
}

fragment float4 backdropFragment(float4 position [[position]],
                                  constant Scene& scene [[buffer(0)]],
                                  texture2d<float> artwork [[texture(0)]]) {
    const float2 point = position.xy / scene.scale;
    constexpr sampler imageSampler(coord::normalized, address::clamp_to_edge, filter::linear);
    float4 color = scene.page;
    const float side = min(1024.0, min(scene.size.x * 0.82, scene.size.y * 1.03));
    const float2 imageCenter(scene.size.x * 0.5, scene.size.y * 0.52);
    const float2 uv = (point - imageCenter) / side + 0.5;
    if (scene.hasArtwork > 0 && point.y >= scene.artworkTop &&
        point.y <= scene.size.y - scene.artworkBottom &&
        all(uv >= 0.0) && all(uv <= 1.0)) {
        const float4 sample = artwork.sample(imageSampler, uv);
        const float warmth = smoothstep(0.10, 0.42, sample.r - sample.b);
        const float light = smoothstep(0.32, 0.78, sample.r);
        const float3 coin = saturate(sample.rgb * (1.0 + scene.goldGain * warmth * light));
        color.rgb = mix(color.rgb, coin, sample.a);
    }

    return float4(color.rgb, 1);
}
)MSL";

simd_float4 colorVector(NSColor* color)
{
    return {(float)color.redComponent, (float)color.greenComponent,
            (float)color.blueComponent, (float)color.alphaComponent};
}

NSColor* colorFromRGB(unsigned rgb)
{
    return [NSColor colorWithSRGBRed:((rgb >> 16) & 255) / 255.0
                               green:((rgb >> 8) & 255) / 255.0
                                blue:(rgb & 255) / 255.0
                               alpha:1];
}
}  // namespace

@implementation GeoCoinMetalBackdrop {
    CAMetalLayer* _surface;
    CALayer* _outline;
    id<MTLCommandQueue> _queue;
    id<MTLRenderPipelineState> _pipeline;
    MTKTextureLoader* _loader;
    NSMapTable<NSImage*, id<MTLTexture>>* _textureCache;
    id<MTLTexture> _texture;
    id<MTLTexture> _emptyTexture;
    NSImage* _artwork;
    NSColor* _pageColor;
    CGFloat _artworkTop;
    CGFloat _artworkBottom;
    BOOL _dark;
    BOOL _dirty;
    BOOL _scheduled;
    BOOL _inFlight;
}

- (instancetype)initWithFrame:(NSRect)frame
{
    if ((self = [super initWithFrame:frame])) {
        self.wantsLayer = YES;
        [self setupMetal];
        _outline = [CALayer layer];
        _outline.cornerCurve = kCACornerCurveContinuous;
        _outline.actions = @{@"bounds": NSNull.null, @"position": NSNull.null,
            @"cornerRadius": NSNull.null, @"borderWidth": NSNull.null,
            @"borderColor": NSNull.null, @"contentsScale": NSNull.null};
        [self.layer addSublayer:_outline];
        [self setArtwork:nil dark:YES];
    }
    return self;
}

- (BOOL)isUsingMetal { return _pipeline != nil; }
- (BOOL)isFlipped { return YES; }
- (NSView*)hitTest:(NSPoint)point { return nil; }

- (void)setupMetal
{
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device) return;
    NSError* error = nil;
    NSString* source = [NSString stringWithUTF8String:shaderSource];
    id<MTLLibrary> library = [device newLibraryWithSource:source options:nil error:&error];
    if (!library) {
        NSLog(@"GeoCoin backdrop Metal library: %@", error.localizedDescription);
        return;
    }
    MTLRenderPipelineDescriptor* state = [MTLRenderPipelineDescriptor new];
    state.vertexFunction = [library newFunctionWithName:@"backdropVertex"];
    state.fragmentFunction = [library newFunctionWithName:@"backdropFragment"];
    state.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    id<MTLRenderPipelineState> pipeline = [device newRenderPipelineStateWithDescriptor:state error:&error];
    if (!pipeline) {
        NSLog(@"GeoCoin backdrop Metal pipeline: %@", error.localizedDescription);
        return;
    }
    _queue = [device newCommandQueue];
    MTLTextureDescriptor* blank = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:1 height:1 mipmapped:NO];
    _emptyTexture = [device newTextureWithDescriptor:blank];
    if (!_queue || !_emptyTexture) return;
    _pipeline = pipeline;
    _loader = [[MTKTextureLoader alloc] initWithDevice:device];
    _textureCache = [NSMapTable weakToStrongObjectsMapTable];

    _surface = [CAMetalLayer layer];
    _surface.device = device;
    _surface.pixelFormat = state.colorAttachments[0].pixelFormat;
    _surface.framebufferOnly = YES;
    _surface.opaque = YES;
    _surface.actions = @{@"bounds": NSNull.null, @"position": NSNull.null,
                         @"contentsScale": NSNull.null};
    CGColorSpaceRef colorSpace = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    _surface.colorspace = colorSpace;
    CGColorSpaceRelease(colorSpace);
    [self.layer addSublayer:_surface];
}

- (void)setArtwork:(NSImage*)artwork dark:(BOOL)dark
{
    _artwork = artwork;
    _dark = dark;
    _pageColor = colorFromRGB(dark ? 0x080A0D : 0xF7E8D5);
    _outline.borderColor = colorFromRGB(dark ? 0xE0B35B : 0x977E53).CGColor;
    _texture = artwork ? [_textureCache objectForKey:artwork] : nil;
    if (artwork && !_texture && _loader) {
        CGImageRef image = [artwork CGImageForProposedRect:NULL context:nil hints:nil];
        if (image) {
            NSError* error = nil;
            _texture = [_loader newTextureWithCGImage:image
                options:@{MTKTextureLoaderOptionSRGB: @NO,
                          MTKTextureLoaderOptionTextureUsage: @(MTLTextureUsageShaderRead)}
                  error:&error];
            if (_texture) [_textureCache setObject:_texture forKey:artwork];
            else NSLog(@"GeoCoin backdrop texture: %@", error.localizedDescription);
        }
    }
    [self requestFrame];
}

- (void)setHeaderHeight:(CGFloat)headerHeight artworkTopInset:(CGFloat)topInset
     artworkBottomInset:(CGFloat)bottomInset
{
    topInset = MAX(topInset, headerHeight);
    if (_artworkTop == topInset && _artworkBottom == bottomInset) return;
    _artworkTop = topInset;
    _artworkBottom = bottomInset;
    [self requestFrame];
}

- (void)layout
{
    [super layout];
    [self resizeSurface];
}

- (void)viewDidChangeBackingProperties
{
    [super viewDidChangeBackingProperties];
    [self resizeSurface];
}

- (void)viewDidMoveToWindow
{
    [super viewDidMoveToWindow];
    [self resizeSurface];
}

- (void)resizeSurface
{
    CGFloat scale = self.window.backingScaleFactor;
    if (scale <= 0) scale = 1;
    CGSize size = CGSizeMake(ceil(self.bounds.size.width * scale), ceil(self.bounds.size.height * scale));
    [self layoutOutlineAtScale:scale];
    _surface.frame = self.bounds;
    if (!CGSizeEqualToSize(_surface.drawableSize, size) || _surface.contentsScale != scale) {
        _surface.contentsScale = scale;
        if (size.width > 0 && size.height > 0) _surface.drawableSize = size;
        [self requestFrame];
    }
    if (!_surface) self.needsDisplay = YES;
}

- (void)layoutOutlineAtScale:(CGFloat)scale
{
    const CGFloat inset = 1 / scale;
    _outline.frame = NSInsetRect(self.bounds, inset, inset);
    _outline.contentsScale = scale;
    _outline.borderWidth = 1;

    // The outline sits one backing pixel inside AppKit's continuous clipping
    // path. Its footprint follows the larger corners on macOS 26.
    CGFloat radius = 10;
    if (@available(macOS 26.0, *)) radius = 16.5;
    _outline.cornerRadius = radius - inset;
    _outline.hidden = (self.window.styleMask & NSWindowStyleMaskFullScreen) != 0;
}

- (void)requestFrame
{
    if (!_surface) {
        self.needsDisplay = YES;
        return;
    }
    _dirty = YES;
    if (_scheduled || _inFlight) return;
    _scheduled = YES;
    __weak GeoCoinMetalBackdrop* weakSelf = self;
    dispatch_async(dispatch_get_main_queue(), ^{
        [weakSelf render];
    });
}

- (void)render
{
    _scheduled = NO;
    if (!_dirty || _inFlight || !self.window.isVisible || NSIsEmptyRect(self.bounds)) return;
    id<CAMetalDrawable> drawable = [_surface nextDrawable];
    if (!drawable) return;
    id<MTLCommandBuffer> command = [_queue commandBuffer];
    if (!command) return;

    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = drawable.texture;
    pass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> encoder = [command renderCommandEncoderWithDescriptor:pass];
    if (!encoder) return;
    const Scene scene = {
        {(float)self.bounds.size.width, (float)self.bounds.size.height},
        (float)_surface.contentsScale, (float)_artworkTop, (float)_artworkBottom,
        _texture ? 1.0f : 0.0f, _dark ? 0.13f : 0.0f, 0,
        colorVector(_pageColor)
    };
    [encoder setRenderPipelineState:_pipeline];
    [encoder setFragmentBytes:&scene length:sizeof(scene) atIndex:0];
    [encoder setFragmentTexture:(_texture ?: _emptyTexture) atIndex:0];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [encoder endEncoding];
    [command presentDrawable:drawable];

    _dirty = NO;
    _inFlight = YES;
    __weak GeoCoinMetalBackdrop* weakSelf = self;
    [command addCompletedHandler:^(id<MTLCommandBuffer> completed) {
        const BOOL failed = completed.status == MTLCommandBufferStatusError;
        if (failed) NSLog(@"GeoCoin backdrop frame: %@", completed.error.localizedDescription);
        dispatch_async(dispatch_get_main_queue(), ^{
            [weakSelf finishFrameWithError:failed];
        });
    }];
    [command commit];
}

- (void)finishFrameWithError:(BOOL)failed
{
    _inFlight = NO;
    if (failed) {
        [_surface removeFromSuperlayer];
        _surface = nil;
        _pipeline = nil;
        self.needsDisplay = YES;
    } else if (_dirty) {
        [self requestFrame];
    }
}

- (void)drawRect:(NSRect)dirtyRect
{
    if (_surface) return;
    [_pageColor setFill];
    NSRectFill(self.bounds);
    const NSSize size = self.bounds.size;
    if (_artwork) {
        CGFloat side = MIN(1024.0, MIN(size.width * 0.82, size.height * 1.03));
        [NSGraphicsContext saveGraphicsState];
        NSRectClip(NSMakeRect(0, _artworkTop, size.width,
                             MAX(0, size.height - _artworkTop - _artworkBottom)));
        [_artwork drawInRect:NSMakeRect((size.width - side) * 0.5, size.height * 0.52 - side * 0.5,
                                       side, side)
                   fromRect:NSZeroRect operation:NSCompositingOperationSourceOver
                   fraction:1 respectFlipped:YES hints:nil];
        [NSGraphicsContext restoreGraphicsState];
    }
}
@end
