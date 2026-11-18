#import "GeoCoinCoreBridge.h"
#import "GeoGpuMinerLauncher.h"

#include "addresstablemodel.h"
#include "clientmodel.h"
#include "guiutil.h"
#include "miningpage.h"
#include "transactiontablemodel.h"
#include "walletmodel.h"
#include "geocoin/crypto/addresses.hpp"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QPointer>
#include <vector>

static NSString* GeoString(const QString& value)
{
    return value.toNSString() ?: @"";
}

static double GeoCoins(qint64 amount)
{
    return static_cast<double>(amount) / geocoin::COIN;
}

static void Disconnect(std::vector<QMetaObject::Connection>& connections)
{
    for (const auto& connection : connections) QObject::disconnect(connection);
    connections.clear();
}

@implementation GeoCoinCoreBridge {
    GeoGpuMinerLauncher* _gpuLauncher;
    std::vector<QMetaObject::Connection> _walletConnections;
    std::vector<QMetaObject::Connection> _clientConnections;
    std::vector<QMetaObject::Connection> _miningConnections;
    QPointer<MiningPage> _miningController;
    NSMutableArray<GeoAddressBlock>* _addressObservers;
    NSMutableArray<GeoBalanceBlock>* _balanceObservers;
    NSMutableArray<GeoGpuStatsBlock>* _gpuObservers;
    double _gpuHashrate;
    NSInteger _gpuAccepted;
    NSInteger _gpuRejected;
    double _syncProgress;
}

- (instancetype)init
{
    if ((self = [super init])) {
        _selectedNetwork = @"degree";
        _addressObservers = [NSMutableArray array];
        _balanceObservers = [NSMutableArray array];
        _gpuObservers = [NSMutableArray array];
        _syncProgress = 1;
    }
    return self;
}

- (void)dealloc
{
    Disconnect(_clientConnections);
    Disconnect(_walletConnections);
    Disconnect(_miningConnections);
}

- (void)observeAddress:(GeoAddressBlock)observer
{
    [_addressObservers addObject:[observer copy]];
    observer(self.primaryAddress ?: @"");
}

- (void)observeBalances:(GeoBalanceBlock)observer
{
    [_balanceObservers addObject:[observer copy]];
    [self refreshBalances];
}

- (void)notifyAddress
{
    NSString* address = self.primaryAddress ?: @"";
    if (self.onAddress) self.onAddress(address);
    for (GeoAddressBlock observer in [_addressObservers copy]) observer(address);
}

- (void)notifyWallet
{
    if (self.onWalletInfo) self.onWalletInfo(self.walletName, 0, self.isWalletLoaded, self.isPruned);
}

- (void)notifyTransactions
{
    if (self.onTransactionsReset) self.onTransactionsReset();
}

