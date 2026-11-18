#import "GeoCoinMiningPage.h"
#import "GeoCoinPageKit.h"
#import "GeoGpuMinerLauncher.h"

@interface GeoCoinMiningPage ()
@property (nonatomic, strong) GeoCoinCoreBridge* core;
@property (nonatomic, strong) NSTextField* address;
@property (nonatomic, strong) NSTextField* cpuThreads;
@property (nonatomic, strong) NSTextField* cpuStatus;
@property (nonatomic, strong) GeoPillButton* cpuToggle;
@property (nonatomic, strong) NSSegmentedControl* backends;
@property (nonatomic, strong) NSTextField* intensity;
@property (nonatomic, strong) NSStackView* deviceRows;
@property (nonatomic, strong) GeoPillButton* refresh;
@property (nonatomic, strong) GeoPillButton* gpuToggle;
@property (nonatomic, strong) NSTextField* gpuStatus;
@property (nonatomic, strong) NSTextField* gpuMetrics;
@property (nonatomic, strong) NSMapTable<NSTextField*, NSString*>* captions;
@property (nonatomic, copy) NSArray<GeoGpuDeviceInfo*>* devices;
@property (nonatomic, copy) NSDictionary* gpuStats;
@property (nonatomic, copy, nullable) NSString* cpuError;
@property (nonatomic, copy, nullable) NSString* gpuError;
@property (nonatomic, strong, nullable) NSTimer* stateTimer;
@property (nonatomic) BOOL probing;
@end

static NSString* GeoMiningRate(double hashes)
{
    if (hashes >= 1e9) return [NSString stringWithFormat:@"%.3f GH/s", hashes / 1e9];
    if (hashes >= 1e6) return [NSString stringWithFormat:@"%.3f MH/s", hashes / 1e6];
    if (hashes >= 1e3) return [NSString stringWithFormat:@"%.2f kH/s", hashes / 1e3];
    return [NSString stringWithFormat:@"%.0f H/s", hashes];
}

static NSInteger GeoMiningInteger(NSTextField* field, NSInteger minimum, NSInteger maximum)
{
    NSScanner* scanner = [NSScanner scannerWithString:field.stringValue];
    long long value = 0;
    if (![scanner scanLongLong:&value] || !scanner.isAtEnd || value < minimum || value > maximum) return 0;
    return (NSInteger)value;
}

@implementation GeoCoinMiningPage

- (NSString*)text:(NSString*)source
{
    return [self.core localized:source context:@"MiningPage"];
}

- (NSStackView*)row:(NSString*)caption control:(NSView*)control
{
    NSStackView* row = GeoMakeRow(8);
    [row addArrangedSubview:[self caption:caption]];
    [row addArrangedSubview:control];
    return row;
}

- (NSTextField*)caption:(NSString*)source
{
    NSTextField* label = GeoMakeLabel([self text:source], nil, 12);
    [self.captions setObject:source forKey:label];
    return label;
}

