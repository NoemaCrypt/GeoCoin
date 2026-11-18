// GeoGpuMinerLauncher — NSTask mirror of MiningPage's QProcess pair in
// src/qt/miningpage_gpu.cpp (gpuDeviceProbe --list-devices, gpuMinerProcess
// --stats-json JSONL lines, stdin "stop" control).
#ifndef GEOCOIN_MACOS_GPU_MINER_LAUNCHER_H
#define GEOCOIN_MACOS_GPU_MINER_LAUNCHER_H

#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

/// One row of `geocoin-gpu-miner --list-devices --stats-json`
/// (tools/gpu_miner_runtime.hpp device_json keys: backend/id/name/identity,
/// plus hashrate on live `event:"status"` snapshots).
@interface GeoGpuDeviceInfo : NSObject
@property (nonatomic, copy) NSString* backend;  // cuda | hip | metal | opencl
@property (nonatomic, assign) NSInteger id;
@property (nonatomic, copy) NSString* name;
@property (nonatomic, copy, nullable) NSString* identity;  // "backend:uuid:…" stable key
@property (nonatomic, assign) double hashrate;   // H/s
@end

/// Mirrors MiningPage::createGpuMinerProcess + gpuMinerReadyRead +
/// processGpuStatsLine + probeGpuDevices + toggleGpuMining.
@interface GeoGpuMinerLauncher : NSObject

@property (nonatomic, copy, nullable) NSString* rpcUrl;        // http://127.0.0.1:<port>/
@property (nonatomic, copy, nullable) NSString* rpcUser;
@property (nonatomic, copy, nullable) NSString* rpcPasswordFile;
@property (nonatomic, copy, nullable) NSString* miningAddress;  // --address
@property (nonatomic, copy) NSString* backend;                  // auto (default)
@property (nonatomic, copy) NSArray<NSString*>* selectedDeviceIds;
@property (nonatomic, assign) NSInteger intensity;
@property (nonatomic, readonly) BOOL running;
@property (nonatomic, copy, nullable) NSString* lastError;

/// probeGpuDevices(): run `--list-devices --stats-json` once.
- (void)probeDevices:(void (^)(NSArray<GeoGpuDeviceInfo*>* devices))completion;
/// toggleGpuMining(): spawn the miner with the Qt argv order.
- (void)start;
/// stopGpuMining(): write "stop\n" to stdin.
- (void)stop;
/// gpuMinerReadyRead/processGpuStatsLine: one parsed JSONL object per call.
@property (nonatomic, copy, nullable) void (^onStats)(NSDictionary* object);
@property (nonatomic, copy, nullable) void (^onExited)(NSInteger exitCode);

@end

NS_ASSUME_NONNULL_END
#endif // GEOCOIN_MACOS_GPU_MINER_LAUNCHER_H
