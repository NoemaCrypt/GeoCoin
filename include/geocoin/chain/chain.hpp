#pragma once

#include "geocoin/chain/block_index.hpp"
#include <vector>

namespace geocoin::chain {

/**
 * Активная цепь блоков
 * Аналог CChain из Bitcoin
 *
 * Представляет текущую активную цепь от Genesis до Tip.
 * Обеспечивает быстрый доступ к блокам по высоте O(1).
 */
class Chain {
public:
    Chain() = default;

    // Получить tip (последний блок) цепи
    BlockIndex* tip() const {
        return chain_.empty() ? nullptr : chain_.back();
    }

    // Получить высоту цепи
    Height height() const {
        return chain_.empty() ? -1 : chain_.back()->height;
    }

    // Получить блок по высоте
    BlockIndex* operator[](Height height) const {
        if (height < 0 || height >= static_cast<Height>(chain_.size())) {
            return nullptr;
        }
        return chain_[height];
    }

    // Проверить, содержит ли цепь блок
    bool contains(const BlockIndex* index) const {
        if (!index) return false;
        if (index->height < 0 || index->height >= static_cast<Height>(chain_.size())) {
            return false;
        }
        return chain_[index->height] == index;
    }

    // Найти форк между этой цепью и блоком
    // Возвращает последний общий блок
    const BlockIndex* find_fork(const BlockIndex* index) const;

    // Установить новый tip
    // Строит цепь от Genesis до нового tip
    void set_tip(BlockIndex* index);

    // Получить Genesis блок
    BlockIndex* genesis() const {
        return chain_.empty() ? nullptr : chain_[0];
    }

    // Получить следующий блок после данного (в активной цепи)
    BlockIndex* next(const BlockIndex* index) const {
        if (!index || !contains(index)) {
            return nullptr;
        }
        Height next_height = index->height + 1;
        if (next_height >= static_cast<Height>(chain_.size())) {
            return nullptr;
        }
        return chain_[next_height];
    }

    // Очистить цепь
    void clear() {
        chain_.clear();
    }

    // Размер цепи
    size_t size() const {
        return chain_.size();
    }

    // Проверить, пуста ли цепь
    bool empty() const {
        return chain_.empty();
    }

    // Получить локатор блоков для синхронизации
    // Возвращает хеши блоков с экспоненциальным шагом
    std::vector<Hash256> get_locator(const BlockIndex* index = nullptr) const;

private:
    // Вектор указателей на BlockIndex
    // Индекс в векторе = высота блока
    std::vector<BlockIndex*> chain_;
};

} // namespace geocoin::chain
