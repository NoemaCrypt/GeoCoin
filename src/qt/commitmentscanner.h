// Copyright (c) 2026 The GeoCoin developers
// Distributed under the MIT software license

#ifndef GEOCOIN_QT_COMMITMENTSCANNER_H
#define GEOCOIN_QT_COMMITMENTSCANNER_H

#include "geocoin/types.hpp"
#include <QObject>
#include <QString>
#include <QStringList>
#include <QSet>
#include <memory>

class QThread;
class CommitmentScannerWorker;

namespace geocoin {
    namespace state {
        class ChainState;
    }
}

/**
 * @brief Умный сканер блокчейна для автоматического сбора commitments
 *
 * Особенности:
 * - Запускается только когда нужно (не постоянно)
 * - Сканирует только новые блоки (с last_scanned_height)
 * - Продолжает контроль после сбора подписей, чтобы заметить reorg
 * - Можно запустить/остановить вручную
 */
class CommitmentScanner : public QObject
{
    Q_OBJECT

public:
    /**
     * @brief Конструктор сканера
     * @param targetHash - delegation_hash или spending_hash для поиска
     * @param multisigAddress - адрес мультиподписи
     * @param requiredM - сколько подписей нужно собрать
     * @param chainState - доступ к блокчейну
     * @param parent - родительский объект Qt
     */
    explicit CommitmentScanner(const QString& targetHash,
                              const QString& multisigAddress,
                              int requiredM,
                              std::shared_ptr<geocoin::state::ChainState> chainState,
                              QObject* parent = nullptr,
                              const QStringList& allowedSignerAddresses = {});

    ~CommitmentScanner() override;

    /**
     * @brief Запустить автоматическое сканирование
     * @param intervalSeconds - интервал между проверками (по умолчанию 30 сек)
     */
    void startAutoScan(int intervalSeconds = 30);

    /**
     * @brief Остановить автоматическое сканирование
     */
    void stopAutoScan();

    /**
     * @brief Проверить блокчейн прямо сейчас (ручная проверка)
     */
    void scanNow();

    /**
     * @brief Активно ли сканирование
     */
    bool isScanning() const { return scanning_; }

    /**
     * @brief Сколько подписей уже найдено
     */
    int foundCount() const { return foundTxids_.size(); }

    /**
     * @brief Сколько подписей нужно
     */
    int requiredCount() const { return requiredM_; }

    /**
     * @brief Последняя проверенная высота блока
     */
    geocoin::Height lastScannedHeight() const { return lastScannedHeight_; }

    /** Pure reorg decision helper used by the worker and deterministic smoke checks. */
    static bool anchorRequiresRescan(geocoin::Height lastScannedHeight,
                                     bool hasAnchor,
                                     const geocoin::Hash256& anchorHash,
                                     geocoin::Height currentHeight,
                                     bool activeAnchorAvailable,
                                     const geocoin::Hash256& activeAnchorHash);

    /** Pure allowlist/uniqueness predicate used by scanning and smoke checks. */
    static bool signerCanBeCollected(const QString& signerAddress,
                                     const QSet<QString>& allowedSigners,
                                     const QSet<QString>& collectedSigners);

    /** Pure reconciliation predicate for one previously collected commitment. */
    static bool storedCommitmentRemainsActive(geocoin::Height commitmentHeight,
                                              const geocoin::Hash256& commitmentBlockHash,
                                              geocoin::Height currentHeight,
                                              bool activeBlockAvailable,
                                              const geocoin::Hash256& activeBlockHash);

    /** Verify that a commitment is present on the current active chain and matches this draft. */
    static bool isCommitmentActive(
        const QString& txid,
        const QString& targetHash,
        const QString& multisigAddress,
        const std::shared_ptr<geocoin::state::ChainState>& chainState,
        const QStringList& allowedSignerAddresses = {},
        QString* signerAddressOut = nullptr);

signals:
    /**
     * @brief Найден новый commitment
     * @param txid - TXID commitment транзакции
     * @param signerAddress - адрес подписанта
     */
    void commitmentFound(const QString& txid, const QString& signerAddress);

    /** A previously scanner-derived commitment was orphaned by a reorg. */
    void commitmentInvalidated(const QString& txid, const QString& signerAddress);

    /**
     * @brief Собраны все необходимые подписи
     */
    void allCommitmentsCollected();

    /**
     * @brief Ошибка при сканировании
     * @param error - описание ошибки
     */
    void scanError(const QString& error);

    /**
     * @brief Обновление статуса сканирования
     * @param message - статусное сообщение
     */
    void statusUpdate(const QString& message);

private slots:
private:
    QThread* workerThread_;
    CommitmentScannerWorker* worker_;
    QString targetHash_;
    QString multisigAddress_;
    int requiredM_;
    std::shared_ptr<geocoin::state::ChainState> chainState_;
    QSet<QString> foundTxids_;
    geocoin::Height lastScannedHeight_;
    int scanCount_;  // Счетчик сканирований для статистики
    bool scanning_;
};

#endif // GEOCOIN_QT_COMMITMENTSCANNER_H