- (instancetype)initWithCore:(GeoCoinCoreBridge*)core
{
    if (!(self = [super initWithFrame:NSZeroRect])) return nil;
    _core = core;
    _devices = @[];
    _gpuStats = @{};
    _captions = [NSMapTable weakToStrongObjectsMapTable];
    self.identifier = @"mining";
    NSStackView* body;
    NSStackView* page = (NSStackView*)GeoMakePage([self text:@"Mining"], &body);
    [_captions setObject:@"Mining" forKey:(NSTextField*)page.arrangedSubviews.firstObject];
    [self addSubview:page];
    [NSLayoutConstraint activateConstraints:@[
        [page.topAnchor constraintEqualToAnchor:self.topAnchor],
        [page.bottomAnchor constraintLessThanOrEqualToAnchor:self.bottomAnchor],
        [page.leadingAnchor constraintEqualToAnchor:self.leadingAnchor],
        [page.trailingAnchor constraintEqualToAnchor:self.trailingAnchor],
    ]];

    _address = GeoMakeLabel([self text:@"No wallet loaded"], nil, 12);
    _address.selectable = YES;
    [body addArrangedSubview:[self row:@"Mining Address:" control:_address]];

    __weak GeoCoinMiningPage* weakSelf = self;
    NSStackView* cpuBody;
    GeoGlassPanel* cpu = GeoMakeGroup([self text:@"CPU"], &cpuBody);
    _cpuThreads = GeoMakeField(nil);
    _cpuThreads.identifier = @"mining-cpu-threads";
    _cpuThreads.stringValue = [NSString stringWithFormat:@"%lu",
        (unsigned long)NSProcessInfo.processInfo.activeProcessorCount];
    [_cpuThreads.widthAnchor constraintEqualToConstant:80].active = YES;
    NSStackView* cpuControls = [self row:@"Mining Threads:" control:_cpuThreads];
    _cpuToggle = GeoMakePill([self text:@"Start Mining"], ^{ [weakSelf toggleCpu]; });
    _cpuToggle.identifier = @"mining-cpu-toggle";
    [cpuControls addArrangedSubview:_cpuToggle];
    [cpuBody addArrangedSubview:cpuControls];
    _cpuStatus = GeoMakeLabel([self text:@"Not running"], nil, 12);
    _cpuStatus.identifier = @"mining-cpu-status";
    [cpuBody addArrangedSubview:_cpuStatus];
    GeoFill(_cpuStatus);
    [body addArrangedSubview:cpu];
    GeoFill(cpu);

    NSStackView* gpuBody;
    GeoGlassPanel* gpu = GeoMakeGroup([self text:@"GPU"], &gpuBody);
    _backends = [NSSegmentedControl segmentedControlWithLabels:@[ @"Auto", @"Metal", @"OpenCL" ]
                                                 trackingMode:NSSegmentSwitchTrackingSelectOne
                                                       target:self action:@selector(changeBackend:)];
    _backends.selectedSegment = 0;
    _backends.identifier = @"mining-gpu-backend";
    [gpuBody addArrangedSubview:[self row:@"Backend:" control:_backends]];
    _intensity = GeoMakeField(nil);
    _intensity.identifier = @"mining-gpu-intensity";
    _intensity.stringValue = @"16777216";
    [_intensity.widthAnchor constraintEqualToConstant:120].active = YES;
    [gpuBody addArrangedSubview:[self row:@"Intensity:" control:_intensity]];

    [gpuBody addArrangedSubview:[self caption:@"GPU devices:"]];
    _deviceRows = GeoMakeStack(3);
    [gpuBody addArrangedSubview:_deviceRows];
    GeoFill(_deviceRows);

    NSStackView* gpuControls = GeoMakeRow(8);
    _refresh = GeoMakePill([self text:@"Refresh GPU Devices"], ^{ [weakSelf probeDevices]; });
    _gpuToggle = GeoMakePill([self text:@"Start GPU Mining"], ^{ [weakSelf toggleGpu]; });
    _refresh.identifier = @"mining-gpu-refresh";
    _gpuToggle.identifier = @"mining-gpu-toggle";
    [gpuControls addArrangedSubview:_refresh];
    [gpuControls addArrangedSubview:_gpuToggle];
    [gpuBody addArrangedSubview:gpuControls];

    _gpuStatus = GeoMakeLabel([self text:@"Not running"], nil, 12);
    _gpuStatus.identifier = @"mining-gpu-status";
    _gpuMetrics = GeoMakeLabel(@"", nil, 12);
    [gpuBody addArrangedSubview:_gpuStatus];
    [gpuBody addArrangedSubview:_gpuMetrics];
    GeoFill(_gpuStatus);
    GeoFill(_gpuMetrics);
    [body addArrangedSubview:gpu];
    GeoFill(gpu);
    core.onGpuStats = ^(NSDictionary* stats) {
        GeoCoinMiningPage* page = weakSelf;
        if (!page) return;
        NSMutableDictionary* combined = [page.gpuStats mutableCopy];
        [combined addEntriesFromDictionary:stats];
        page.gpuStats = combined;
        [page updateControls];
    };
    [self updateDeviceRows];
    [self updateControls];
    return self;
}