- (void)setWalletModel:(WalletModel*)model
{
    if (_walletModel == model) return;
    Disconnect(_walletConnections);
    _walletModel = model;
    if (self.onWalletHistoryError) self.onWalletHistoryError(model ? GeoString(model->getHistoryScanError()) : @"");
    if (!model) return;
    __weak GeoCoinCoreBridge* bridge = self;
    _walletConnections.push_back(QObject::connect(model, &WalletModel::balanceChanged, model,
        [bridge](qint64, qint64, qint64) { [bridge refreshBalances]; }));
    _walletConnections.push_back(QObject::connect(model, &WalletModel::watchOnlyBalanceChanged, model,
        [bridge](qint64, qint64, qint64) { [bridge refreshBalances]; }));
    _walletConnections.push_back(QObject::connect(model, &WalletModel::primaryAddressChanged, model,
        [bridge](const QString&) { [bridge notifyAddress]; }));
    _walletConnections.push_back(QObject::connect(model, &WalletModel::message, model,
        [bridge](const QString& title, const QString& text, unsigned int style) {
            GeoMessageBlock message = bridge.onMessage;
            if (message) message(GeoString(title), GeoString(text), style);
        }));
    _walletConnections.push_back(QObject::connect(model, &WalletModel::encryptionStatusChanged, model,
        [bridge](int) {
            GeoEmptyBlock notify = bridge.onEncryptionChanged;
            if (notify) notify();
        }));
    _walletConnections.push_back(QObject::connect(model, &WalletModel::walletLoadedChanged, model,
        [bridge, model](bool) {
            if (bridge.onWalletHistoryError) bridge.onWalletHistoryError(GeoString(model->getHistoryScanError()));
            [bridge notifyWallet];
            [bridge notifyAddress];
            [bridge refreshBalances];
        }));
    _walletConnections.push_back(QObject::connect(model, &WalletModel::walletHistoryError, model,
        [bridge](const QString& error) {
            if (bridge.onWalletHistoryError) bridge.onWalletHistoryError(GeoString(error));
        }));
    _walletConnections.push_back(QObject::connect(model, &WalletModel::walletStateChanged, model,
        [bridge]() {
            GeoEmptyBlock refresh = bridge.onRefresh;
            if (refresh) refresh();
        }));
    auto* transactions = model->getTransactionTableModel();
    _walletConnections.push_back(QObject::connect(transactions, &QAbstractItemModel::modelReset,
        model, [bridge]() { [bridge notifyTransactions]; }));
    _walletConnections.push_back(QObject::connect(transactions, &QAbstractItemModel::dataChanged,
        model, [bridge]() { [bridge notifyTransactions]; }));
    _walletConnections.push_back(QObject::connect(transactions, &QAbstractItemModel::rowsInserted,
        model, [bridge]() { [bridge notifyTransactions]; }));
    _walletConnections.push_back(QObject::connect(transactions, &QAbstractItemModel::rowsRemoved,
        model, [bridge]() { [bridge notifyTransactions]; }));
    [self notifyAddress];
    [self notifyWallet];
    [self refreshBalances];
    [self notifyTransactions];
}

- (void)setClientModel:(ClientModel*)model
{
    if (_clientModel == model) return;
    Disconnect(_clientConnections);
    _clientModel = model;
    if (!model) return;
    __weak GeoCoinCoreBridge* bridge = self;
    _clientConnections.push_back(QObject::connect(model, &ClientModel::numConnectionsChanged, model,
        [bridge](int count) {
            GeoCountsBlock notify = bridge.onConnections;
            if (notify) notify(count);
        }));
    _clientConnections.push_back(QObject::connect(model, &ClientModel::numBlocksChanged, model,
        [bridge](int count) {
            GeoCountsBlock notify = bridge.onBlocks;
            if (notify) notify(count);
        }));
    _clientConnections.push_back(QObject::connect(model, &ClientModel::miningChanged, model,
        [bridge](bool enabled, int threads, double hashrate, quint64 accepted) {
            GeoMiningStatusBlock notify = bridge.onMining;
            if (notify) notify(enabled, threads, hashrate, static_cast<NSInteger>(accepted), 0);
        }));
    _clientConnections.push_back(QObject::connect(model, &ClientModel::message, model,
        [bridge](const QString& title, const QString& text, unsigned int style) {
            GeoMessageBlock notify = bridge.onMessage;
            if (notify) notify(GeoString(title), GeoString(text), style);
        }));
    if (self.onConnections) self.onConnections(self.numConnections);
    if (self.onBlocks) self.onBlocks(self.numBlocks);
    if (self.onMining) self.onMining(self.miningEnabled, self.miningThreads, self.miningHashrate,
                                     self.miningAccepted, 0);
}

- (NSInteger)numConnections { return _clientModel ? _clientModel->getNumConnections() : 0; }
- (NSInteger)numBlocks { return _clientModel ? _clientModel->getNumBlocks() : 0; }
- (double)syncProgress { return _syncProgress; }

- (void)updateSyncProgress:(double)progress current:(NSInteger)current target:(NSInteger)target
{
    (void)current;
    (void)target;
    _syncProgress = progress;
    if (self.onSyncProgress) self.onSyncProgress(progress);
}

- (double)miningHashrate { return _clientModel ? _clientModel->getHashrate() : 0; }
- (NSString*)networkName { return self.selectedNetwork; }
- (NSString*)walletName
{
    if (!self.isWalletLoaded) return @"";
    return GeoString(QFileInfo(_walletModel->getWalletFilePath()).fileName());
}
- (BOOL)isWalletLoaded { return _walletModel && _walletModel->isWalletLoaded(); }
- (BOOL)isPruned
{
    auto service = _clientModel ? _clientModel->getNodeComponents().services.info : nullptr;
    return service && service->is_pruned_mode();
}
- (NSString*)primaryAddress { return self.isWalletLoaded ? GeoString(_walletModel->getAddress()) : nil; }

