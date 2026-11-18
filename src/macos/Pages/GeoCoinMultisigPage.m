#import "GeoCoinMultisigPage.h"

@implementation GeoCoinMultisigPage

- (instancetype)initWithCore:(GeoCoinCoreBridge*)core
{
    (void)core;
    if ((self = [super initWithFrame:NSZeroRect])) self.identifier = @"multisig";
    return self;
}

- (void)applyTheme {}

@end