- (void)viewDidMoveToWindow
{
    [super viewDidMoveToWindow];
    [self.stateTimer invalidate];
    self.stateTimer = nil;
    if (self.window) {
        __weak GeoCoinMiningPage* weakSelf = self;
        self.stateTimer = [NSTimer scheduledTimerWithTimeInterval:0.5 repeats:YES block:^(NSTimer* _) {
            if (!weakSelf) [_ invalidate];
            [weakSelf updateControls];
        }];
        [self probeDevices];
    }
}

- (NSString*)selectedBackend
{
    NSInteger segment = self.backends.selectedSegment;
    return segment == 1 ? @"metal" : segment == 2 ? @"opencl" : @"auto";
}

- (void)changeBackend:(id)sender
{
    self.gpuError = nil;
    [self updateDeviceRows];
    [self updateControls];
}

- (void)probeDevices
{
    if (self.probing || [self.core isGpuMiningActive]) return;
    self.probing = YES;
    self.gpuError = nil;
    [self updateControls];
    __weak GeoCoinMiningPage* weakSelf = self;
    [self.core.gpuLauncher probeDevices:^(NSArray<GeoGpuDeviceInfo*>* devices) {
        GeoCoinMiningPage* page = weakSelf;
        if (!page) return;
        page.probing = NO;
        page.devices = devices;
        page.gpuError = page.core.gpuLauncher.lastError;
        [page updateDeviceRows];
        [page updateControls];
    }];
}

- (void)updateDeviceRows
{
    for (NSView* row in self.deviceRows.arrangedSubviews.copy) {
        [self.deviceRows removeArrangedSubview:row];
        [row removeFromSuperview];
    }
    NSString* backend = [self selectedBackend];
    if ([backend isEqualToString:@"auto"]) {
        backend = @"opencl";
        for (GeoGpuDeviceInfo* device in self.devices) {
            if ([device.backend isEqualToString:@"metal"]) {
                backend = @"metal";
                break;
            }
        }
    }
    for (GeoGpuDeviceInfo* device in self.devices) {
        if (![device.backend isEqualToString:backend]) continue;
        NSString* name = [NSString stringWithFormat:@"%@ #%ld", device.name, (long)device.id];
        NSButton* row = [NSButton checkboxWithTitle:name target:self action:@selector(deviceChanged:)];
        row.identifier = @"mining-gpu-device";
        row.tag = device.id;
        row.state = NSControlStateValueOn;
        [self.deviceRows addArrangedSubview:row];
    }
    if (!self.deviceRows.arrangedSubviews.count) {
        [self.deviceRows addArrangedSubview:[self caption:@"No GPU devices found"]];
    }
}

- (void)deviceChanged:(id)sender
{
    [self updateControls];
}

- (NSArray<NSString*>*)selectedDeviceIds
{
    NSMutableArray<NSString*>* ids = [NSMutableArray array];
    for (NSView* row in self.deviceRows.arrangedSubviews) {
        if (![row isKindOfClass:NSButton.class]) continue;
        NSButton* button = (NSButton*)row;
        if (button.state == NSControlStateValueOn) [ids addObject:@(button.tag).stringValue];
    }
    return ids;
}

- (void)toggleCpu
{
    self.cpuError = nil;
    NSInteger threads = 0;
    if (![self.core miningEnabled]) {
        threads = GeoMiningInteger(self.cpuThreads, 1, NSProcessInfo.processInfo.activeProcessorCount);
        if (!threads) self.cpuError = [self text:@"Enter a valid CPU thread count."];
    }
    if (!self.cpuError) self.cpuError = [self.core requestCpuMiningWithThreads:threads];
    [self updateControls];
}

