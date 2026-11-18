#import "GeoCoinReceiveCoinsPage.h"
#import "GeoCoinPageKit.h"
#import <CoreImage/CoreImage.h>

static NSImage* GeoReceiveQr(NSString* address)
{
    if (!address.length) return nil;
    CIFilter* filter = [CIFilter filterWithName:@"CIQRCodeGenerator"];
    [filter setValue:[address dataUsingEncoding:NSUTF8StringEncoding] forKey:@"inputMessage"];
    CIImage* code = filter.outputImage;
    if (!code) return nil;
    CGRect extent = CGRectInset(code.extent, -4, -4);
    CIImage* background = [[CIImage imageWithColor:CIColor.whiteColor] imageByCroppingToRect:extent];
    CIImage* padded = [code imageByCompositingOverImage:background];
    padded = [padded imageByApplyingTransform:CGAffineTransformMakeScale(6, 6)];
    static CIContext* context;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ context = [CIContext context]; });
    CGImageRef cg = [context createCGImage:padded fromRect:padded.extent];
    if (!cg) return nil;
    NSImage* image = [[NSImage alloc] initWithCGImage:cg size:NSZeroSize];
    CGImageRelease(cg);
    return image;
}

@implementation GeoCoinReceiveCoinsPage

- (instancetype)initWithCore:(GeoCoinCoreBridge*)core
{
    if ((self = [super initWithFrame:NSZeroRect])) {
        self.identifier = @"receive";
        NSStackView* body;
        NSView* page = GeoMakePage([core localized:@"Receive Coins" context:@"ReceiveCoinsPage"], &body);
        [self addSubview:page];
        [NSLayoutConstraint activateConstraints:@[
            [page.topAnchor constraintEqualToAnchor:self.topAnchor],
            [page.bottomAnchor constraintEqualToAnchor:self.bottomAnchor],
            [page.leadingAnchor constraintEqualToAnchor:self.leadingAnchor],
            [page.trailingAnchor constraintEqualToAnchor:self.trailingAnchor]
        ]];
        NSStackView* inner;
        GeoGlassPanel* card = GeoMakeGroup([core localized:@"Your Address" context:@"ReceiveCoinsPage"],
                                           &inner);
        NSTextField* address = GeoMakeField(@"");
        address.editable = NO;
        address.selectable = YES;
        [address setAccessibilityLabel:[core localized:@"Your Address" context:@"ReceiveCoinsPage"]];
        GeoPillButton* copy = GeoMakePill([core localized:@"Copy Address" context:@"ReceiveCoinsPage"], ^{
            if (!address.stringValue.length) return;
            [[NSPasteboard generalPasteboard] clearContents];
            [[NSPasteboard generalPasteboard] setString:address.stringValue forType:NSPasteboardTypeString];
        });
        NSStackView* addrRow = GeoMakeRow(6);
        [addrRow addArrangedSubview:address];
        [addrRow addArrangedSubview:copy];
        [inner addArrangedSubview:addrRow];
        [body addArrangedSubview:card];

        NSStackView* qrBody;
        GeoGlassPanel* qr = GeoMakeGroup(@"QR Code", &qrBody);
        NSImageView* image = [NSImageView new];
        image.imageScaling = NSImageScaleProportionallyUpOrDown;
        image.translatesAutoresizingMaskIntoConstraints = NO;
        [image.widthAnchor constraintEqualToConstant:210].active = YES;
        [image.heightAnchor constraintEqualToConstant:210].active = YES;
        [image setAccessibilityLabel:@"GeoCoin address QR code"];
        NSTextField* hint = GeoMakeLabel([core localized:@"No wallet loaded" context:@"ReceiveCoinsPage"],
                                         [GeoThemeManager shared].colors.text, 13);
        [qrBody addArrangedSubview:image];
        [qrBody addArrangedSubview:hint];
        [body addArrangedSubview:qr];

        __weak NSTextField* weakAddress = address;
        __weak NSImageView* weakImage = image;
        __weak NSTextField* weakHint = hint;
        __weak GeoPillButton* weakCopy = copy;
        [core observeAddress:^(NSString* value) {
            weakAddress.stringValue = value;
            weakCopy.enabled = value.length != 0;
            weakImage.image = GeoReceiveQr(value);
            weakImage.hidden = !weakImage.image;
            weakHint.hidden = weakImage.image != nil;
        }];
    }
    return self;
}

- (void)applyTheme {}

@end
