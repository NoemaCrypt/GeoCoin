#include "geocoin/chain/chain.hpp"
#include <algorithm>
#include <limits>

namespace geocoin::chain {

const BlockIndex* Chain::find_fork(const BlockIndex* index) const {
    if (!index) {
        return nullptr;
    }

    if (index->height > height()) {
        index = index->get_ancestor(height());
        // КРИТИЧНО: get_ancestor() может вернуть nullptr
        if (!index) {
            return nullptr;
        }
    }

    while (index && !contains(index)) {
        index = index->prev;
    }

    return index;
}

void Chain::set_tip(BlockIndex* index) {
    if (!index) {
        chain_.clear();
        return;
    }

    // КРИТИЧНО: Проверяем overflow при height + 1
    if (index->height >= std::numeric_limits<Height>::max()) {
        chain_.clear();
        return;
    }

    // Строим цепь от Genesis до tip
    chain_.resize(index->height + 1);

    while (index) {
        chain_[index->height] = index;
        // Проверяем перед разыменованием prev
        index = index->prev;  // May be nullptr
    }
}

std::vector<Hash256> Chain::get_locator(const BlockIndex* index) const {
    std::vector<Hash256> locator;

    if (!index) {
        index = tip();
    }

    if (!index) {
        return locator;
    }

    Height step = 1;
    Height height = index->height;

    while (height >= 0) {
        // Добавляем блок на текущей высоте
        BlockIndex* block = (*this)[height];
        if (block) {
            locator.push_back(block->get_block_hash());
        }

        // Вычисляем следующую высоту
        if (height == 0) {
            break;
        }

        if (height <= step) {
            height = 0;
        } else {
            height -= step;
        }

        // Увеличиваем шаг экспоненциально
        if (locator.size() > 10) {
            step *= 2;
        }
    }

    return locator;
}

} // namespace geocoin::chain
