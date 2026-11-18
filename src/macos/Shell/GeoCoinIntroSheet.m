#import "GeoCoinIntroSheet.h"
@implementation GeoCoinIntroSheet
- (void)presentAsModalForWindow:(NSWindow*)window {
    NSOpenPanel* panel = [NSOpenPanel openPanel];
    panel.canChooseDirectories = YES;
    panel.canChooseFiles = NO;
    __weak GeoCoinIntroSheet* weakSelf = self;
    [panel beginSheetModalForWindow:window completionHandler:^(NSModalResponse response) {
        if (response == NSModalResponseOK) { weakSelf.dataDirectory = panel.URL.path; }
    }];
}
@end
