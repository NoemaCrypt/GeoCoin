// GeoGpuMinerLauncher implementation — NSTask equivalent of the Qt QProcess pair.
#import "GeoGpuMinerLauncher.h"

@implementation GeoGpuDeviceInfo
@end

@interface GeoGpuMinerLauncher ()
@property (nonatomic, strong, nullable) NSTask* minerTask;
@property (nonatomic, strong, nullable) NSTask* probeTask;
@property (nonatomic, strong, nullable) NSFileHandle* minerStdin;
@property (nonatomic, strong) NSMutableData* outputBuffer;
@end

@implementation GeoGpuMinerLauncher

- (instancetype)init
{
    if ((self = [super init])) {
        _backend = @"auto";
        _intensity = 0;
        _selectedDeviceIds = @[];
        _outputBuffer = [NSMutableData data];
    }
    return self;
}

- (BOOL)running { return self.minerTask.running; }

- (NSString*)minerBinaryPath
{
    NSString* exe = @"geocoin-gpu-miner";
    NSString* executable = NSBundle.mainBundle.executablePath;
    NSString* directory = executable.length ? executable.stringByDeletingLastPathComponent
                                             : NSFileManager.defaultManager.currentDirectoryPath;
    NSArray<NSString*>* candidates = [directory.lastPathComponent isEqualToString:@"MacOS"]
        ? @[ @"../Helpers/geocoin-gpu-miner", @"../../../geocoin-gpu-miner", exe, @"bin/geocoin-gpu-miner" ]
        : @[ exe, @"bin/geocoin-gpu-miner" ];
    for (NSString* relative in candidates) {
        NSString* path = [directory stringByAppendingPathComponent:relative].stringByStandardizingPath;
        if ([NSFileManager.defaultManager isExecutableFileAtPath:path]) return path;
    }
    return [directory stringByAppendingPathComponent:exe];
}

- (void)probeDevices:(void (^)(NSArray<GeoGpuDeviceInfo*>* devices))completion
{
    self.lastError = nil;
    NSString* miner = [self minerBinaryPath];
    if (![NSFileManager.defaultManager isExecutableFileAtPath:miner]) {
        self.lastError = @"geocoin-gpu-miner was not found in the bundle.";
        if (completion) completion(@[]);
        return;
    }
    if (self.probeTask.running) [self.probeTask terminate];
    NSTask* task = [[NSTask alloc] init];
    task.executableURL = [NSURL fileURLWithPath:miner];
    task.arguments = @[ @"--list-devices", @"--stats-json" ];
    NSPipe* pipe = [NSPipe pipe];
    task.standardOutput = pipe;
    task.standardError = pipe;
    self.probeTask = task;

    NSError* error = nil;
    if (![task launchAndReturnError:&error]) {
        self.probeTask = nil;
        self.lastError = error.localizedDescription;
        if (completion) completion(@[]);
        return;
    }
    __weak GeoGpuMinerLauncher* weakSelf = self;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
        GeoGpuMinerLauncher* launcher = weakSelf;
        if (launcher.probeTask == task && task.running) [task terminate];
    });
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        NSData* output = [pipe.fileHandleForReading readDataToEndOfFile];
        [task waitUntilExit];
        NSMutableArray<GeoGpuDeviceInfo*>* devices = [NSMutableArray array];
        NSString* text = [[NSString alloc] initWithData:output encoding:NSUTF8StringEncoding];
        BOOL foundResult = NO;
        for (NSString* line in [text componentsSeparatedByString:@"\n"]) {
            NSData* json = [line dataUsingEncoding:NSUTF8StringEncoding];
            if (!json.length) continue;
            id result = [NSJSONSerialization JSONObjectWithData:json options:0 error:NULL];
            if (![result isKindOfClass:NSDictionary.class] || ![result[@"event"] isEqual:@"devices"]) continue;
            NSArray* rows = result[@"devices"];
            if (![rows isKindOfClass:NSArray.class]) continue;
            foundResult = YES;
            for (id row in rows) {
                if (![row isKindOfClass:NSDictionary.class] || ![row[@"available"] boolValue]) continue;
                NSString* backend = row[@"backend"];
                NSNumber* deviceId = row[@"id"];
                if (![backend isKindOfClass:NSString.class] || ![deviceId isKindOfClass:NSNumber.class]) continue;
                GeoGpuDeviceInfo* device = [[GeoGpuDeviceInfo alloc] init];
                device.backend = backend;
                device.id = deviceId.integerValue;
                device.name = [row[@"name"] isKindOfClass:NSString.class] ? row[@"name"] : backend;
                device.identity = [row[@"identity"] isKindOfClass:NSString.class] ? row[@"identity"] : nil;
                [devices addObject:device];
            }
        }
        dispatch_async(dispatch_get_main_queue(), ^{
            GeoGpuMinerLauncher* launcher = weakSelf;
            if (!launcher || launcher.probeTask != task) return;
            launcher.probeTask = nil;
            if (!foundResult || task.terminationStatus != 0) {
                launcher.lastError = @"Could not detect GPU devices.";
                if (completion) completion(@[]);
            } else if (completion) {
                completion(devices);
            }
        });
    });
}

