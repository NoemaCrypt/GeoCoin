#include "geocoin/core/blockchain.hpp"
#include "geocoin/core/blockchain_internal.hpp"
#include "geocoin/consensus/consensus.hpp"
#include "geocoin/consensus/proof_of_work.hpp"
#include "geocoin/consensus/chainparams.hpp"
#include "geocoin/utils/logger.hpp"
#include "geocoin/utils/metrics.hpp"

#include <algorithm>
#include <functional>
#include <iostream>
#include <vector>

namespace geocoin::core {

bool Blockchain::process_new_block(
    const Block& block,
    consensus::ValidationState& state
) {
    // Reject an invalid header/PoW before parsing the block body deeply or
    // verifying post-quantum signatures.  Otherwise a peer can spend almost no
    // work on a header and make us perform many expensive signature checks.
    if (!consensus::check_block_header(block.header, state)) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        record_block_reject_(state, false);
        return false;
    }

    const chain::BlockIndex* prev_index = nullptr;
    const bool is_genesis = (block.header.prev_block_hash == Hash256{});
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        if (!is_genesis) {
            prev_index = find_block_index(block.header.prev_block_hash);
            if (!prev_index) {
                state.error("prev block not found");
                record_block_reject_(state, false);
                return false;
            }
            if (prev_index->is_failed()) {
                state.invalid(
                    consensus::RejectCode::INVALID,
                    "bad-prevblk-failed",
                    "prev block failed validation"
                );
                record_block_reject_(state, false);
                return false;
            }
        }
    }

    if (!consensus::contextual_check_block_header(block.header, state, prev_index)) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        record_block_reject_(state, false);
        return false;
    }

    // The body check is intentionally outside the global lock so independent
    // valid-PoW candidates can be checked concurrently.
    if (!consensus::check_block(block, state)) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        record_block_reject_(state, false);
        return false;
    }

    std::lock_guard<std::recursive_mutex> lock(mutex_);
    skip_expensive_block_verify_ = true;
    struct VerifyModeGuard {
        bool& flag;
        ~VerifyModeGuard() { flag = false; }
    } guard{skip_expensive_block_verify_};

    // Этап 2: Принять блок в индекс
    chain::BlockIndex* index = nullptr;
    if (!accept_block(block, state, &index)) {
        record_block_reject_(state, false);
        return false;
    }

    // Этап 3: Активировать лучшую цепь
    const bool ok = activate_best_chain(state, &block);
    if (!ok) {
        const bool policy_reject =
            (state.get_reject_code() == consensus::RejectCode::CHECKPOINT) &&
            (state.get_reject_reason().rfind("policy-checkpoint", 0) == 0);
        record_block_reject_(state, policy_reject);
    }
    return ok;
}

bool Blockchain::accept_block(
    const Block& block,
    consensus::ValidationState& state,
    chain::BlockIndex** out_index
) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    if (out_index) {
        *out_index = nullptr;
    }

    // Проверяем, нет ли уже блока
    Hash256 block_hash = block.get_hash();
    chain::BlockIndex* known_index = find_block_index(block_hash);
    if (known_index && known_index->is_failed()) {
        return state.invalid(
            consensus::RejectCode::DUPLICATE,
            "duplicate-invalid",
            "block is already known to be invalid"
        );
    }
    // Находим предыдущий блок
    chain::BlockIndex* prev_index = nullptr;
    const bool is_genesis = (block.header.prev_block_hash == Hash256{});
    if (!is_genesis) {
        prev_index = find_block_index(block.header.prev_block_hash);
        if (!prev_index) {
            return state.error("prev block not found");
        }
        if (prev_index->is_failed()) {
            return state.invalid(
                consensus::RejectCode::INVALID,
                "bad-prevblk-failed",
                "prev block failed validation"
            );
        }
    }

    const Height height = is_genesis ? 0 : (prev_index->height + 1);

    storage::BlockStorage::PrunedIndexAnchor anchor;
    if (block_storage_ && block_storage_->get_pruned_index_anchor(anchor)) {
        const auto* ancestor = height > anchor.height ? prev_index->get_ancestor(anchor.height) : nullptr;
        if (!ancestor || ancestor->get_block_hash() != anchor.hash) {
            return state.invalid(consensus::RejectCode::CHECKPOINT, "policy-checkpoint-pruned-history",
                                 "block precedes or diverges from retained history", false);
        }
    }
    if (known_index && known_index->has_data()) {
        if (out_index) *out_index = known_index;
        return true;
    }

    // Контекстная проверка
    if (!consensus::contextual_check_block(block, state, prev_index)) {
        return false;
    }

    // Добавляем в индекс
    chain::BlockIndex* index = known_index;
    if (!index) {
        index = add_to_block_index(block);
        if (!index) {
            return state.error("failed to add to index");
        }
    } else if (index->height != height || index->prev != prev_index) {
        record_permanent_failure(index);
        return state.error("known block index context mismatch");
    }

    index->height = height;

    // Вычисляем chainwork
    index->chain_work = compute_chainwork(prev_index, block.header.bits);

    // Строим skip указатель
    build_skip(index);

    // Сохраняем на диск
    if (!save_block_to_disk(block, index, height)) {
        return state.error("failed to save to disk");
    }
    if (block_storage_ &&
        !block_storage_->set_chainwork(index->get_block_hash(), index->chain_work)) {
        // Chainwork полностью пересчитывается из валидированных headers при
        // restart, поэтому это не consensus-failure. Запись нужна как
        // дополнительная проверка/ускорение и для диагностируемости storage.
        GEO_LOG_WARN(
            "blockchain",
            "Failed to persist chainwork for block " << HashToHex(index->get_block_hash())
        );
    }

    // Помечаем, что данные есть
    index->status |= chain::BlockStatus::HAVE_DATA;
    index->status |= chain::BlockStatus::VALID_TRANSACTIONS;

    // Добавляем в кандидаты
    block_index_candidates_.insert(index);

    if (out_index) {
        *out_index = index;
    }

    return true;
}

chain::BlockIndex* Blockchain::add_to_block_index(const Block& block) {
    Hash256 block_hash = block.get_hash();

    // Создаем новый BlockIndex
    auto index = std::make_unique<chain::BlockIndex>(block.header);
    index->hash = block_hash;
    index->sequence_id = next_sequence_id_++;

    // Находим предыдущий блок
    if (block.header.prev_block_hash != Hash256{}) {
        index->prev = find_block_index(block.header.prev_block_hash);
    }

    // Сохраняем в map
    chain::BlockIndex* ptr = index.get();
    block_index_[block_hash] = std::move(index);

    return ptr;
}

bool Blockchain::save_block_to_disk(const Block& block, chain::BlockIndex* index, Height height) {
    // Для Genesis верификация отключена.
    // На fast-path после check_block() повторная expensive verify не нужна.
    bool enforce_validation = (height > 0) && !skip_expensive_block_verify_;

    int32_t file;
    uint32_t pos;

    if (!block_storage_->save_block_with_pos(block, height, file, pos, enforce_validation)) {
        return false;
    }

    // Обновляем позицию на диске в BlockIndex
    index->block_pos.file = file;
    index->block_pos.pos = pos;

    return true;
}

} // namespace geocoin::core
