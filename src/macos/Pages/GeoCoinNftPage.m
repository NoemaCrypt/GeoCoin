#import "GeoCoinNftPage.h"

@implementation GeoCoinNftPage

- (instancetype)initWithCore:(GeoCoinCoreBridge*)core
{
    (void)core;
    if ((self = [super initWithFrame:NSZeroRect])) self.identifier = @"nft";
    return self;
}

- (void)applyTheme {}

@end