/// MiningPage::toggleGpuMining() argv (exact Qt order):
/// --url <rpc url> --rpc-user <user> --rpc-pass-file <file> --address <addr>
/// --backend <cuda|hip|opencl> --intensity <n> --continuous --control-stdin
/// --stats-json --devices <ids csv>
- (void)start
{
    if (self.minerTask.running) { [self stop]; return; }
    NSString* miner = [self minerBinaryPath];
    if (![[NSFileManager defaultManager] fileExistsAtPath:miner]) {
        self.lastError = @"geocoin-gpu-miner was not found in the bundle.";
        if (self.onStats) { self.onStats(@{ @"event": @"error",
                                            @"message": self.lastError }); }
        return;
    }
    NSMutableArray<NSString*>* args = [NSMutableArray array];
    [args addObjectsFromArray:@[ @"--url", self.rpcUrl ?: @"http://127.0.0.1:18332/" ]];
    [args addObjectsFromArray:@[ @"--rpc-user", self.rpcUser ?: @"geocoin" ]];
    [args addObjectsFromArray:@[ @"--rpc-pass-file",
                                 self.rpcPasswordFile ?: @"gpu_rpc_password" ]];
    [args addObjectsFromArray:@[ @"--address", self.miningAddress ?: @"" ]];
    [args addObjectsFromArray:@[ @"--backend", self.backend ]];
    [args addObjectsFromArray:@[ @"--intensity", [NSString stringWithFormat:@"%ld",
                                                  (long)self.intensity] ]];
    [args addObjectsFromArray:@[ @"--continuous", @"--control-stdin", @"--stats-json" ]];
    [args addObjectsFromArray:@[ @"--devices",
                                 [self.selectedDeviceIds componentsJoinedByString:@","] ]];

    NSTask* task = [[NSTask alloc] init];
    task.executableURL = [NSURL fileURLWithPath:miner];
    task.arguments = args;
    NSPipe* outPipe = [NSPipe pipe];
    NSPipe* inPipe = [NSPipe pipe];
    task.standardOutput = outPipe;
    task.standardError = outPipe;
    task.standardInput = inPipe;
    self.minerStdin = inPipe.fileHandleForWriting;
    self.minerTask = task;
    self.outputBuffer.length = 0;

    __weak GeoGpuMinerLauncher* weakSelf = self;
    NSFileHandle* fh = outPipe.fileHandleForReading;
    fh.readabilityHandler = ^(NSFileHandle* handle) {
        GeoGpuMinerLauncher* s = weakSelf;
        if (!s) { return; }
        NSData* data = handle.availableData;
        if (data.length == 0) { return; }
        [s.outputBuffer appendData:data];
        NSString* whole = [[NSString alloc]
            initWithData:s.outputBuffer encoding:NSUTF8StringEncoding];
        NSRange lastNL = [whole rangeOfString:@"\n" options:NSBackwardsSearch];
        if (lastNL.location == NSNotFound) { return; }
        NSString* complete = [whole substringToIndex:lastNL.location];
        NSString* rest = [whole substringFromIndex:NSMaxRange(lastNL)];
        s.outputBuffer.length = 0;
        [s.outputBuffer appendData:[rest dataUsingEncoding:NSUTF8StringEncoding]];
        for (NSString* raw in [complete componentsSeparatedByString:@"\n"]) {
            NSString* line = [raw stringByTrimmingCharactersInSet:
                              [NSCharacterSet whitespaceAndNewlineCharacterSet]];
            if (line.length == 0) { continue; }
            NSData* json = [line dataUsingEncoding:NSUTF8StringEncoding];
            id parsed = [NSJSONSerialization JSONObjectWithData:json options:0 error:nil];
            dispatch_async(dispatch_get_main_queue(), ^{
                if ([parsed isKindOfClass:[NSDictionary class]]) {
                    if (s.onStats) { s.onStats(parsed); }
                } else {
                    s.lastError = [line substringToIndex:MIN(line.length, 1024)];
                    if (s.onStats) {
                        s.onStats(@{ @"event": @"raw", @"message": s.lastError });
                    }
                }
            });
        }
    };
    task.terminationHandler = ^(NSTask* finished) {
        GeoGpuMinerLauncher* s = weakSelf;
        dispatch_async(dispatch_get_main_queue(), ^{
            if (s.onExited) { s.onExited(finished.terminationStatus); }
        });
    };
    NSError* error = nil;
    @try { [task launchAndReturnError:&error]; } @catch (NSException* e) {
        self.lastError = e.reason;
    }
    if (error) { self.lastError = error.localizedDescription; }
}

- (void)stop
{
    [self.minerStdin writeData:[@"stop\n" dataUsingEncoding:NSUTF8StringEncoding]];
}

@end
