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

bool Blockchain::initialize() try {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    GEO_LOG_INFO("blockchain", "Initializing blockchain...");
    valid_ancestry_.clear();

    if (block_storage_ && block_storage_->has_incomplete_split_write()) {
        GEO_LOG_ERROR(
            "blockchain",
            "Incomplete split write between block index and chaindata. "
            "The unfinished operation is preserved; database recovery is required."
        );
        return false;
    }

    // Загружаем индекс блоков из хранилища
    GEO_LOG_INFO("blockchain", "Loading block index from disk...");

    // Получаем persistent header/index записи. Для старых datadir без bi:
    // BlockStorage восстановит записи из оставшихся b:<hash> тел блоков.
    std::vector<storage::BlockStorage::BlockIndexRecord> block_records =
        block_storage_->get_all_block_index_records();
    storage::BlockStorage::PrunedIndexAnchor pruned_anchor;
    const bool has_pruned_anchor =
        block_storage_ && block_storage_->get_pruned_index_anchor(pruned_anchor);

    GEO_LOG_INFO("blockchain", "Found " << block_records.size() << " block index records in storage");

    const std::string& expected_ruleset = consensus::Params().consensus_ruleset_id();
    std::string stored_ruleset;
    if (!block_storage_->get_consensus_ruleset_id(stored_ruleset)) {
        if (!block_records.empty() || has_pruned_anchor) {
            GEO_LOG_ERROR(
                "blockchain",
                "Non-empty block database has no consensus ruleset identifier. "
                    << "Discard this pre-launch datadir and synchronize from genesis."
            );
            return false;
        }
        if (!block_storage_->set_consensus_ruleset_id(expected_ruleset)) {
            GEO_LOG_ERROR("blockchain", "Failed to persist consensus ruleset identifier");
            return false;
        }
    } else if (stored_ruleset != expected_ruleset) {
        GEO_LOG_ERROR(
            "blockchain",
            "Consensus ruleset mismatch: database='" << stored_ruleset
                << "', binary='" << expected_ruleset
                << "'. This binary cannot open a datadir created under a different "
                   "consensus ruleset (including a pre-sidechain-v3 chain). "
                   "Use an empty network datadir and resync from genesis."
        );
        return false;
    }

    if (has_pruned_anchor) {
        GEO_LOG_INFO(
            "blockchain",
            "Found pruned index anchor: height=" << pruned_anchor.height
                << ", hash=" << HashToHex(pruned_anchor.hash)
        );
    }

    if (block_records.empty()) {
        // Как в Bitcoin: если блоков нет - создаём Genesis
        // Genesis - это константа сети, должен быть создан при первом запуске
        GEO_LOG_INFO("blockchain", "No blocks found, applying Genesis...");
        return apply_genesis();
    }

    // Вспомогательная функция для конвертации хеша в строку
    auto hash_to_string = [](const Hash256& hash) {
        std::ostringstream ss;
        for (auto byte : hash) {
            ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(byte);
        }
        return ss.str();
    };

    const Height UNSET_HEIGHT = std::numeric_limits<Height>::max();

    for (const auto& record : block_records) {
        if (record.header.get_hash() != record.hash) {
            GEO_LOG_ERROR(
                "blockchain",
                "Block-index hash does not match serialized header: key_hash="
                    << hash_to_string(record.hash)
                    << ", header_hash=" << hash_to_string(record.header.get_hash())
            );
            return false;
        }

        // Создаем BlockIndex
        auto index = std::make_unique<chain::BlockIndex>(record.header);
        index->hash = record.hash;
        index->sequence_id = next_sequence_id_++;

        if (record.height != UNSET_HEIGHT) {
            index->height = record.height;
        } else if (record.header.prev_block_hash == Hash256{}) {
            index->height = 0;
        }
        if (record.has_chain_work) {
            index->chain_work = record.chain_work;
        }

        index->block_pos.file = record.has_body ? 0 : -1;
        index->block_pos.pos = (record.height != UNSET_HEIGHT)
            ? static_cast<uint32_t>(record.height)
            : 0;

        index->status = chain::BlockStatus::VALID_HEADER;
        if (record.has_body) {
            index->status |= chain::BlockStatus::HAVE_DATA;
        }
        if (record.failed) {
            index->status |= chain::BlockStatus::FAILED_VALID;
        }

        // Добавляем в map
        block_index_[record.hash] = std::move(index);
    }

    GEO_LOG_INFO("blockchain", "Loaded " << block_index_.size() << " blocks into index");

    // ========================================================================
    // ВАЖНО: Проверяем Genesis блок (как в Bitcoin init.cpp:1031)
    // Если загруженная цепь имеет неверный genesis, завершаем работу сразу
    // ========================================================================
    if (!block_index_.empty()) {
        Hash256 expected_genesis = consensus::Params().get_genesis_hash();

        if (block_index_.count(expected_genesis) == 0 && !has_pruned_anchor) {
            std::ostringstream expected;
            for (int i = 0; i < 32; i++) {
                expected << std::hex << std::setw(2) << std::setfill('0')
                         << static_cast<int>(expected_genesis[i]);
            }

            GEO_LOG_ERROR(
                "blockchain",
                "Incorrect or no genesis block found. "
                << "Expected genesis hash for network '" << consensus::Params().network_id() << "': "
                << expected.str() << ". "
                << "This usually means you are using the wrong datadir for this network."
            );
            return false;
        }

        if (block_index_.count(expected_genesis) != 0) {
            GEO_LOG_INFO(
                "blockchain",
                "Genesis block verified for network: " << consensus::Params().network_id()
            );
        } else {
            GEO_LOG_INFO(
                "blockchain",
                "Genesis block index is compacted below pruned anchor for network: "
                    << consensus::Params().network_id()
            );
        }
    }

    // Связываем блоки (prev указатели) и проверяем целостность
    GEO_LOG_INFO("blockchain", "Linking blocks and checking chain integrity...");
    int orphan_blocks = 0;

    for (auto& [hash, index] : block_index_) {
        const bool is_pruned_anchor =
            has_pruned_anchor &&
            hash == pruned_anchor.hash;

        // Находим предыдущий блок (кроме genesis)
        if (index->header.prev_block_hash != Hash256{}) {
            if (is_pruned_anchor) {
                continue;
            }
            index->prev = find_block_index(index->header.prev_block_hash);
            if (!index->prev) {
                GEO_LOG_ERROR(
                    "blockchain",
                    "Block (hash=" << hash_to_string(hash) << ") references unknown prev block: "
                    << hash_to_string(index->header.prev_block_hash)
                );
                orphan_blocks++;
            }
        }
    }

    // Всегда восстанавливаем высоты из графа prev-ссылок. Нельзя доверять
    // сохраненной высоте: на одной высоте допустимо несколько fork-блоков.
    for (auto& [hash, index] : block_index_) {
        (void)hash;
        index->height = UNSET_HEIGHT;
    }

    enum class HeightVisit : uint8_t { UNSEEN, VISITING, DONE };
    std::map<chain::BlockIndex*, HeightVisit> height_visit;
    bool invalid_height_graph = false;
    const Hash256 expected_genesis = consensus::Params().get_genesis_hash();

    std::function<Height(chain::BlockIndex*)> compute_height = [&](chain::BlockIndex* idx) -> Height {
        if (!idx) {
            return UNSET_HEIGHT;
        }

        HeightVisit& visit = height_visit[idx];
        if (visit == HeightVisit::DONE) {
            return idx->height;
        }
        if (visit == HeightVisit::VISITING) {
            invalid_height_graph = true;
            return UNSET_HEIGHT;
        }
        visit = HeightVisit::VISITING;

        if (has_pruned_anchor && idx->get_block_hash() == pruned_anchor.hash) {
            idx->height = pruned_anchor.height;
            visit = HeightVisit::DONE;
            return idx->height;
        }

        if (idx->header.prev_block_hash == Hash256{}) {
            if (idx->get_block_hash() != expected_genesis) {
                invalid_height_graph = true;
                return UNSET_HEIGHT;
            }
            idx->height = 0;
            visit = HeightVisit::DONE;
            return 0;
        }
        if (!idx->prev) {
            idx->height = UNSET_HEIGHT;
            invalid_height_graph = true;
            return UNSET_HEIGHT;
        }
        const Height ph = compute_height(idx->prev);
        if (ph == UNSET_HEIGHT || ph == UNSET_HEIGHT - 1) {
            idx->height = UNSET_HEIGHT;
            invalid_height_graph = true;
            return UNSET_HEIGHT;
        }
        idx->height = ph + 1;
        visit = HeightVisit::DONE;
        return idx->height;
    };

    for (auto& [hash, index] : block_index_) {
        compute_height(index.get());
    }

    // КРИТИЧНО: Если есть блоки-сироты, цепь повреждена
    if (orphan_blocks > 0 || invalid_height_graph) {
        GEO_LOG_ERROR(
            "blockchain",
            "Invalid block-index graph (orphans=" << orphan_blocks << "). "
            << "Chain is broken. Please restart with -reindex to rebuild the database."
        );
        return false;
    }

    if (NodeVerboseEnabled()) {
        GEO_LOG_INFO("blockchain", "Chain integrity verified: all blocks properly linked");
    }

    // Вычисляем chainwork для всех блоков (как в Bitcoin - сортируем по высоте)
    if (NodeVerboseEnabled()) {
        GEO_LOG_INFO("blockchain", "Computing chainwork...");
    }

    // Сортируем блоки по высоте для последовательной обработки
    std::vector<std::pair<Height, chain::BlockIndex*>> sorted_blocks;
    sorted_blocks.reserve(block_index_.size());
    for (const auto& [hash, index] : block_index_) {
        (void)hash;
        if (index->height != UNSET_HEIGHT) {
            sorted_blocks.push_back({index->height, index.get()});
        }
    }
    std::sort(sorted_blocks.begin(), sorted_blocks.end(), [](const auto& a, const auto& b) {
        if (a.first != b.first) {
            return a.first < b.first;
        }
        return a.second->get_block_hash() < b.second->get_block_hash();
    });

    for (const auto& [height, index] : sorted_blocks) {
        // Вычисляем chainwork (как в Bitcoin: prev ? prev->chainwork : 0)
        const bool is_pruned_anchor =
            has_pruned_anchor &&
            index->get_block_hash() == pruned_anchor.hash &&
            index->height == pruned_anchor.height;
        if (is_pruned_anchor) {
            index->chain_work = pruned_anchor.chain_work;
        } else {
            index->chain_work = compute_chainwork(index->prev, index->header.bits);
        }

        // Строим skip указатель ТОЛЬКО если есть prev (как в Bitcoin)
        if (index->prev) {
            build_skip(index);
        }

        consensus::ValidationState checkpoint_state;
        if (!consensus::check_hardened_checkpoint(
                index->height,
                index->get_block_hash(),
                checkpoint_state)) {
            index->status |= chain::BlockStatus::FAILED_VALID;
        }
        if (!consensus::ProofOfWork::is_valid_target_bits(index->header.bits)) {
            index->status |= chain::BlockStatus::FAILED_VALID;
        }
        if (index->prev && index->prev->is_failed()) {
            index->status |= chain::BlockStatus::FAILED_CHILD;
        }

        // Добавляем в кандидаты только валидные блоки с вычисленной высотой
        // Genesis (height 0) или блоки с валидным prev
        if (!index->is_failed() &&
            index->height != UNSET_HEIGHT &&
            (index->height == 0 || index->prev || is_pruned_anchor)) {
            block_index_candidates_.insert(index);
        }
    }

    if (!recover_interrupted_active_chain_transition(has_pruned_anchor)) {
        return false;
    }

    if (NodeVerboseEnabled()) {
        GEO_LOG_INFO("blockchain", "Chainwork computed for all blocks");
    }

    // Восстанавливаем активную цепь
    if (NodeVerboseEnabled()) {
        GEO_LOG_INFO("blockchain", "Restoring active chain...");
    }

    // ========================================================================
    // Источник истины: индекс активной цепи в BlockStorage.
    // Если он задан — восстанавливаем ровно его (даже если это не max-work).
    // Если отсутствует (новый/старый datadir) — выбираем max-work и фиксируем.
    // ========================================================================
    chain::BlockIndex* best_tip = nullptr;
    bool restored_from_active_index = false;
    if (block_storage_) {
        Hash256 active_tip_hash;
        Height active_tip_height = 0;
        if (block_storage_->get_active_tip(active_tip_hash, active_tip_height)) {
            chain::BlockIndex* active_tip_index = find_block_index(active_tip_hash);
            if (!active_tip_index) {
                GEO_LOG_ERROR(
                    "blockchain",
                    "Active tip from BlockStorage not found in block index (db corruption?): height="
                        << active_tip_height
                );
                return false;
            }

            // Минимальная sanity-проверка: высота tip должна совпадать.
            if (active_tip_index->height != active_tip_height) {
                GEO_LOG_ERROR(
                    "blockchain",
                    "Active tip height mismatch: storage=" << active_tip_height
                        << ", index=" << active_tip_index->height
                );
                return false;
            }

            if (!is_chain_valid(active_tip_index)) {
                GEO_LOG_ERROR("blockchain", "Active chain from BlockStorage is not valid (failed/without data)");
                return false;
            }

            best_tip = active_tip_index;
            restored_from_active_index = true;
        }
    }

    if (!best_tip) {
        best_tip = find_most_work_chain();
    }

    if (best_tip) {
        chain_active_.set_tip(best_tip);
        GEO_LOG_INFO(
            "blockchain",
            "Active chain restored: height=" << chain_active_.height()
                << (restored_from_active_index ? " (from BlockStorage active index)" : " (from most-work)")
        );
    } else {
        GEO_LOG_ERROR("blockchain", "Failed to find valid chain");
        return false;
    }

    // Сначала восстанавливаем субъективный checkpoint относительно сохраненной
    // active-цепи. Нельзя выбирать most-work fork до этой проверки: иначе
    // запрещенная ветка обходила checkpoint простым перезапуском ноды.
    if (!load_policy_checkpoint()) {
        // У compacted pruned datadir проверенный anchor является единственной
        // доступной локальной точкой доверия. Восстанавливаем checkpoint только
        // если active-цепь действительно содержит тот же anchor.
        chain::BlockIndex* active_anchor = has_pruned_anchor
            ? chain_active_[pruned_anchor.height]
            : nullptr;
        if (active_anchor && active_anchor->get_block_hash() == pruned_anchor.hash &&
            block_storage_->set_policy_checkpoint(pruned_anchor.hash, pruned_anchor.height)) {
            has_policy_checkpoint_ = true;
            policy_checkpoint_height_ = pruned_anchor.height;
            policy_checkpoint_hash_ = pruned_anchor.hash;
        } else {
            GEO_LOG_ERROR(
                "blockchain",
                "Persisted policy checkpoint is inconsistent with the persisted active chain"
            );
            return false;
        }
    }
    if (!has_policy_checkpoint_ && block_storage_) {
        chain::BlockIndex* genesis = chain_active_[0];
        if (genesis && block_storage_->set_policy_checkpoint(genesis->get_block_hash(), 0)) {
            has_policy_checkpoint_ = true;
            policy_checkpoint_height_ = 0;
            policy_checkpoint_hash_ = genesis->get_block_hash();
        } else if (!genesis && has_pruned_anchor &&
                   block_storage_->set_policy_checkpoint(pruned_anchor.hash, pruned_anchor.height)) {
            has_policy_checkpoint_ = true;
            policy_checkpoint_height_ = pruned_anchor.height;
            policy_checkpoint_hash_ = pruned_anchor.hash;
        }
    }

    // Reconcile the saved active chain with its snapshot before considering
    // any pending candidate. Setting a new tip here would force a full replay
    // even when a pruned snapshot can apply the available tail directly.

    if (block_storage_ && restored_from_active_index && !active_chain_index_matches()) {
        GEO_LOG_WARN(
            "blockchain",
            "Persisted active-chain height index is incomplete or stale; rewriting it atomically"
        );
        restored_from_active_index = false;
    }

    // Фиксируем индекс активной цепи на диске.
    // Важно:
    // - Хранилище блоков должно быть fork-aware: на одной высоте могут существовать разные блоки.
    // - Поэтому отдельный индекс active-chain (height -> active_hash) является обязательным.
    if (block_storage_ && !restored_from_active_index) {
        if (!persist_active_chain_index()) {
            GEO_LOG_ERROR("blockchain", "Failed to persist active chain index");
            return false;
        }
    }

    // Синхронизируем ChainState с активной цепью
    if (NodeVerboseEnabled()) {
        GEO_LOG_INFO("blockchain", "Synchronizing ChainState...");
    }

    // Проверяем, совпадает ли высота ChainState с активной цепью
    Height chain_state_height = chain_state_->get_height();
    Height active_chain_height = chain_active_.height();

    // ВАЖНО: Height недостаточно (на одной высоте могут быть разные ветки).
    // Поэтому дополнительно сверяем tip hash ChainState с tip активной цепи.
    const chain::BlockIndex* active_tip = chain_active_.tip();
    Hash256 active_tip_hash = active_tip ? active_tip->get_block_hash() : Hash256{};
    bool tip_mismatch = (chain_state_->get_tip_hash() != active_tip_hash);
    const bool missing_derived_state = !chain_state_->has_derived_state_snapshot();

    // Перестраиваем ChainState если:
    // - высота отличается
    // - UTXO пустой при ненулевой цепи
    // - tip hash не совпадает с активной цепью
    // - snapshot derived-state текущей epoch отсутствует или не прошёл проверку
    bool need_rebuild = (chain_state_height != active_chain_height) ||
                        (chain_state_->get_utxo_count() == 0 && active_chain_height > 0) ||
                        tip_mismatch ||
                        missing_derived_state;

    if (need_rebuild) {
        if (has_pruned_anchor && pruned_anchor.height > 0) {
            GEO_LOG_ERROR(
                "blockchain",
                "Cannot rebuild ChainState from compacted pruned block index: anchor_height="
                    << pruned_anchor.height
                    << ". Start from a validated snapshot or resync from full history."
            );
            return false;
        }
        if (chain_state_height != active_chain_height) {
            if (NodeVerboseEnabled()) {
                GEO_LOG_INFO(
                    "blockchain",
                    "ChainState height mismatch: " << chain_state_height << " vs " << active_chain_height
                );
            }
        }
        if (chain_state_->get_utxo_count() == 0 && active_chain_height > 0) {
            if (NodeVerboseEnabled()) {
                GEO_LOG_INFO("blockchain", "ChainState UTXO set is empty, rebuilding...");
            }
        }
        if (tip_mismatch) {
            if (NodeVerboseEnabled()) {
                GEO_LOG_INFO("blockchain", "ChainState tip hash mismatch, rebuilding...");
            }
        }
        if (missing_derived_state) {
            GEO_LOG_INFO("blockchain", "ChainState snapshot incomplete; checking recovery source...");
        }

        if (NodeVerboseEnabled()) {
            GEO_LOG_INFO("blockchain", "Rebuilding ChainState from blocks...");
        }

        // Verify the entire replay source before touching the saved snapshot.
        // In particular, a missing body is not permission to clear pruned state.
        for (Height h = 0;; ++h) {
            auto* index = chain_active_[h];
            Block candidate;
            if (!index || !index->has_data() || !load_block_from_disk(index, candidate)) {
                GEO_LOG_ERROR("blockchain", "Recovery source unavailable at height " << h
                    << "; saved state and journal retained");
                return false;
            }
            for (size_t ti = 1; ti < candidate.transactions.size(); ++ti) {
                const auto& tx = candidate.transactions[ti];
                const bool required = tx.type == TransactionType::STANDARD ||
                    tx.type == TransactionType::NFT_CREATE || tx.type == TransactionType::NFT_TRANSFER ||
                    tx.type == TransactionType::NFT_BURN || tx.type == TransactionType::CADASTRAL_DOC;
                if (required && tx.witnesses.empty()) {
                    GEO_LOG_ERROR("blockchain", "Recovery witness unavailable at height " << h
                        << "; saved state and journal retained");
                    return false;
                }
            }
            if (h == active_chain_height) break;
        }
        if (!chain_state_->prepare_rebuild()) {
            GEO_LOG_ERROR("blockchain", "Could not checkpoint and prepare state for recovery");
            return false;
        }

        // Применяем все блоки активной цепи
        consensus::ValidationState state;

        for (Height h = 0; h <= active_chain_height; h++) {
            chain::BlockIndex* index = chain_active_[h];
            if (!index) {
                GEO_LOG_ERROR("blockchain", "Missing block at height " << h);
                return false;
            }
            if (!index->has_data()) {
                GEO_LOG_ERROR(
                    "blockchain",
                    "Cannot rebuild ChainState from pruned block body at height " << h
                        << ". Start from a validated snapshot or resync from full history."
                );
                return false;
            }

            Block block;
            if (!load_block_from_disk(index, block)) {
                GEO_LOG_ERROR("blockchain", "Failed to load block at height " << h);
                return false;
            }

            bool has_missing_witness = false;
            for (size_t ti = 1; ti < block.transactions.size(); ++ti) {
                const auto tx_type = block.transactions[ti].type;
                const bool witness_required =
                    (tx_type == TransactionType::STANDARD) ||
                    (tx_type == TransactionType::NFT_CREATE) ||
                    (tx_type == TransactionType::NFT_TRANSFER) ||
                    (tx_type == TransactionType::NFT_BURN) ||
                    (tx_type == TransactionType::CADASTRAL_DOC);
                if (witness_required && block.transactions[ti].witnesses.empty()) {
                    has_missing_witness = true;
                    break;
                }
            }

            if (has_missing_witness) {
                GEO_LOG_ERROR(
                    "blockchain",
                    "Cannot rebuild ChainState from block with missing witness at height " << h
                        << ". Restore the authenticated witness payload, use a validated snapshot, "
                           "or resync from full history."
                );
                return false;
            }

            // Do not bypass canonical body, commitment, transaction or
            // signature checks while deriving state from stored blocks.
            if (!chain_state_->apply_block(block)) {
                GEO_LOG_ERROR(
                    "blockchain",
                    "Failed to verify and apply stored block at height " << h
                );
                return false;
            }

            if (NodeVerboseEnabled()) {
                if ((h + 1) % 1000 == 0) {
                    GEO_LOG_INFO("blockchain", "Applied " << (h + 1) << " blocks...");
                }
            }
        }

        if (!chain_state_->finish_rebuild()) {
            GEO_LOG_ERROR("blockchain", "Could not finalize rebuilt state");
            return false;
        }
        GEO_LOG_INFO("blockchain", "ChainState rebuilt successfully");
    } else {
        if (NodeVerboseEnabled()) {
            GEO_LOG_INFO("blockchain", "ChainState is synchronized (height=" << chain_state_height << ")");
        }
    }

    for (;;) {
        auto* candidate = find_most_work_chain_allowed();
        if (!candidate || candidate == chain_active_.tip() ||
            candidate->chain_work <= chain_active_.tip()->chain_work) break;
        consensus::ValidationState activation;
        if (activate_best_chain(activation)) break;
        if (!activation.is_invalid() || candidate == find_most_work_chain_allowed() ||
            !chain_state_->has_derived_state_snapshot()) {
            GEO_LOG_ERROR("blockchain", "Failed to activate stored candidate: " << activation.get_reject_reason());
            return false;
        }
        // Invalid candidates were removed; the restored snapshot can try the next one.
    }

    GEO_LOG_INFO(
        "blockchain",
        "Blockchain initialized successfully: blocks_in_index=" << block_index_.size()
        << ", active_height=" << chain_active_.height()
        << ", total_supply=" << chain_state_->get_total_supply() << " angles"
    );

    maybe_update_policy_checkpoint();

    return true;
} catch (const storage::ReadError& error) {
    GEO_LOG_ERROR("blockchain", "Storage read failed during initialization: " << error.what());
    return false;
}

