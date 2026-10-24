// Copyright (c) 2026 The GeoCoin developers
// Distributed under the MIT software license

#ifndef GEOCOIN_QT_MULTISIGPAGE_H
#define GEOCOIN_QT_MULTISIGPAGE_H

#include <QWidget>
#include <QJsonDocument>
#include <QJsonArray>
#include <QVector>
#include <QMetaObject>
#include <QHash>
#include <QPointer>

class ClientModel;
class WalletModel;

QT_BEGIN_NAMESPACE
class QButtonGroup;
class QGroupBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QRadioButton;
class QSpinBox;
class QStackedWidget;
class QTextEdit;
class QComboBox;
class QCheckBox;
class QMessageBox;
class QTableWidget;
QT_END_NAMESPACE

/**
 * Multisig workflow page (Initiator / Signer UX)
 */
class MultisigPage : public QWidget
{
    Q_OBJECT

public:
    explicit MultisigPage(QWidget *parent = nullptr);
    ~MultisigPage() override;

    void setClientModel(ClientModel *model);
    void setWalletModel(WalletModel *model);
    void applyTheme();

signals:
    void requestDraftCreation(const QString& configJson,
                              const QString& recipient,
                              qint64 amount,
                              qint64 fee);
    void requestCreateAndSignMultisig(const QString& configJson,
                                     const QString& recipient,
                                     qint64 amount,
                                     qint64 fee,
                                     const QString& privateKeyHex);
    void requestCreateAndSignMultisigWithWalletKey(const QString& configJson,
                                                   const QString& recipient,
                                                   qint64 amount,
                                                   qint64 fee,
                                                   const QString& walletAddress,
                                                   const QString& walletPassword);
    void requestDraftFinalize(const QString& draftJson,
                              const QStringList& commitmentTxids,
                              bool broadcast);
    void requestCommitmentSign(const QString& draftJson,
                               const QString& privateKeySource,
                               const QString& keyPath,
                               bool broadcastImmediately);

private slots:
    void onRoleChanged(int roleId);
    void onBrowseConfig();
    void onCreateMultisigAddress();
    void onAddMultisigToWallet();
    void onSavedMultisigSelected(int index);
    void onLoadDraftJson();
    void onSaveDraft();
    void onCopyDraftJson();
    void onCreateDraft();
    void onCreateAndSignMultisig();
    void onCommitmentChanged();
    void onAutoScanMultisigToggled(bool checked);
    void onScanNowMultisig();
    void onFinalizeDraft();
    void onSignerDraftChanged();
    void onSignCommitment();
    void updateMultisigBalance();

private slots:
    void handleDraftReady(const QString& draftJson, const QString& txHash);
    void handleDraftFailed(const QString& error);
    void handleCreateAndSignReady(const QString& spendingHash, const QString& myCommitmentTxid, const QString& multisigSpendHex);
    void handleCreateAndSignFailed(const QString& error);
    void handleCommitmentReady(const QString& commitmentHex,
                               const QString& commitmentHash,
                               const QString& broadcastTxid);
    void handleCommitmentFailed(const QString& error);
    void handleFinalizeReady(const QString& finalHex, const QString& broadcastTxid);
    void handleFinalizeFailed(const QString& error);

    // Handlers для сканера
    void onMultisigCommitmentFound(const QString& txid, const QString& signerAddress);
    void onMultisigCommitmentInvalidated(const QString& txid, const QString& signerAddress);
    void onAllMultisigCommitmentsCollected();
    void onMultisigScanError(const QString& error);
    void onMultisigScanStatus(const QString& message);

    // Очистка истории
    void clearHistory();

private:
    void setupUi();
    QWidget* buildRoleSelector();
    QWidget* buildInitiatorPage();
    QWidget* buildSignerPage();
    QGroupBox* buildInitiatorConfigBox();
    QGroupBox* buildInitiatorTxBox();
    QGroupBox* buildDraftInfoBox();
    QGroupBox* buildCommitmentsBox();
    QGroupBox* buildSignerDraftBox();
    QGroupBox* buildSignerInfoBox();
    QGroupBox* buildSignerKeyBox();
    void applyGeoTheme(QWidget* widget);
    void updateInitiatorState();
    void updateCommitmentState();
    void updateSignerState();
    void updateKeySourceState(int sourceId);
    bool loadConfigFromFile(const QString& path);
    bool parseSignerDraft(const QString& jsonText);
    void loadSavedMultisigAddresses();
    void updateMultisigCommitmentFields();
    bool ensureMultisigScanner();
    void resetMultisigScanner(bool clearCommitments);
    void resetMultisigOperation();
    bool validateInitiatorInputs(QString& error,
                                 qint64* amount = nullptr,
                                 qint64* fee = nullptr) const;
    bool isKeyInputReady(int sourceId) const;
    QString computeDraftHash(const QJsonDocument& doc) const;