- (void)toggleGpu
{
    self.gpuError = nil;
    if ([self.core isGpuMiningActive]) {
        [self.core stopGpuMining];
    } else {
        NSInteger intensity = GeoMiningInteger(self.intensity, 1024, INT32_MAX);
        if (!intensity) {
            self.gpuError = [self text:@"Invalid GPU intensity."];
        } else {
            self.gpuError = [self.core startGpuMiningWithBackend:[self selectedBackend]
                                                      deviceIds:[self selectedDeviceIds] intensity:intensity];
        }
    }
    [self updateControls];
}

- (void)updateControls
{
    BOOL ready = [self.core isWalletLoaded];
    NSString* address = [self.core primaryAddress];
    self.address.stringValue = address.length ? address : [self text:@"No wallet loaded"];
    BOOL cpuActive = [self.core miningEnabled];
    BOOL cpuStopping = [self.core isCpuMiningStopping];
    self.cpuToggle.title = [self text:cpuStopping ? @"Stopping..." : cpuActive ? @"Stop Mining" : @"Start Mining"];
    self.cpuToggle.enabled = (ready || cpuActive) && !cpuStopping;
    self.cpuThreads.enabled = !cpuActive && !cpuStopping;
    NSString* cpuRate = GeoMiningRate(cpuActive ? [self.core miningHashrate] : 0);
    self.cpuStatus.stringValue = self.cpuError ?: [NSString stringWithFormat:@"%@  ·  %@ %ld",
        cpuRate, [self text:@"Blocks Found:"], (long)[self.core miningAccepted]];

    BOOL gpuActive = [self.core isGpuMiningActive];
    BOOL gpuStopping = [self.core isGpuMiningStopping];
    self.gpuToggle.title = [self text:gpuStopping ? @"Stopping..." : gpuActive ? @"Stop GPU Mining" : @"Start GPU Mining"];
    self.gpuToggle.enabled = !gpuStopping && (gpuActive || (ready && !self.probing && [self selectedDeviceIds].count));
    self.backends.enabled = !gpuActive && !self.probing;
    self.intensity.enabled = !gpuActive;
    self.refresh.enabled = !gpuActive && !self.probing;
    for (NSView* row in self.deviceRows.arrangedSubviews) {
        if ([row isKindOfClass:NSButton.class]) ((NSButton*)row).enabled = !gpuActive && !self.probing;
    }
    NSDictionary* stats = self.gpuStats;
    NSString* backend = stats[@"backend"];
    NSString* state = stats[@"status"] ?: [self text:@"Not running"];
    if (gpuActive && backend.length && ![backend isEqualToString:@"auto"]) {
        state = [NSString stringWithFormat:@"%@  ·  %@", [backend isEqualToString:@"metal"] ? @"Metal" : @"OpenCL", state];
    }
    self.gpuStatus.stringValue = self.gpuError ?: (self.probing ? [self text:@"Reading GPU devices..."] : state);
    self.gpuMetrics.stringValue = [NSString stringWithFormat:@"%@  ·  %@ %@  ·  %@ %@",
        GeoMiningRate(gpuActive ? [stats[@"hashrate"] doubleValue] : 0),
        [self text:@"Accepted Blocks:"], stats[@"accepted"] ?: @0,
        [self text:@"Rejected Blocks:"], stats[@"rejected"] ?: @0];
}

- (void)applyTheme
{
    GeoApplyThemeToView(self);
}

- (void)applyLanguage
{
    for (NSTextField* label in self.captions.keyEnumerator) {
        label.stringValue = [self text:[self.captions objectForKey:label]];
    }
    self.refresh.title = [self text:@"Refresh GPU Devices"];
    [self updateControls];
}

- (void)updateCpuEnabled:(BOOL)enabled threads:(NSInteger)threads
{
    if (enabled && threads > 0) self.cpuThreads.stringValue = [NSString stringWithFormat:@"%ld", (long)threads];
    [self updateControls];
}
@end