- (void)refreshBalances
{
    const bool loaded = self.isWalletLoaded;
    const double available = loaded ? GeoCoins(_walletModel->getBalance()) : 0;
    const double pending = loaded ? GeoCoins(_walletModel->getUnconfirmedBalance()) : 0;
    const double immature = loaded ? GeoCoins(_walletModel->getImmatureBalance()) : 0;
    const double watch = loaded ? GeoCoins(_walletModel->getWatchOnlyBalance()) : 0;
    if (self.onBalances) self.onBalances(available, pending, immature, watch);
    for (GeoBalanceBlock observer in [_balanceObservers copy]) observer(available, pending, immature, watch);
}

- (NSArray<NSNumber*>*)watchOnlyBalances
{
    if (!self.isWalletLoaded) return @[@0, @0, @0];
    return @[@(GeoCoins(_walletModel->getWatchOnlyBalance())),
             @(GeoCoins(_walletModel->getWatchOnlyUnconfirmedBalance())),
             @(GeoCoins(_walletModel->getWatchOnlyImmatureBalance()))];
}

- (void)startMiningThreads:(NSInteger)threads
{
    NSString* error = [self requestCpuMiningWithThreads:threads];
    if (error && self.onMessage) self.onMessage(@"CPU Mining", error, 2);
}
- (void)stopMining { [self startMiningThreads:0]; }
- (NSString*)requestCpuMiningWithThreads:(NSInteger)threads
{
    if (!_miningController) return @"Mining is unavailable until Core finishes starting.";
    if (threads < 0 || threads > INT_MAX) return @"Invalid CPU thread count.";
    QString error;
    return _miningController->requestNativeCpuMining(static_cast<int>(threads), &error) ? nil : GeoString(error);
}
- (BOOL)isCpuMiningStopping { return _miningController && _miningController->hasPendingMiningAction(); }
- (NSInteger)miningThreads { return _clientModel ? _clientModel->getMiningThreads() : 1; }
- (BOOL)miningEnabled { return _clientModel && _clientModel->getMiningEnabled(); }
- (NSInteger)miningAccepted { return _clientModel ? _clientModel->getMinedBlocksAccepted() : 0; }
- (NSInteger)miningRejected { return 0; }

static NSImage* GeoResourceImage(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return nil;
    const QByteArray bytes = file.readAll();
    return [[NSImage alloc] initWithData:[NSData dataWithBytes:bytes.constData()
                                                       length:static_cast<NSUInteger>(bytes.size())]];
}

- (NSImage*)logoImage { return GeoResourceImage(QStringLiteral(":/images/logo")); }

- (NSImage*)backgroundImageForTheme:(NSString*)theme
{
    return GeoResourceImage([theme isEqualToString:@"dark"] ? QStringLiteral(":/images/background_dark")
                                                            : QStringLiteral(":/images/background"));
}

- (NSString*)localized:(NSString*)source context:(NSString*)context
{
    return GeoString(QCoreApplication::translate(context.UTF8String, source.UTF8String));
}

- (GeoGpuMinerLauncher*)gpuLauncher
{
    if (!_gpuLauncher) _gpuLauncher = [GeoGpuMinerLauncher new];
    return _gpuLauncher;
}

- (void)observeGpuStatus:(GeoGpuStatsBlock)observer
{
    [_gpuObservers addObject:[observer copy]];
}

- (void)publishGpuStatus
{
    if (!_miningController) return;
    NSDictionary* status = @{@"status": GeoString(_miningController->nativeGpuMiningStatus()),
                             @"backend": GeoString(_miningController->nativeGpuBackend()),
                             @"hashrate": @(_gpuHashrate), @"accepted": @(_gpuAccepted),
                             @"rejected": @(_gpuRejected)};
    if (self.onGpuStats) self.onGpuStats(status);
    for (GeoGpuStatsBlock observer in [_gpuObservers copy]) observer(status);
}