    ClientModel *clientModel_;
    WalletModel *walletModel_;

    // Shared controls
    QButtonGroup *roleButtons_;
    QStackedWidget *roleStack_;

    // Initiator controls
    QComboBox *savedMultisigCombo_;
    QLineEdit *configPathEdit_;
    QSpinBox *initiatorMSpin_;
    QSpinBox *initiatorNSpin_;
    QTextEdit *participantsEdit_;
    QLabel *multisigAddressLabel_;
    QLineEdit *receiverEdit_;
    QLineEdit *amountEdit_;
    QLineEdit *feeEdit_;
    QLabel *balanceLabel_;
    bool multisigBalanceKnown_;
    qint64 multisigAvailableBalance_;
    QPushButton *createDraftButton_;
    QPushButton *createAndSignButton_;
    QLineEdit *spendingHashEdit_;
    QPushButton *copySpendingHashButton_;
    QPushButton *showSpendingHashQRButton_;
    QTextEdit *draftJsonEdit_;
    QLabel *draftHashLabel_;
    QPushButton *saveDraftButton_;
    QPushButton *copyDraftButton_;
    QList<QLineEdit*> commitmentEdits_;
    QPushButton *finalizeButton_;
    QCheckBox *finalizeBroadcastCheck_;
    QCheckBox *autoScanMultisigCheck_;
    QPushButton *scanNowMultisigButton_;
    QLabel *scanStatusMultisigLabel_;
    QString currentMultisigAddress_;
    QString currentDraftHash_;
    QString currentSpendingHash_;
    QString currentMultisigSpendHex_;  // Сохраненная транзакция для финализации
    QJsonDocument currentDraftDoc_;
    QString currentDraftJson_;
    bool configLoaded_;
    QJsonArray configParticipants_;
    QVector<QMetaObject::Connection> walletConnections_;
    QMetaObject::Connection clientBlocksConnection_;
    QLabel *statusLabel_;
    QTextEdit *activityLog_;
    bool draftInProgress_;
    bool commitmentInProgress_;
    bool finalizeInProgress_;
    QTableWidget *historyTable_;
    QPushButton *historyLoadButton_;
    QString historyFilePath_;
    QJsonArray historyEntries_;
    QTimer *balanceUpdateTimer_;  // Таймер для throttling обновления баланса

    // Commitment scanner
    class CommitmentScanner* multisigScanner_;
    QHash<QString, QPointer<QLineEdit>> scannerMultisigFields_;
    QString multisigScannerTarget_;
    QString multisigScannerAddress_;
    int multisigScannerRequired_ = 0;

    // Signer controls
    QTextEdit *signerDraftEdit_;
    QLabel *signerSummaryLabel_;
    QLabel *signerHashLabel_;
    QButtonGroup *keySourceButtons_;
    QComboBox *walletKeyCombo_;
    QLineEdit *fileKeyPathEdit_;
    QLineEdit *manualKeyEdit_;
    QCheckBox *broadcastCheck_;
    QPushButton *signCommitmentButton_;
    QJsonDocument signerDraftDoc_;
    QString signerDraftHash_;
    bool signerDraftValid_;

    void logInfo(const QString& message);
    void logError(const QString& message);
    void setDraftInProgress(bool inProgress);
    void setCommitmentInProgress(bool inProgress);
    void setFinalizeInProgress(bool inProgress);
    void updateStatusBanner();
    bool saveWalletChanges();
    void addHistoryEntry(const QString& type,
                         const QString& status,
                         const QString& details,
                         const QString& payload = QString());
    void loadHistory();
    void saveHistory();
    void refreshHistoryView();
    void onHistorySelectionChanged();
    void onHistoryLoadClicked();

    // Pending transactions (незавершенные транзакции)
    void savePendingTransaction();
    void loadPendingTransaction();
    void clearPendingTransaction();
    bool hasPendingTransaction() const;
    void schedulePendingRestorePrompt();
    QString pendingTransactionFilePath_;
};

#endif // GEOCOIN_QT_MULTISIGPAGE_H
