// Copyright (c) 2026 The GeoCoin developers
// Distributed under the MIT software license

#ifndef GEOCOIN_QT_CORPORATEPAGE_H
#define GEOCOIN_QT_CORPORATEPAGE_H

#include <QWidget>
#include <QJsonDocument>
#include <QJsonArray>
#include <QVector>
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
class QDoubleSpinBox;
class QStackedWidget;
class QTextEdit;
class QComboBox;
class QCheckBox;
class QTableWidget;
QT_END_NAMESPACE

/**
 * Страница корпоративного управления (Corporate Governance)
 * Поддерживает иерархическую мультиподпись с delegation proof
 */
class CorporatePage : public QWidget
{
    Q_OBJECT

public:
    explicit CorporatePage(QWidget *parent = nullptr);
    ~CorporatePage() override;

    void setClientModel(ClientModel *model);
    void setWalletModel(WalletModel *model);
    void applyTheme();

signals:
    // Founder signals - создание delegation proof
    void requestDelegationProofDraft(const QString& primaryConfigJson,
                                     const QString& delegatedConfigJson,
                                     quint64 validFrom,
                                     quint64 validUntil,
                                     int nonce,
                                     qint64 maxTotal,
                                     qint64 maxPerTx,
                                     int maxPercent);
    void requestCreateAndSignDelegationProof(const QString& primaryConfigJson,
                                            const QString& delegatedConfigJson,
                                            quint64 validFrom,
                                            quint64 validUntil,
                                            int nonce,
                                            qint64 maxTotal,
                                            qint64 maxPerTx,
                                            int maxPercent,
                                            const QString& privateKeyHex);
    void requestCreateAndSignDelegationProofWithWalletKey(
        const QString& primaryConfigJson,
        const QString& delegatedConfigJson,
        quint64 validFrom,
        quint64 validUntil,
        int nonce,
        qint64 maxTotal,
        qint64 maxPerTx,
        int maxPercent,
        const QString& walletAddress,
        const QString& walletPassword);
    void requestDelegationProofFinalize(const QString& draftJson,
                                       const QStringList& commitmentTxids,
                                       bool broadcast);

    // Director signals - делегированная трата
    void requestHierarchicalSpendDraft(const QString& delegationProofTxid,
                                       const QString& delegatedConfigJson,
                                       const QString& recipient,
                                       qint64 amount,
                                       qint64 fee);
    void requestHierarchicalSpendFinalize(const QString& draftJson,
                                         const QStringList& commitmentTxids,
                                         bool broadcast);

    // Signer signals - универсальное подписание
    void requestCommitmentSign(const QString& draftJson,
                              const QString& privateKeySource,
                              const QString& keyPath,
                              bool broadcastImmediately);

private slots:
    void onRoleChanged(int roleId);

    // Founder slots
    void onLoadPrimaryConfig();
    void onLoadDelegatedConfig();
    void onSavedPrimaryMultisigSelected(int index);
    void onSavedDelegatedMultisigSelected(int index);
    void onCreatePrimaryAddress();
    void onCreateDelegatedAddress();
    void onAddPrimaryToWallet();
    void onAddDelegatedToWallet();
    void onCreateDelegationProofDraft();
    void onCreateAndSignDelegationProof();
    void onDelegationCommitmentChanged();
    void onAutoScanDelegationToggled(bool checked);
    void onScanNowDelegation();
    void onFinalizeDelegationProof();
    void onSaveDelegationDraft();
    void onCopyDelegationDraft();
    void onCopyDelegationHash();
    void onShowDelegationHashQR();
    void onLoadDelegationCommitmentFile();

    // Director slots
    void onLoadDelegationProof();
    void onDelegatedAddressChanged();
    void onActiveDelegationProofSelected(int index);
    void onLoadDirectorDelegatedConfig();
    void onCreateHierarchicalSpendDraft();
    void onHierarchicalCommitmentChanged();
    void onAutoScanHierarchicalToggled(bool checked);
    void onScanNowHierarchical();
    void onFinalizeHierarchicalSpend();
    void onSaveHierarchicalDraft();
    void onCopyHierarchicalDraft();
    void onLoadHierarchicalCommitmentFile();

    // Signer slots
    void onSignerDraftChanged();
    void onLoadSignerDraft();
    void onKeySourceChanged(int sourceId);
    void onBrowseKeyFile();
    void onSignCommitment();

	    // Common slots
	    void updatePrimaryBalance();
	    void updateDelegatedBalance();
	    void updateDelegationAllowance();
	    void loadHistory();
    void onHistoryFilterChanged(int index);
    void onHistorySelectionChanged();
    void onHistoryLoadClicked();
    void onHistoryCopyHash();
    void onExportHistory();

private slots:
    // Handlers для ответов от WalletModel
    void handleDelegationProofDraftReady(const QString& draftJson, const QString& txHash);
    void handleDelegationProofDraftFailed(const QString& error);
    void handleCreateAndSignReady(const QString& delegationHash, const QString& myCommitmentTxid, const QString& draftJson);
    void handleCreateAndSignFailed(const QString& error);
    void handleDelegationProofFinalizeReady(const QString& finalHex, const QString& broadcastTxid);
    void handleDelegationProofFinalizeFailed(const QString& error);

