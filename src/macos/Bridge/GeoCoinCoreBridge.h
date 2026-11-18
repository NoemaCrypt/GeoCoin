// GeoCoinCoreBridge — ObjC bridge over src/qt WalletModel/ClientModel (Qt Core models reused 1:1).
#import <Foundation/Foundation.h>
#import <AppKit/AppKit.h>

#ifdef __cplusplus
class WalletModel;
class ClientModel;
class MiningPage;
#endif
@class GeoGpuMinerLauncher;
@class GeoGpuDeviceInfo;

NS_ASSUME_NONNULL_BEGIN

typedef void (^GeoCountsBlock)(NSInteger count);
typedef void (^GeoBalanceBlock)(double available, double pending, double immature, double watchOnly);
typedef void (^GeoAddressBlock)(NSString* address);
typedef void (^GeoEmptyBlock)(void);
typedef void (^GeoMessageBlock)(NSString* title, NSString* message, NSUInteger style);
typedef void (^GeoMiningStatusBlock)(BOOL enabled, NSInteger threads, double hashrate, NSInteger accepted, NSInteger rejected);
typedef void (^GeoWalletNameBlock)(NSString* name, NSInteger walletId, BOOL loaded, BOOL pruned);
typedef void (^GeoGpuStatsBlock)(NSDictionary* json);          // parsed --stats-json line
typedef void (^GeoGpuDevicesBlock)(NSArray<GeoGpuDeviceInfo*> *devices); // [{backend,id,name,identity(,hashrate)}]
typedef void (^GeoGpuExitBlock)(NSInteger exitCode);

/**
 * Presents the application-owned models to the AppKit frontend on the main thread.
 */
@interface GeoCoinCoreBridge : NSObject
#ifdef __cplusplus
@property (assign, nullable) WalletModel* walletModel;
@property (assign, nullable) ClientModel* clientModel;
- (void)setClientModel:(ClientModel* _Nullable)model;
- (void)setWalletModel:(WalletModel* _Nullable)model;
- (void)setMiningController:(MiningPage* _Nullable)controller;
#endif

// ClientModel signals (numConnectionsChanged, numBlocksChanged, miningChanged,
// messagesShown, syncProgressChanged) → status bar.
@property (nonatomic, copy, nullable) GeoCountsBlock onConnections;
@property (nonatomic, copy, nullable) GeoCountsBlock onBlocks;
@property (nonatomic, copy, nullable) GeoMiningStatusBlock onMining;
@property (nonatomic, copy, nullable) GeoMessageBlock onMessage;
@property (nonatomic, copy, nullable) GeoEmptyBlock onRefresh;   // refreshWallet / reload
@property (nonatomic, copy, nullable) void (^onSyncProgress)(double progress);
@property (nonatomic, copy) NSString* selectedNetwork;
@property (nonatomic, assign) BOOL testing;

// WalletModel signals (balanceChanged, addressChanged, transactionReset,
// addressBookReset, encryptionChanged, walletLoadedChanged, primaryAddressChanged)
@property (nonatomic, copy, nullable) GeoBalanceBlock onBalances;
@property (nonatomic, copy, nullable) GeoAddressBlock onAddress;
@property (nonatomic, copy, nullable) GeoWalletNameBlock onWalletInfo;
@property (nonatomic, copy, nullable) void (^onWalletHistoryError)(NSString* error);
@property (nonatomic, copy, nullable) GeoEmptyBlock onTransactionsReset;
@property (nonatomic, copy, nullable) GeoEmptyBlock onEncryptionChanged;

- (NSInteger)numConnections;
- (NSInteger)numBlocks;
- (double)syncProgress;                 // 0..1
- (double)miningHashrate;               // H/s
- (NSString*)networkName;               // "degree" | "testnet"
- (NSString*)walletName;
- (BOOL)isWalletLoaded;
- (BOOL)isPruned;
- (NSString* _Nullable)primaryAddress;
- (void)refreshBalances;                // pulls getBalance() into onBalances
- (void)startMiningThreads:(NSInteger)threads;
- (void)stopMining;
- (nullable NSString*)requestCpuMiningWithThreads:(NSInteger)threads;
- (BOOL)isCpuMiningStopping;
- (NSInteger)miningThreads;
- (BOOL)miningEnabled;
- (NSInteger)miningAccepted;
- (NSInteger)miningRejected;
- (nullable NSImage*)logoImage;         // :/images/logo.png
- (nullable NSImage*)backgroundImageForTheme:(NSString*)theme;
- (NSArray<NSNumber*>*)watchOnlyBalances;
- (NSString*)localized:(NSString*)source context:(NSString*)context;
- (void)updateSyncProgress:(double)progress current:(NSInteger)current target:(NSInteger)target;
- (void)observeAddress:(GeoAddressBlock)observer;
- (void)observeBalances:(GeoBalanceBlock)observer;

// GPU stack (miningpage_gpu.cpp): NSTask launcher + JSONL stats relay.
@property (nonatomic, strong, readonly) GeoGpuMinerLauncher* gpuLauncher;
@property (nonatomic, copy, nullable) GeoGpuStatsBlock onGpuStats;
@property (nonatomic, copy, nullable) GeoGpuDevicesBlock onGpuDevices;
@property (nonatomic, copy, nullable) GeoGpuExitBlock onGpuExit;
@property (nonatomic, copy, nullable) NSString* dataDirectory;   // Intro::pickDataDirectory
- (nullable NSString*)startGpuMiningWithBackend:(NSString*)backend
                                      deviceIds:(NSArray<NSString*>*)ids intensity:(NSInteger)intensity;
- (void)stopGpuMining;
- (BOOL)isGpuMiningActive;
- (BOOL)isGpuMiningStopping;
- (void)observeGpuStatus:(GeoGpuStatsBlock)observer;

// QAbstractTableModel snapshots (DisplayRole cells) for NSTableView sources.
- (NSArray<NSDictionary<NSString*, NSString*>*>*)transactions;  // TransactionTableModel
- (NSArray<NSDictionary<NSString*, NSString*>*>*)addressBook;   // AddressTableModel
/// WalletModel::sendCoins(address, amount satoshi, fee satoshi, password)
- (NSInteger)sendCoinsTo:(NSString*)address amount:(double)amountGEO fee:(double)feeGEO;
- (NSString*)standardFee;
- (void)confirmSendCoinsTo:(NSString*)address amount:(NSString*)amount fee:(NSString*)fee
                  window:(nullable NSWindow*)window completion:(GeoEmptyBlock)completion;
@end

NS_ASSUME_NONNULL_END