- (void)setMiningController:(MiningPage*)controller
{
    Disconnect(_miningConnections);
    _miningController = controller;
    if (!controller) return;
    __weak GeoCoinCoreBridge* weakBridge = self;
    _miningConnections.push_back(QObject::connect(controller, &MiningPage::nativeGpuStatusChanged,
        controller, [weakBridge](const QString&) { [weakBridge publishGpuStatus]; }));
    _miningConnections.push_back(QObject::connect(controller, &MiningPage::gpuMiningChanged, controller,
        [weakBridge](bool running, double rate, int accepted, int rejected) {
            GeoCoinCoreBridge* bridge = weakBridge;
            if (!bridge) return;
            bridge->_gpuHashrate = running ? rate : 0;
            bridge->_gpuAccepted = accepted;
            bridge->_gpuRejected = rejected;
            [bridge publishGpuStatus];
        }));
    _miningConnections.push_back(QObject::connect(controller, &MiningPage::gpuMiningStopped, controller,
        [weakBridge]() {
            GeoCoinCoreBridge* bridge = weakBridge;
            if (!bridge) return;
            [bridge publishGpuStatus];
            if (bridge.onGpuExit) bridge.onGpuExit(0);
        }));
}

- (NSString*)startGpuMiningWithBackend:(NSString*)backend
                             deviceIds:(NSArray<NSString*>*)ids intensity:(NSInteger)intensity
{
    if (!_miningController) return @"Mining is unavailable until Core finishes starting.";
    if (intensity < 1 || intensity > INT_MAX) return @"Invalid GPU intensity.";
    QStringList devices;
    for (NSString* identifier in ids) devices.append(QString::fromNSString(identifier));
    QString error;
    BOOL started = _miningController->startNativeGpuMining(QString::fromNSString(backend), devices,
                                                           static_cast<int>(intensity), &error);
    [self publishGpuStatus];
    return started ? nil : GeoString(error);
}
- (void)stopGpuMining { if (_miningController) _miningController->stopGpuMining(); }
- (BOOL)isGpuMiningActive { return _miningController && _miningController->isGpuMiningActive(); }
- (BOOL)isGpuMiningStopping { return _miningController && _miningController->isNativeGpuMiningStopping(); }

static NSArray<NSDictionary<NSString*, NSString*>*>* GeoTableSnapshot(QAbstractItemModel* model)
{
    if (!model) return @[];
    NSMutableArray* rows = [NSMutableArray array];
    const QModelIndex root;
    const int columns = model->columnCount(root);
    for (int row = 0; row < model->rowCount(root); ++row) {
        NSMutableDictionary* cells = [NSMutableDictionary dictionary];
        for (int col = 0; col < columns; ++col) {
            cells[[NSString stringWithFormat:@"col%d", col]] =
                GeoString(model->data(model->index(row, col), Qt::DisplayRole).toString());
        }
        if (qobject_cast<TransactionTableModel*>(model)) {
            const auto index = model->index(row, TransactionTableModel::Status);
            cells[@"txid"] = GeoString(index.data(TransactionTableModel::TxHashRole).toString());
            cells[@"address"] = GeoString(index.data(TransactionTableModel::FullAddressRole).toString());
            cells[@"type"] = GeoString(index.data(TransactionTableModel::TypeRole).toString());
        }
        [rows addObject:cells];
    }
    return rows;
}

- (NSArray<NSDictionary<NSString*, NSString*>*>*)transactions
{
    return _walletModel ? GeoTableSnapshot(_walletModel->getTransactionTableModel()) : @[];
}

- (NSArray<NSDictionary<NSString*, NSString*>*>*)addressBook
{
    return _walletModel ? GeoTableSnapshot(_walletModel->getAddressTableModel()) : @[];
}

- (NSString*)standardFee
{
    return GeoString(GUIUtil::formatAmount(_walletModel ? _walletModel->standardRelayFee()
                                                       : WalletModel::standardRelayFee(nullptr)));
}