bool Blockchain::apply_genesis() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    // Genesis - константа сети (как в Bitcoin): берем из chainparams.
    const Block& genesis = consensus::Params().get_genesis_block();
    const Hash256& expected_genesis_hash = consensus::Params().get_genesis_hash();
    const Hash256 genesis_hash = genesis.get_hash();

    if (genesis_hash != expected_genesis_hash) {
        GEO_LOG_ERROR("blockchain", "Genesis block mismatch (built != expected)!");
        return false;
    }

    GEO_LOG_INFO("blockchain", "Creating Genesis block...");

    consensus::ValidationState state;

    // Принимаем Genesis блок
    chain::BlockIndex* index = nullptr;
    if (!accept_block(genesis, state, &index)) {
        GEO_LOG_ERROR(
            "blockchain",
            "Failed to accept Genesis block: " << state.get_reject_reason()
        );
        return false;
    }

    storage::ActiveChainTransitionRecord transition;
    transition.kind = storage::ActiveChainTransitionKind::CONNECT;
    transition.old_tip_exists = false;
    transition.new_height = 0;
    transition.new_tip_hash = genesis_hash;
    if (block_storage_ && !block_storage_->begin_active_chain_transition(transition)) {
        GEO_LOG_ERROR("blockchain", "Failed to persist Genesis transition intent");
        return false;
    }

    // Для Genesis блока применяем напрямую к ChainState (без валидации комиссий)
    if (!chain_state_->apply_block(genesis)) {
        GEO_LOG_ERROR("blockchain", "Failed to apply Genesis block to ChainState");
        if (block_storage_ && !block_storage_->clear_active_chain_transition()) {
            GEO_LOG_WARN("blockchain", "Failed to clear aborted Genesis transition intent");
        }
        return false;
    }

    // Обновляем активную цепь
    chain_active_.set_tip(index);

    if (block_storage_) {
        if (!block_storage_->set_active_block_and_tip(0, genesis_hash)) {
            GEO_LOG_ERROR("blockchain", "Failed to persist Genesis active chain tip");
            // Keep the transition journal: startup can roll the active index
            // forward to the already committed ChainState.
            return false;
        }
        if (!block_storage_->clear_active_chain_transition()) {
            GEO_LOG_WARN("blockchain", "Failed to clear committed Genesis transition intent");
        }
        block_storage_->set_policy_checkpoint(genesis_hash, 0);
    }
    has_policy_checkpoint_ = true;
    policy_checkpoint_height_ = 0;
    policy_checkpoint_hash_ = genesis_hash;

    // Помечаем Genesis как валидный
    index->status |= chain::BlockStatus::VALID_SCRIPTS;

    GEO_LOG_INFO("blockchain", "Genesis block applied successfully");
    return true;
}

} // namespace geocoin::core
