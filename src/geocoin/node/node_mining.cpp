#include "geocoin/consensus/chainparams.hpp"
#include "geocoin/node/node.hpp"

#include "geocoin/consensus/consensus.hpp"
#include "geocoin/consensus/geo_tag.hpp"
#include "geocoin/consensus/optimized_miner.hpp"
#include "geocoin/core/merkle.hpp"
#include "geocoin/node/node_runtime.hpp"
#include "geocoin/node/node_status_utils.hpp"
#include "geocoin/utils/logger.hpp"
#include "geocoin/utils/logging.hpp"
#include "geocoin/utils/metrics.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <iomanip>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

using geocoin::utils::LogPrintf;

namespace geocoin::node {
namespace {

void configure_mining_thread_scheduling(int thread_id) {
#ifdef _WIN32
    // Windows splits large CPUs into processor groups. Distribute mining
    // workers across groups so machines with 128/192+ logical CPUs are used.
    const WORD group_count = GetActiveProcessorGroupCount();
    if (group_count > 1) {
        std::vector<WORD> groups;
        groups.reserve(group_count);
        for (WORD group = 0; group < group_count; ++group) {
            if (GetActiveProcessorCount(group) > 0) {
                groups.push_back(group);
            }
        }

        if (!groups.empty()) {
            const size_t group_index = static_cast<size_t>(thread_id) % groups.size();
            const WORD group = groups[group_index];
            const DWORD processors = GetActiveProcessorCount(group);
            if (processors > 0) {
                const DWORD cpu_index = static_cast<DWORD>(
                    (static_cast<size_t>(thread_id) / groups.size()) % processors
                );
                if (cpu_index < sizeof(KAFFINITY) * 8) {
                    GROUP_AFFINITY affinity{};
                    affinity.Group = group;
                    affinity.Mask = static_cast<KAFFINITY>(1) << cpu_index;
                    (void)SetThreadGroupAffinity(GetCurrentThread(), &affinity, nullptr);
                }
            }
        }
    }

    // Below-normal keeps the GUI responsive without the severe throttling of
    // THREAD_MODE_BACKGROUND_BEGIN, which hurts miner hashrate on Windows.
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#else
    (void)thread_id;
#endif
}

}  // namespace

bool Node::start_mining() {
    std::lock_guard<std::mutex> lock(mining_control_mutex_);
    return start_mining_unlocked();
}

bool Node::start_mining_unlocked() {
    if (mining_ || !running_) {
        GEO_METRIC_INC("node.mining_start_fail_total", 1);
        return false;
    }

    if (config_.mining_address.empty()) {
        LogPrintf("Mining address not set\n");
        GEO_METRIC_INC("node.mining_start_fail_total", 1);
        return false;
    }

    if (config_.mining_threads == 0) {
        LogPrintf("Mining thread count must be greater than zero\n");
        GEO_METRIC_INC("node.mining_start_fail_total", 1);
        return false;
    }
    const size_t safe_thread_limit = cpu_mining_thread_limit();
    if (config_.mining_threads > safe_thread_limit) {
        LogPrintf(
            "Mining thread count %zu exceeds safe limit %zu; clamping\n",
            config_.mining_threads,
            safe_thread_limit
        );
        config_.mining_threads = safe_thread_limit;
    }

    const Height local_height = get_active_height();
    Height sync_target_height = local_height;
    bool sync_discovery_active = false;
    if (sync_manager_) {
        const auto sync_stats = sync_manager_->get_stats();
        sync_target_height = std::max(sync_target_height, sync_stats.target_height);
        sync_discovery_active = sync_stats.status == network::SyncStatus::SYNCING;
    }

    if (sync_discovery_active || detail::is_height_behind(local_height, sync_target_height)) {
        LogPrintf(
            "Mining blocked: local height %llu is behind sync target height %llu\n",
            static_cast<unsigned long long>(local_height),
            static_cast<unsigned long long>(sync_target_height)
        );
        GEO_LOG_WARN(
            "node",
            "Mining blocked until sync completes: local_height=" << local_height
                << ", sync_target_height=" << sync_target_height
        );
        GEO_METRIC_INC("node.mining_start_blocked_by_sync_total", 1);
        GEO_METRIC_INC("node.mining_start_fail_total", 1);
        return false;
    }

    if (!consensus::set_mining_sha256_backend(config_.mining_sha256_backend)) {
        LogPrintf("Unknown mining SHA256 backend '%s', falling back to auto\n", config_.mining_sha256_backend.c_str());
        config_.mining_sha256_backend = "auto";
        consensus::set_mining_sha256_backend(config_.mining_sha256_backend);
    }

    LogPrintf("Starting mining with %zu threads (sha256=%s)...\n",
        config_.mining_threads,
        consensus::get_mining_sha256_backend().c_str());

    // Workers from an earlier session that ended on their own (a worker
    // exception or a background runtime failure clears mining_ without
    // joining) stay joinable until stop_mining() runs. mining_ is false here,
    // so they are exiting; reap them before starting new ones so the vector
    // and the reported thread count only describe the new session.
    for (auto& thread : mining_threads_) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    mining_threads_.clear();

    mining_ = true;
    GEO_METRIC_SET("node.mining", 1);
    GEO_METRIC_INC("node.mining_start_total", 1);
    {
        std::lock_guard<std::mutex> lock(mining_stats_mutex_);
        total_hashes_.store(0, std::memory_order_relaxed);
        last_reported_hashes_ = 0;
        current_hashrate_ = 0.0;
        mining_start_time_ = std::chrono::steady_clock::now();
        mining_last_hashrate_time_ = mining_start_time_;
    }

    // Thread construction can fail after a subset has started. Roll the
    // subset back synchronously so no joinable worker escapes this method.
    try {
        mining_threads_.reserve(config_.mining_threads);
        for (size_t i = 0; i < config_.mining_threads; i++) {
            mining_threads_.emplace_back([this, i]() {
                try {
                    mining_thread(static_cast<int>(i));
                } catch (const std::exception& error) {
                    GEO_LOG_ERROR("mining", "Mining worker terminated: " << error.what());
                    mining_.store(false, std::memory_order_release);
                } catch (...) {
                    GEO_LOG_ERROR("mining", "Mining worker terminated with a non-standard exception");
                    mining_.store(false, std::memory_order_release);
                }
            });
        }
    } catch (const std::exception& error) {
        GEO_LOG_ERROR("mining", "Failed to start mining workers: " << error.what());
        mining_.store(false, std::memory_order_release);
        for (auto& thread : mining_threads_) {
            if (thread.joinable()) {
                thread.join();
            }
        }
        mining_threads_.clear();
        GEO_METRIC_SET("node.mining", 0);
        GEO_METRIC_INC("node.mining_start_fail_total", 1);
        return false;
    } catch (...) {
        GEO_LOG_ERROR("mining", "Failed to start mining workers");
        mining_.store(false, std::memory_order_release);
        for (auto& thread : mining_threads_) {
            if (thread.joinable()) {
                thread.join();
            }
        }
        mining_threads_.clear();
        GEO_METRIC_SET("node.mining", 0);
        GEO_METRIC_INC("node.mining_start_fail_total", 1);
        return false;
    }

    return true;
}

bool Node::start_mining(size_t threads, const Address& mining_address) {
    std::lock_guard<std::mutex> lock(mining_control_mutex_);

    if (mining_ || !running_) {
        GEO_METRIC_INC("node.mining_start_fail_total", 1);
        return false;
    }

    // Обновляем конфиг
    config_.mining_threads = clamp_cpu_mining_threads(threads);
    config_.mining_address = mining_address;
    config_.enable_mining = true;

    // Запускаем майнинг
    return start_mining_unlocked();
}

void Node::stop_mining() {
    std::lock_guard<std::mutex> lock(mining_control_mutex_);
    if (!mining_.load(std::memory_order_acquire) && mining_threads_.empty()) {
        return;
    }

    LogPrintf("Stopping mining...\n");

    mining_.store(false, std::memory_order_release);
    GEO_METRIC_SET("node.mining", 0);
    GEO_METRIC_INC("node.mining_stop_total", 1);

    // Ждем завершения потоков
    for (auto& thread : mining_threads_) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    mining_threads_.clear();

    LogPrintf("Mining stopped (total hashes: %llu)\n",
              static_cast<unsigned long long>(total_hashes_.load(std::memory_order_relaxed)));
    {
        std::lock_guard<std::mutex> lock(mining_stats_mutex_);
        current_hashrate_ = 0.0;
        last_reported_hashes_ = 0;
    }
}

void Node::mining_thread(int thread_id) {
    configure_mining_thread_scheduling(thread_id);

    {
        std::lock_guard<std::mutex> lock(mining_output_mutex_);
        if (detail::sync_debug_enabled()) {
            LogPrintf("⛏️  Mining thread %d started\n", thread_id);
        }
    }

    // Используем оптимизированный майнер
    consensus::OptimizedMiner miner;
    const uint64_t nonce_step = std::max<uint64_t>(1, config_.mining_threads);
    uint64_t next_nonce = static_cast<uint64_t>(thread_id);

    while (mining_) {
        // Сохраняем счетчик обновлений mempool (как в Bitcoin)
        uint64_t nTransactionsUpdatedLast = mempool_->get_transactions_updated();

        // Создаем блок для майнинга
        core::Block block = create_mining_block();

        // Bind PoW to the exact parent captured by the template. Reading tip
        // before building allowed the builder and miner to use different
        // parents when a block arrived in between.
        const core::ActiveTipSnapshot parent_snapshot = blockchain_->get_active_tip_snapshot();
        if (!parent_snapshot.index || parent_snapshot.hash != block.header.prev_block_hash) {
            continue;
        }

        // Split nonce space into stable per-thread lanes:
        // thread 0 checks 0,N,2N..., thread 1 checks 1,N+1,2N+1...
        // This avoids duplicated work and keeps batches contiguous across yields.
        block.header.nonce = next_nonce;

        // Время начала майнинга
        uint64_t nStart = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();

        // Майним блок небольшими батчами. Это не меняет PoW/консенсус, но
        // не дает full-CPU майнингу надолго вытеснять GUI/network потоки.
        uint64_t max_iterations = config_.mining_cooperative_batch_iterations;
        if (max_iterations == 0) {
            max_iterations = 65536;
        }

        const uint32_t prev_timestamp = static_cast<uint32_t>(parent_snapshot.median_time_past);
        const Hash256 prev_hash = parent_snapshot.hash;
        bool found = miner.mine_block_optimized(
            block,
            block.header.bits,
            prev_timestamp,
            prev_hash,
            parent_snapshot.height + 1,
            max_iterations,
            &mining_,
            thread_id,
            &total_hashes_,
            nonce_step
        );
        next_nonce = block.header.nonce;

        if (!found && mining_) {
            if (config_.mining_cooperative_pause_us > 0) {
                std::this_thread::sleep_for(std::chrono::microseconds(config_.mining_cooperative_pause_us));
            } else {
                std::this_thread::yield();
            }
        }

        if (!blockchain_->is_active_tip(parent_snapshot.hash)) {
            // Tip изменился, другой поток нашел блок - начинаем майнить новый блок
            continue;
        }

        // Проверяем обновления mempool (как в Bitcoin)
        uint64_t nTime = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();
        if (mempool_->get_transactions_updated() != nTransactionsUpdatedLast && nTime - nStart > 60) {
            // Mempool обновился и прошло больше 60 секунд - пересоздаем блок
            continue;
        }

        if (found) {
            // Финальная проверка перед отправкой (как в Bitcoin)
            if (!blockchain_->is_active_tip(parent_snapshot.hash)) {
                // Блок устарел
                std::lock_guard<std::mutex> output_lock(mining_output_mutex_);
                if (detail::sync_debug_enabled()) {
                    LogPrintf("⚠️  Thread %d: Block is stale (tip changed)\n", thread_id);
                }
                continue;
            }

            // Синхронизируем вывод
            {
                std::lock_guard<std::mutex> output_lock(mining_output_mutex_);

                const uint32_t prev_timestamp = static_cast<uint32_t>(parent_snapshot.median_time_past);
                const Hash256 prev_hash = parent_snapshot.hash;
                auto block_hash = consensus::ProofOfWork::compute_pow_hash(block.header, prev_timestamp, prev_hash, parent_snapshot.height + 1);
                auto target = consensus::ProofOfWork::bits_to_target(block.header.bits);

                LogPrintf("\n╔════════════════════════════════════════════════════════════╗\n");
                LogPrintf("║  🎉 BLOCK FOUND BY THREAD %2d! 🎉                        ║\n", thread_id);
                LogPrintf("╚════════════════════════════════════════════════════════════╝\n");
                LogPrintf("Height:       %llu\n", static_cast<unsigned long long>(parent_snapshot.height + 1));
                LogPrintf("Nonce:        %llu\n", static_cast<unsigned long long>(block.header.nonce));
                LogPrintf("Difficulty:   0x%x\n", block.header.bits);
                LogPrintf("Transactions: %zu\n", block.transactions.size());
                const double elapsed_s = std::chrono::duration_cast<std::chrono::duration<double>>(
                    std::chrono::steady_clock::now() - mining_start_time_
                ).count();
                const uint64_t total_hashes = total_hashes_.load(std::memory_order_relaxed);
                const double node_rate_hs = (elapsed_s > 0.0)
                    ? (static_cast<double>(total_hashes) / elapsed_s)
                    : 0.0;
                LogPrintf("Node Rate:    %.2f MH/s\n", (node_rate_hs / 1000000.0));
                LogPrintf("Block Hash:   ");
                for (size_t i = 0; i < 32; i++) {
                    LogPrintf("%02x", block_hash[i]);
                }
                LogPrintf("\nTarget:       ");
                for (int i = 3; i >= 0; i--) {
                    LogPrintf("%016llx", static_cast<unsigned long long>(target.data[i]));
                }
                LogPrintf("\n");
            }

            // RPC, P2P and all mining threads publish through the same ordered
            // mutation port.  The coordinator performs consensus validation,
            // active-tip publication and mempool cleanup as one operation.
            const auto submission = process_block(block);
            if (submission.is_accepted() &&
                submission.active_tip_changed &&
                submission.block_is_active) {
                mined_blocks_accepted_.fetch_add(1, std::memory_order_relaxed);
                LogPrintf("✅ Block applied to chain! New height: %llu\n",
                          static_cast<unsigned long long>(submission.active_height));
                GEO_METRIC_INC("node.mined_blocks_applied_total", 1);

                LogPrintf("⛏️  Starting to mine block %llu...\n",
                          static_cast<unsigned long long>(submission.active_height + 1));

                // Продолжаем майнинг следующего блока (как в Bitcoin: break из внутреннего цикла)
                continue;
            } else {
                const std::string reason = submission.reject_reason.empty()
                    ? "mined block did not become the active tip"
                    : submission.reject_reason;
                LogPrintf("❌ Failed to apply block: %s\n", reason.c_str());
                GEO_METRIC_INC("node.mined_blocks_rejected_total", 1);
                // Продолжаем майнинг того же блока
                continue;
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(mining_output_mutex_);
        if (detail::sync_debug_enabled()) {
            LogPrintf("Mining thread %d stopped\n", thread_id);
        }
    }
}

core::Block Node::create_mining_block() {
    core::Block last_template;
    for (unsigned int attempt = 0; attempt < 3; ++attempt) {
        const auto tip_snapshot = blockchain_->get_active_tip_snapshot();
        last_template = create_mining_block_for_tip(tip_snapshot);
        if (blockchain_->is_active_tip(tip_snapshot.hash)) {
            return last_template;
        }
    }

    // The coordinator and mining loop still reject residual stale work.
    return last_template;
}

core::Block Node::create_mining_block_for_tip(const core::ActiveTipSnapshot& tip_snapshot) {
    core::Block block;

    const Height next_height = tip_snapshot.index ? tip_snapshot.height + 1 : 0;

    // Заголовок
    block.header.version = 1;
    uint64_t block_timestamp = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count());

    // Привязываем блок к текущему tip
    if (tip_snapshot.index) {
        block.header.prev_block_hash = tip_snapshot.hash;
        const uint64_t min_timestamp = tip_snapshot.median_time_past + 1;
        if (block_timestamp < min_timestamp) {
            block_timestamp = min_timestamp;
        }
    }
    block.header.timestamp = static_cast<uint32_t>(block_timestamp);

    // Вычисляем динамическую сложность
    block.header.bits = consensus::calculate_next_difficulty(tip_snapshot.index, block.header.timestamp);
    block.header.nonce = 0;

    // GeoPoW (новая схема): геометка вычисляется детерминированно из prev_timestamp + prev_hash.
    // Geo_tag сохраняется в заголовке блока.
    uint32_t prev_timestamp = 0;
    Hash256 prev_hash = {};
    if (tip_snapshot.index) {
        prev_timestamp = static_cast<uint32_t>(tip_snapshot.median_time_past);
        prev_hash = tip_snapshot.hash;
    }

    const auto tag = consensus::geo::compute_block_geo_tag(prev_timestamp, prev_hash, next_height,
        consensus::Params().geopow_activation_height());
    block.header.geo_tag = tag.geo_tag;

    if (detail::sync_debug_enabled()) {
        GEO_LOG_DEBUG(
            "node",
            "create_block_template: geo_tag=0x" << std::hex << tag.geo_tag << std::dec
        );
        GEO_LOG_DEBUG(
            "node",
            "create_block_template: lat_ndeg=" << tag.lat_ndeg << " lon_ndeg=" << tag.lon_ndeg
        );
    }

    // INTEREST_DISTRIBUTION is consensus-mandatory at index 1 when it can be
    // built. Reserve its size before selecting user transactions from mempool.
    core::Transaction interest_distribution;
    const bool has_interest_distribution =
        chain_state_->create_interest_distribution_transaction(next_height, interest_distribution);
    size_t mempool_limit = consensus::params::BLOCK_TEMPLATE_SOFT_LIMIT;
    if (has_interest_distribution) {
        const size_t interest_size = interest_distribution.serialize().size();
        mempool_limit = interest_size < mempool_limit ? mempool_limit - interest_size : 0;
    }

    auto mempool_txs = mempool_->get_transactions_for_mining(mempool_limit, block.header.timestamp);

    if (detail::sync_debug_enabled()) {
        GEO_LOG_DEBUG(
            "node",
            "create_block_template: Got " << mempool_txs.size() << " transactions from mempool"
        );
    }

    const Amount block_reward = consensus::ProofOfWork::calculate_block_reward(next_height);

    // Считаем комиссии блока. Сохраняем только безопасный префикс пакета:
    // это не разрывает порядок зависимостей, который вернул mempool.
    Amount block_fees = 0;
    size_t accepted_count = 0;
    for (const Amount fee : chain_state_->project_block_transaction_fees(mempool_txs, 0, next_height)) {
        Amount next_fees = 0;
        Amount coinbase_total = 0;
        if (fee < 0 ||
            !checked_add_amount(block_fees, fee, next_fees) ||
            !checked_add_amount(block_reward, next_fees, coinbase_total)) {
            GEO_LOG_WARN("node", "Block template fee total overflow; truncating mempool package");
            break;
        }
        block_fees = next_fees;
        ++accepted_count;
    }
    mempool_txs.resize(accepted_count);

    if (detail::sync_debug_enabled()) {
        GEO_LOG_DEBUG("node", "create_block_template: Total fees: " << block_fees);
    }

    // Создаем coinbase транзакцию с учетом комиссий
    auto coinbase = core::Transaction::create_coinbase(
        next_height,
        config_.mining_address,
        block_reward,
        block_fees,
        chain_state_->get_foundation_address()
    );

    block.transactions.push_back(coinbase);

    if (has_interest_distribution) {
        block.transactions.push_back(std::move(interest_distribution));
    }

    // Добавляем транзакции из mempool
    for (const auto& tx : mempool_txs) {
        block.transactions.push_back(tx);
    }

    if (detail::sync_debug_enabled()) {
        GEO_LOG_DEBUG(
            "node",
            "create_block_template: Block has " << block.transactions.size()
                << " transactions (including coinbase)"
        );
    }

    // Вычисляем merkle root
    std::vector<Hash256> tx_hashes;
    for (const auto& tx : block.transactions) {
        tx_hashes.push_back(tx.get_hash());
    }
    block.header.merkle_root = core::compute_merkle_root(tx_hashes);

    if (detail::sync_debug_enabled()) {
        GEO_LOG_DEBUG("node", "create_block_template: Merkle root computed");
    }

    // Witness commitment должен быть заполнен ДО майнинга, т.к. входит в PoW-хеш заголовка.
    block.header.witness_commitment = block.calculate_witness_commitment();

    return block;
}

uint32_t Node::calculate_next_difficulty(Timestamp block_timestamp) {
    auto* tip = blockchain_->get_tip();
    uint32_t new_bits = consensus::calculate_next_difficulty(tip, block_timestamp);

    if (detail::sync_debug_enabled() && tip) {
        GEO_LOG_DEBUG("node", "Difficulty adjustment at height " << (tip->height + 1));
        GEO_LOG_DEBUG("node", "  Old bits: 0x" << std::hex << tip->get_bits() << std::dec);
        GEO_LOG_DEBUG("node", "  New bits: 0x" << std::hex << new_bits << std::dec);
    }

    return new_bits;
}

}  // namespace geocoin::node