- (void)confirmSendCoinsTo:(NSString*)destination amount:(NSString*)amountText fee:(NSString*)feeText
                  window:(NSWindow*)window completion:(GeoEmptyBlock)completion
{
    auto translate = [self](NSString* source) { return [self localized:source context:@"SendCoinsPage"]; };
    auto report = [self, translate](NSString* message) {
        if (self.onMessage) self.onMessage(translate(@"Send Coins"), message, 0);
    };
    if (!self.isWalletLoaded) {
        report(translate(@"Load or create a wallet before sending."));
        return;
    }
    const QPointer<WalletModel> model(_walletModel);
    const QString walletPath = model->getWalletFilePath();
    const QString address = QString::fromNSString(destination).trimmed();
    const geocoin::Address parsedAddress(address.toStdString());
    if (!geocoin::crypto::verify_address(parsedAddress)) {
        report(translate(@"Invalid recipient address."));
        return;
    }
    qint64 amount = 0;
    qint64 fee = 0;
    qint64 total = 0;
    if (!GUIUtil::parseAmount(QString::fromNSString(amountText), amount) || amount <= 0 ||
        !GUIUtil::parseAmount(QString::fromNSString(feeText), fee) || fee <= 0 ||
        !GUIUtil::addAmounts(amount, fee, total)) {
        report(translate(@"Enter a valid positive amount and fee."));
        return;
    }
    const qint64 minimum = model->estimateFee(amount);
    if (minimum <= 0 || fee < minimum) {
        report(GeoString(QStringLiteral("Fee below the current relay minimum of %1 GC.")
            .arg(GUIUtil::formatAmount(qMax(minimum, model->standardRelayFee())))));
        return;
    }
    if (total > model->getBalance()) {
        report(translate(@"Insufficient balance."));
        return;
    }

    NSAlert* confirm = [NSAlert new];
    confirm.messageText = translate(@"Confirm Send");
    confirm.informativeText = GeoString(QStringLiteral("Amount: %1 GC\nFee: %2 GC\nTotal: %3 GC\n\nTo: %4")
        .arg(GUIUtil::formatAmount(amount), GUIUtil::formatAmount(fee), GUIUtil::formatAmount(total), address));
    [confirm addButtonWithTitle:translate(@"Send")].keyEquivalent = @"";
    [confirm addButtonWithTitle:translate(@"Cancel")].keyEquivalent = @"\r";

    void (^send)(NSModalResponse) = ^(NSModalResponse response) {
        if (response != NSAlertFirstButtonReturn || !model || !model->isWalletLoaded() ||
            model->getWalletFilePath() != walletPath) return;
        QString password;
        GUIUtil::ScopedQStringCleanser clean(password);
        if (model->isWalletEncrypted()) {
            NSAlert* prompt = [NSAlert new];
            prompt.messageText = translate(@"Wallet Password");
            prompt.informativeText = translate(@"Enter wallet password to sign transaction:");
            NSSecureTextField* field = [[NSSecureTextField alloc] initWithFrame:NSMakeRect(0, 0, 280, 24)];
            prompt.accessoryView = field;
            [prompt addButtonWithTitle:translate(@"OK")];
            [prompt addButtonWithTitle:translate(@"Cancel")];
            [prompt layout];
            prompt.window.initialFirstResponder = field;
            const NSModalResponse answer = [prompt runModal];
            if (answer == NSAlertFirstButtonReturn) password = QString::fromNSString(field.stringValue);
            field.stringValue = @"";
            if (answer != NSAlertFirstButtonReturn || !model || model->getWalletFilePath() != walletPath) return;
            if (!model->verifyPassword(password)) {
                report(translate(@"Incorrect password. Transaction cancelled."));
                return;
            }
        }
        const auto status = model->sendCoins(address, amount, fee, password);
        if (status == WalletModel::OK) {
            if (completion) completion();
            report(translate(@"Transaction sent."));
        } else {
            report([NSString stringWithFormat:@"Unable to send the transaction (%d).", static_cast<int>(status)]);
        }
    };
    if (window) [confirm beginSheetModalForWindow:window completionHandler:send];
    else send([confirm runModal]);
}

- (NSInteger)sendCoinsTo:(NSString*)address amount:(double)amountGEO fee:(double)feeGEO
{
    [self confirmSendCoinsTo:address amount:[NSString stringWithFormat:@"%.8f", amountGEO]
                         fee:[NSString stringWithFormat:@"%.8f", feeGEO] window:NSApp.keyWindow completion:^{}];
    return WalletModel::OK;
}

@end