    // Handlers для сканера commitments
    void onDelegationCommitmentFound(const QString& txid, const QString& signerAddress);
    void onDelegationCommitmentInvalidated(const QString& txid, const QString& signerAddress);
    void onAllDelegationCommitmentsCollected();
    void onDelegationScanError(const QString& error);
    void onDelegationScanStatus(const QString& message);

    void handleHierarchicalSpendDraftReady(const QString& draftJson, const QString& txHash);
    void handleHierarchicalSpendDraftFailed(const QString& error);
    void handleHierarchicalSpendFinalizeReady(const QString& finalHex, const QString& broadcastTxid);
    void handleHierarchicalSpendFinalizeFailed(const QString& error);

    // Handlers для сканера hierarchical commitments
    void onHierarchicalCommitmentFound(const QString& txid, const QString& signerAddress);
    void onHierarchicalCommitmentInvalidated(const QString& txid, const QString& signerAddress);
    void onAllHierarchicalCommitmentsCollected();
    void onHierarchicalScanError(const QString& error);
    void onHierarchicalScanStatus(const QString& message);

    void handleCommitmentReady(const QString& commitmentHex,
                               const QString& commitmentHash,
                               const QString& broadcastTxid);
    void handleCommitmentFailed(const QString& error);

private:
    void setupUi();
    QWidget* buildRoleSelector();
    QWidget* buildFounderPage();
    QWidget* buildDirectorPage();
    QWidget* buildSignerPage();
    QGroupBox* buildFounderPrimaryConfigBox();
    QGroupBox* buildFounderDelegatedConfigBox();
    QGroupBox* buildFounderParametersBox();
    QGroupBox* buildFounderDraftBox();
    QGroupBox* buildFounderCommitmentsBox();
    QGroupBox* buildDirectorDelegationProofBox();
    QGroupBox* buildDirectorSpendParametersBox();
    QGroupBox* buildDirectorDraftBox();
    QGroupBox* buildDirectorCommitmentsBox();
    QGroupBox* buildSignerDraftBox();
    QGroupBox* buildSignerInfoBox();
    QGroupBox* buildSignerKeyBox();
    QGroupBox* buildHistoryBox();

    void updateFounderState();
    void updateDelegationCommitmentState();
    void updateFounderCommitmentFields();  // Обновить поля для подписей учредителей
    bool ensureDelegationScanner();
    void resetDelegationScanner(bool clearCommitments);
    void resetDelegationOperation();
    void updateDirectorState();
    void updateHierarchicalCommitmentState();
    void updateDirectorCommitmentFields();  // Обновить поля для подписей директоров
    bool ensureHierarchicalScanner();
    void resetHierarchicalScanner(bool clearCommitments);
    void resetHierarchicalOperation();
    void updateSignerState();
    void updateKeySourceState(int sourceId);

    bool loadConfigFromFile(const QString& path, bool isPrimary);
    bool parseDelegationProofInfo(const QString& txid);
    bool isCurrentDelegationProofActive(QString& error) const;
    bool parseSignerDraft(const QString& jsonText);
    bool validateFounderInputs(QString& error,
                               qint64* maxTotal = nullptr,
                               qint64* maxPerTx = nullptr) const;
    bool delegationValidityTimestamps(quint64& validFrom,
                                      quint64& validUntil,
                                      QString& error) const;
    bool validateDirectorInputs(QString& error,
                                qint64* amount = nullptr,
                                qint64* fee = nullptr) const;
    bool isKeyInputReady(int sourceId) const;

    void loadSavedMultisigAddresses();
    void loadActiveDelegationProofs();
    void saveHistory();
    void refreshHistoryView();
    void addHistoryEntry(const QString& type,
                        const QString& status,
                        const QString& details,
                        const QString& payload = QString());

    void logInfo(const QString& message);
    void logError(const QString& message);
    void setFounderDraftInProgress(bool inProgress);
    void setDirectorDraftInProgress(bool inProgress);
    void setCommitmentInProgress(bool inProgress);
    void updateStatusBanner();
    bool saveWalletChanges();

    ClientModel *clientModel_;
    WalletModel *walletModel_;

    // Shared controls
    QButtonGroup *roleButtons_;
    QStackedWidget *roleStack_;
    QLabel *statusLabel_;
    QTextEdit *activityLog_;

    // Founder controls
    QComboBox *primaryMultisigCombo_;
    QLineEdit *primaryConfigEdit_;
    QSpinBox *primaryMSpin_;
    QSpinBox *primaryNSpin_;
    QLabel *primaryAddressLabel_;
    QLabel *primaryBalanceLabel_;
    QTextEdit *primaryParticipantsEdit_;

    QComboBox *delegatedMultisigCombo_;
    QLineEdit *delegatedConfigEdit_;
    QSpinBox *delegatedMSpin_;
    QSpinBox *delegatedNSpin_;
    QLabel *delegatedAddressLabel_;
    QTextEdit *delegatedParticipantsEdit_;

