#include "geocoin/consensus/consensus.hpp"
#include "geocoin/consensus/consensus_internal.hpp"
#include "geocoin/consensus/chainparams.hpp"
#include "geocoin/consensus/proof_of_work.hpp"
#include "geocoin/core/merkle.hpp"
#include "geocoin/utils/logger.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <exception>
#include <limits>
#include <set>
#include <thread>
#include <vector>

namespace geocoin::consensus {

bool transaction_requires_basic_signature(const core::Transaction& tx) {
    return tx.type != core::TransactionType::MULTISIG_SPEND &&
           tx.type != core::TransactionType::DELEGATION_PROOF &&
           tx.type != core::TransactionType::REVOKE_DELEGATION &&
           tx.type != core::TransactionType::STANDARD;
}

void verify_basic_signatures(
    const std::vector<core::Transaction>& transactions,
    std::vector<uint8_t>& verified
) {
    verified.assign(transactions.size(), 1);
    std::vector<size_t> pending;
    pending.reserve(transactions.size());
    for (size_t i = 0; i < transactions.size(); ++i) {
        if (transaction_requires_basic_signature(transactions[i])) {
            pending.push_back(i);
        }
    }
    if (pending.size() <= 1) {
        for (const size_t index : pending) {
            verified[index] = transactions[index].verify() ? 1 : 0;
        }
        return;
    }

    const unsigned workers = std::max(
        1u,
        std::min(
            static_cast<unsigned>(pending.size()),
            std::thread::hardware_concurrency()
        )
    );
    std::atomic<size_t> next{0};
    std::vector<std::thread> pool;
    pool.reserve(workers);
    for (unsigned worker = 0; worker < workers; ++worker) {
        pool.emplace_back([&]() {
            while (true) {
                const size_t job = next.fetch_add(1, std::memory_order_relaxed);
                if (job >= pending.size()) {
                    return;
                }
                const size_t index = pending[job];
                verified[index] = transactions[index].verify() ? 1 : 0;
            }
        });
    }
    for (auto& thread : pool) {
        thread.join();
    }
}

bool check_block(
    const core::Block& block,
    ValidationState& state,
    ValidationMode mode
) {
    // Проверяем заголовок
    if (!check_block_header(block.header, state)) {
        return false;
    }

    // Проверяем, что есть транзакции
    if (block.transactions.empty()) {
        return state.invalid(RejectCode::INVALID, "bad-blk-length", "no transactions");
    }
    if (block.transactions.size() > params::MAX_BLOCK_TRANSACTIONS) {
        return state.invalid(
            RejectCode::INVALID,
            "bad-blk-tx-count",
            "too many transactions in block"
        );
    }

    // Проверяем размер блока. Typed transaction с отсутствующим обязательным
    // payload может бросить при serialize(); consensus validation обязана
    // превратить недоверенный ввод в reject, а не выпускать исключение в P2P.
    size_t block_size = 0;
    try {
        block_size = block.serialize().size();
    } catch (const std::exception& e) {
        return state.invalid(
            RejectCode::MALFORMED,
            "bad-blk-serialization",
            e.what()
        );
    } catch (...) {
        return state.invalid(
            RejectCode::MALFORMED,
            "bad-blk-serialization",
            "block serialization failed"
        );
    }
    if (block_size > params::MAX_BLOCK_SIZE) {
        return state.invalid(RejectCode::INVALID, "bad-blk-length", "size limits failed");
    }

    // Проверяем, что первая транзакция - coinbase
    if (!block.transactions[0].is_coinbase()) {
        return state.invalid(RejectCode::INVALID, "bad-cb-missing", "first tx is not coinbase");
    }

    // Проверяем, что остальные транзакции - не coinbase
    for (size_t i = 1; i < block.transactions.size(); i++) {
        if (block.transactions[i].is_coinbase()) {
            return state.invalid(RejectCode::INVALID, "bad-cb-multiple", "more than one coinbase");
        }
    }

    // Duplicate txids make two different block bodies share a merkle root
    // (and therefore a block hash) when the tree duplicates the last leaf.
    std::set<Hash256> seen_txids;
    for (const auto& tx : block.transactions) {
        if (!seen_txids.insert(tx.get_hash()).second) {
            return state.invalid(
                RejectCode::INVALID,
                "bad-txns-duplicate",
                "duplicate txid in block"
            );
        }
    }

    std::vector<uint8_t> signatures_ok;
    if (mode == ValidationMode::FULL) {
        verify_basic_signatures(block.transactions, signatures_ok);
    }

    // Проверяем все транзакции. Синтаксис и первая ошибка остаются в порядке блока.
    for (size_t i = 0; i < block.transactions.size(); ++i) {
        const auto& tx = block.transactions[i];
        if (!check_transaction(tx, state)) {
            return false;
        }
        if (mode == ValidationMode::FULL &&
            transaction_requires_basic_signature(tx) &&
            signatures_ok[i] == 0) {
            return state.invalid(RejectCode::INVALID, "bad-txns-signature", "transaction signature failed");
        }
    }

    if (!check_block_angle_package(block, state)) {
        return false;
    }

    // Проверяем merkle root
    std::vector<Hash256> tx_hashes;
    tx_hashes.reserve(block.transactions.size());
    for (const auto& tx : block.transactions) {
        tx_hashes.push_back(tx.get_hash());
    }

    Hash256 computed_merkle_root = core::compute_merkle_root(tx_hashes);
    if (block.header.merkle_root != computed_merkle_root) {
        return state.invalid(RejectCode::INVALID, "bad-txnmrklroot", "merkle root mismatch");
    }

    const Hash256 computed_witness_commitment = block.calculate_witness_commitment();
    if (block.header.witness_commitment != computed_witness_commitment) {
        return state.invalid(
            RejectCode::INVALID,
            "bad-witness-commitment",
            "witness commitment mismatch"
        );
    }

    return true;
}

bool check_block_transaction_hardening(
    const core::Block& block,
    Height height,
    ValidationState& state
) {
    // Soft fork: blocks below the activation height keep the original rules,
    // so the established chain stays valid on every node.
    if (!is_transaction_hardening_active(height)) {
        return true;
    }
    for (const auto& tx : block.transactions) {
        if (!has_canonical_signature_encodings(tx)) {
            return state.invalid(RejectCode::INVALID, "bad-txns-noncanonical-signature",
                "transaction carries a non-minimal Falcon signature encoding");
        }
        if (!is_revoke_delegation_change_standard(tx)) {
            return state.invalid(RejectCode::INVALID, "bad-revoke-delegation",
                "REVOKE_DELEGATION change is dust or carries a script");
        }
    }
    return true;
}

bool contextual_check_block(
    const core::Block& block,
    ValidationState& state,
    const chain::BlockIndex* prev_index
) {
    // Проверяем заголовок
    if (!contextual_check_block_header(block.header, state, prev_index)) {
        return false;
    }

    if (block.transactions.empty()) {
        return state.invalid(RejectCode::INVALID, "bad-cb-missing", "first tx is not coinbase");
    }

    // Проверяем coinbase транзакцию
    const auto& coinbase = block.transactions[0];

    const Height height = prev_index ? (prev_index->height + 1) : 0;
    if (!check_coinbase_height(coinbase, height, state)) {
        return false;
    }

    for (const auto& tx : block.transactions) {
        if (!is_final_transaction(tx, height, block.header.timestamp)) {
            return state.invalid(RejectCode::INVALID, "bad-txns-nonfinal", "transaction lock_time has not elapsed");
        }
    }

    if (!check_block_transaction_hardening(block, height, state)) {
        return false;
    }

    // ПРИМЕЧАНИЕ: Проверка награды coinbase происходит в ConnectBlock(),
    // где есть доступ к UTXO set для вычисления комиссий.
    // Здесь мы только проверяем базовую структуру.

    return true;
}

bool check_block_header(
    const core::BlockHeader& header,
    ValidationState& state
) {
    // Проверка PoW выполняется в contextual_check_block_header(),
    // потому что в новой схеме PoW зависит от данных предыдущего блока.
    // Проверяем timestamp (не в будущем)
    if (!check_timestamp(header.timestamp, state)) {
        return false;
    }

    return true;
}

bool contextual_check_block_header(
    const core::BlockHeader& header,
    ValidationState& state,
    const chain::BlockIndex* prev_index
) {
    if (prev_index && prev_index->height == (std::numeric_limits<Height>::max)()) {
        return state.invalid(
            RejectCode::INVALID,
            "bad-height-overflow",
            "block height overflow"
        );
    }
    const Height height = prev_index ? (prev_index->height + 1) : 0;
    if (!check_hardened_checkpoint(height, header.get_hash(), state)) {
        return false;
    }

    if (prev_index) {
        // Проверяем prev_block_hash
        if (header.prev_block_hash != prev_index->get_block_hash()) {
            return state.invalid(RejectCode::INVALID, "bad-prevblk", "prev block hash mismatch");
        }

        // Проверяем timestamp >= median time past
        if (!check_timestamp_against_prev(header.timestamp, prev_index, state)) {
            return false;
        }

        // Проверяем сложность
        if (!check_difficulty(header.bits, prev_index, header.timestamp, state)) {
            return false;
        }

        // Проверяем PoW (если не пропускаем)
        if (!ProofOfWork::verify_proof_of_work(
                header,
                header.bits,
                static_cast<uint32_t>(prev_index->get_median_time_past()),
                prev_index->get_block_hash(), prev_index->height + 1)) {
            return state.invalid(RejectCode::INVALID, "bad-pow", "invalid proof of work");
        }
    } else {
        // Genesis блок
        // Для Genesis prev_index нет, поэтому PoW проверяем с prev_timestamp=0 и prev_hash=0.
        if (!ProofOfWork::verify_proof_of_work(header, header.bits, 0U, Hash256{}, 0)) {
            return state.invalid(RejectCode::INVALID, "bad-pow", "invalid genesis proof of work");
        }
    }

    return true;
}

bool check_hardened_checkpoint(
    Height height,
    const Hash256& block_hash,
    ValidationState& state
) {
    const auto& checkpoints = Params().checkpoints();
    const auto it = checkpoints.find(height);
    if (it == checkpoints.end() || it->second == block_hash) {
        return true;
    }

    return state.invalid(
        RejectCode::CHECKPOINT,
        "checkpoint-mismatch",
        "block hash does not match hardened network checkpoint",
        false
    );
}

bool check_timestamp(
    Timestamp timestamp,
    ValidationState& state
) {
    // Проверяем, что timestamp не слишком далеко в будущем
    auto now = std::chrono::system_clock::now();
    auto now_timestamp = std::chrono::duration_cast<std::chrono::seconds>(
        now.time_since_epoch()
    ).count();

    if (timestamp > now_timestamp + params::MAX_FUTURE_BLOCK_TIME) {
        return state.invalid(RejectCode::INVALID, "time-too-new", "block timestamp too far in future");
    }

    return true;
}

bool check_timestamp_against_prev(
    Timestamp timestamp,
    const chain::BlockIndex* prev_index,
    ValidationState& state
) {
    if (!prev_index) {
        return true;
    }

    // Timestamp должен быть >= median time past
    Timestamp median_time_past = get_median_time_past(prev_index);

    if (timestamp <= median_time_past) {
        return state.invalid(RejectCode::INVALID, "time-too-old", "block timestamp too old");
    }

    return true;
}

Timestamp get_median_time_past(const chain::BlockIndex* index) {
    if (!index) {
        return 0;
    }

    return index->get_median_time_past();
}

bool check_coinbase_height(
    const core::Transaction& coinbase,
    Height height,
    ValidationState& state
) {
    if (!coinbase.is_coinbase()) {
        return state.invalid(RejectCode::INVALID, "bad-cb-height", "transaction is not coinbase");
    }

    // Текущий genesis содержит фиксированное текстовое сообщение и отдельно
    // закреплен hardcoded hash, поэтому BIP34-like encoding начинается с h=1.
    if (height == 0) {
        return true;
    }

    const bytes& script_sig = coinbase.inputs[0].script_sig;
    if (script_sig.size() < sizeof(Height)) {
        return state.invalid(
            RejectCode::INVALID,
            "bad-cb-height",
            "coinbase script_sig does not contain block height"
        );
    }

    Height committed_height = 0;
    for (size_t i = 0; i < sizeof(Height); ++i) {
        committed_height |= static_cast<Height>(script_sig[i]) << (i * 8);
    }
    if (committed_height != height) {
        return state.invalid(
            RejectCode::INVALID,
            "bad-cb-height",
            "coinbase height commitment mismatch"
        );
    }

    return true;
}

bool check_coinbase_outputs(
    const core::Transaction& coinbase,
    Height height,
    Amount expected_reward,
    Amount fees,
    const Address& foundation_address,
    ValidationState& state
) {
    if (!coinbase.is_coinbase()) {
        return state.invalid(RejectCode::INVALID, "bad-cb-check", "not a coinbase transaction");
    }
    if (!check_transaction(coinbase, state) || !check_coinbase_height(coinbase, height, state)) {
        return false;
    }

    Amount total_reward = 0;
    if (expected_reward < 0 || fees < 0 || !checked_add_amount(expected_reward, fees, total_reward)) {
        return state.invalid(RejectCode::INVALID, "bad-cb-amount", "invalid or overflowing coinbase reward");
    }

    // === ОСОБЫЙ СЛУЧАЙ: Genesis блок ===
    // Genesis блок имеет 1 output со 100% награды на адрес фонда
    if (height == 0) {
        // Genesis блок: должен быть 1 output на адрес фонда
        if (coinbase.outputs.size() != 1) {
            return state.invalid(RejectCode::INVALID, "bad-cb-genesis-outputs",
                "genesis coinbase must have 1 output (foundation)");
        }

        // Проверяем адрес фонда
        if (coinbase.outputs[0].address != foundation_address) {
            return state.invalid(RejectCode::INVALID, "bad-cb-genesis-addr",
                "genesis coinbase must pay to foundation address");
        }

        // Проверяем сумму
        if (coinbase.outputs[0].value != total_reward) {
            return state.invalid(RejectCode::INVALID, "bad-cb-genesis-amount",
                "genesis coinbase amount mismatch");
        }

        return true;
    }

    // === ОБЫЧНЫЕ БЛОКИ (height > 0) ===
    const Amount foundation_cut = calculate_foundation_reward_cut(total_reward);
    const Amount expected_miner = total_reward - foundation_cut;

    if (is_foundation_reward_redirected_to_pool(height)) {
        // После активации interest pool coinbase содержит только выплату майнеру.
        if (coinbase.outputs.size() != 1) {
            return state.invalid(RejectCode::INVALID, "bad-cb-outputs",
                "coinbase must have 1 output (miner only) after pool activation");
        }

        if (coinbase.outputs[0].value != expected_miner) {
            return state.invalid(RejectCode::INVALID, "bad-cb-miner-amount",
                "invalid miner reward amount after pool activation");
        }
    } else if (is_foundation_reward_active(height)) {
        // До активации interest pool действует классическое правило miner + foundation.
        if (coinbase.outputs.size() != 2) {
            return state.invalid(RejectCode::INVALID, "bad-cb-outputs",
                "coinbase must have 2 outputs (miner + foundation)");
        }

        if (coinbase.outputs[1].address != foundation_address) {
            return state.invalid(RejectCode::INVALID, "bad-cb-foundation-addr",
                "invalid foundation address");
        }

        if (coinbase.outputs[1].value != foundation_cut) {
            return state.invalid(RejectCode::INVALID, "bad-cb-foundation-amount",
                "invalid foundation reward amount");
        }

        if (coinbase.outputs[0].value != expected_miner) {
            return state.invalid(RejectCode::INVALID, "bad-cb-miner-amount",
                "invalid miner reward amount");
        }

        Amount total_outputs = 0;
        if (!checked_add_amount(
                coinbase.outputs[0].value,
                coinbase.outputs[1].value,
                total_outputs) ||
            total_outputs != total_reward) {
            return state.invalid(RejectCode::INVALID, "bad-cb-amount",
                "coinbase amount mismatch");
        }
    } else {
        // Foundation reward отключен - должен быть 1 output
        if (coinbase.outputs.size() != 1) {
            return state.invalid(RejectCode::INVALID, "bad-cb-outputs",
                "coinbase must have 1 output when foundation disabled");
        }

        if (coinbase.outputs[0].value > total_reward) {
            return state.invalid(RejectCode::INVALID, "bad-cb-amount",
                "coinbase pays too much");
        }
    }

    return true;
}

} // namespace geocoin::consensus
