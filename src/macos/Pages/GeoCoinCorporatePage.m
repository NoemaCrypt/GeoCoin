#import "GeoCoinCorporatePage.h"

@implementation GeoCoinCorporatePage

- (instancetype)initWithCore:(GeoCoinCoreBridge*)core
{
    (void)core;
    if ((self = [super initWithFrame:NSZeroRect])) self.identifier = @"corporate";
    return self;
}

- (void)applyTheme {}

@end