    QSpinBox *validFromSpin_;
    QSpinBox *validUntilSpin_;
    QSpinBox *nonceSpin_;
    QLineEdit *maxTotalSpin_;
    QLineEdit *maxPerTxSpin_;
    QSpinBox *maxPercentSpin_;

    QPushButton *createDelegationDraftButton_;
    QPushButton *createAndSignDelegationButton_;
    QTextEdit *delegationDraftEdit_;
    QLineEdit *delegationHashEdit_;
    QPushButton *copyDelegationHashButton_;
    QPushButton *showDelegationHashQRButton_;
    QLabel *delegationDraftHashLabel_;
    QPushButton *saveDelegationDraftButton_;
    QPushButton *copyDelegationDraftButton_;

    QList<QLineEdit*> delegationCommitmentEdits_;
    QPushButton *finalizeDelegationButton_;
    QCheckBox *finalizeDelegationBroadcastCheck_;
    QCheckBox *autoScanDelegationCheck_;
    QPushButton *scanNowDelegationButton_;
    QLabel *scanStatusDelegationLabel_;

    QString currentPrimaryAddress_;
    QString currentDelegatedAddress_;
    QString currentDelegationDraftJson_;
    QString currentDelegationHash_;
    QJsonDocument currentDelegationDraftDoc_;
    QJsonArray primaryConfigParticipants_;
    QJsonArray delegatedConfigParticipants_;
    bool primaryConfigLoaded_;
    bool delegatedConfigLoaded_;
    bool founderDraftInProgress_;

    // Director controls
    QLineEdit *delegatedAddressDirectorEdit_;
    QLineEdit *delegatedConfigDirectorEdit_;
    QComboBox *activeDelegationProofsCombo_;
    QLineEdit *delegationProofTxidEdit_;
    QLabel *delegationInfoLabel_;
    QLabel *delegationStatusLabel_;

    QLineEdit *recipientEdit_;
	    QLineEdit *spendAmountEdit_;
	    QLineEdit *spendFeeEdit_;
	    QLabel *availableBalanceLabel_;
	    QLabel *delegatedUtxoBalanceLabel_;
	    QLabel *delegationAllowanceLabel_;

    QPushButton *createHierarchicalDraftButton_;
    QLineEdit *hierarchicalSpendingHashEdit_;
    QPushButton *copyHierarchicalHashButton_;
    QPushButton *showHierarchicalHashQRButton_;
    QTextEdit *hierarchicalDraftEdit_;
    QLabel *hierarchicalDraftHashLabel_;
    QPushButton *saveHierarchicalDraftButton_;
    QPushButton *copyHierarchicalDraftButton_;

    QList<QLineEdit*> hierarchicalCommitmentEdits_;
    QPushButton *finalizeHierarchicalButton_;
    QCheckBox *finalizeHierarchicalBroadcastCheck_;
    QCheckBox *autoScanHierarchicalCheck_;
    QPushButton *scanNowHierarchicalButton_;
    QLabel *scanStatusHierarchicalLabel_;

    QString currentDelegationProofTxid_;
    QString currentHierarchicalDraftJson_;
    QString currentHierarchicalSpendingHash_;
    QJsonDocument currentHierarchicalDraftDoc_;
    bool delegationProofLoaded_;
    bool directorDraftInProgress_;

    // Commitment scanners
    class CommitmentScanner* delegationScanner_;
    class CommitmentScanner* hierarchicalScanner_;
    QHash<QString, QPointer<QLineEdit>> scannerDelegationFields_;
    QHash<QString, QPointer<QLineEdit>> scannerHierarchicalFields_;
    QString delegationScannerTarget_;
    QString delegationScannerAddress_;
    int delegationScannerRequired_ = 0;
    QString hierarchicalScannerTarget_;
    QString hierarchicalScannerAddress_;
    int hierarchicalScannerRequired_ = 0;

    // Signer controls
    QTextEdit *signerDraftEdit_;
    QPushButton *loadSignerDraftButton_;
    QLabel *signerInfoLabel_;
    QLabel *signerHashLabel_;

    QButtonGroup *keySourceButtons_;
    QComboBox *walletKeyCombo_;
    QLineEdit *fileKeyPathEdit_;
    QPushButton *browseKeyFileButton_;
    QLineEdit *manualKeyEdit_;
    QCheckBox *broadcastCheck_;
    QPushButton *signCommitmentButton_;
    QLabel *commitmentResultLabel_;

    QJsonDocument signerDraftDoc_;
    QString signerDraftHash_;
    bool signerDraftValid_;
    bool commitmentInProgress_;

    // History
    QTableWidget *historyTable_;
    QComboBox *historyFilterCombo_;
    QPushButton *historyLoadButton_;
    QPushButton *historyCopyHashButton_;
    QPushButton *historyExportButton_;
    QString historyFilePath_;
    QJsonArray historyEntries_;

    QVector<QMetaObject::Connection> walletConnections_;
};

#endif // GEOCOIN_QT_CORPORATEPAGE_H
