#include "geocoin/wallet/wallet.hpp"
#include "geocoin/wallet/wallet_internal.hpp"
#include "geocoin/utils/u64_math.hpp"

#include <algorithm>
#include <cstddef>

namespace geocoin::wallet {

void Wallet::rebuild_tx_index_locked() {
    tx_index_.clear();
    tx_index_.reserve(transactions_.size());
    for (size_t i = 0; i < transactions_.size(); ++i) {
        tx_index_[transactions_[i].tx_hash] = i;
    }
}

void Wallet::add_transaction(const WalletTransaction& wallet_tx) {
    Hash256 tx_hash;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = tx_index_.find(wallet_tx.tx_hash);
        if (it != tx_index_.end()) {
            transactions_[it->second] = wallet_tx;
        } else {
            transactions_.push_back(wallet_tx);
            tx_index_[wallet_tx.tx_hash] = transactions_.size() - 1;
        }
        tx_hash = wallet_tx.tx_hash;
    }

    // Уведомляем UI (как в Bitcoin)
    // ВАЖНО: делаем это вне mutex кошелька, чтобы избежать дедлоков с UI callbacks,
    // которые могут читать состояние кошелька (например, get_transactions()).
    NotifyTransactionChanged(this, tx_hash);
}


std::vector<WalletTransaction> Wallet::get_transactions(
    const Address& address,
    size_t limit,
    size_t offset
) const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<WalletTransaction> result;
    size_t matched = 0;

    for (const auto& tx : transactions_) {
        if (!address.empty()) {
            bool found = false;

            for (const auto& output : tx.tx.outputs) {
                if (output.address == address) {
                    found = true;
                    break;
                }
            }

            if (!found) {
                continue;
            }
        }

        if (matched < offset) {
            ++matched;
            continue;
        }

        result.push_back(tx);
        ++matched;

        if (limit > 0 && result.size() >= limit) {
            break;
        }
    }

    return result;
}

std::vector<WalletTransaction> Wallet::get_recent_transactions(
    const Address& address,
    size_t limit,
    size_t skip
) const {
    std::lock_guard<std::mutex> lock(mutex_);

    // A spend from the address may send nothing back to it, so outputs alone
    // miss it; resolve inputs through earlier wallet transactions as well.
    const auto touches_address = [&](const WalletTransaction& wtx) {
        for (const auto& output : wtx.tx.outputs) {
            if (output.address == address) {
                return true;
            }
        }
        for (const auto& input : wtx.tx.inputs) {
            const auto prev = tx_index_.find(input.prev_tx_hash);
            if (prev == tx_index_.end()) {
                continue;
            }
            const auto& prev_outputs = transactions_[prev->second].tx.outputs;
            if (input.vout < prev_outputs.size() && prev_outputs[input.vout].address == address) {
                return true;
            }
        }
        return false;
    };

    std::vector<WalletTransaction> result;
    size_t skipped = 0;
    for (auto it = transactions_.rbegin(); it != transactions_.rend(); ++it) {
        if (limit > 0 && result.size() >= limit) {
            break;
        }
        if (!address.empty() && !touches_address(*it)) {
            continue;
        }
        if (skipped < skip) {
            ++skipped;
            continue;
        }
        result.push_back(*it);
    }
    std::reverse(result.begin(), result.end());
    return result;
}

bool Wallet::discard_unconfirmed_transaction(const Hash256& tx_hash) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = tx_index_.find(tx_hash);
        if (it == tx_index_.end() || transactions_[it->second].block_height != 0) {
            return false;
        }
        transactions_.erase(transactions_.begin() + static_cast<std::ptrdiff_t>(it->second));
        rebuild_tx_index_locked();
    }
    NotifyTransactionChanged(this, tx_hash);
    return true;
}

int64_t Wallet::get_debit(WalletTransaction& wtx, std::shared_ptr<state::ChainState> chain_state) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return get_debit_locked(wtx, chain_state);
}

int64_t Wallet::get_debit_locked(WalletTransaction& wtx, std::shared_ptr<state::ChainState> chain_state) const {
    // Возвращаем кэшированное значение, если доступно (как в Bitcoin)
    if (wtx.debit_cached) {
        return wtx.debit_cache;
    }

    // Коинбейс не имеет дебита
    if (wtx.is_coinbase) {
        wtx.debit_cache = 0;
        wtx.debit_cached = true;
        return 0;
    }

    int64_t debit = 0;

    // Считаем дебит по каждому входу
    for (const auto& input : wtx.tx.inputs) {
        // Сначала ищем предыдущую транзакцию в кошельке (быстрый путь)
        bool found = false;
        auto it = tx_index_.find(input.prev_tx_hash);
        if (it != tx_index_.end()) {
            const auto& prev_wtx = transactions_[it->second];
            if (input.vout < prev_wtx.tx.outputs.size()) {
                const auto& prev_output = prev_wtx.tx.outputs[input.vout];
                try {
                    Address in_addr(prev_output.address);
                    if (keys_.find(in_addr) != keys_.end() ||
                        multisig_addresses_.find(in_addr.str()) != multisig_addresses_.end()) {
                        if (!checked_accumulate_nonnegative(debit, prev_output.value)) {
                            wtx.debit_cache = 0;
                            wtx.debit_cached = true;
                            return 0;
                        }
                    }
                } catch (...) {}
                found = true;
            }
        }

        // Если не найдено в кошельке — пробуем chain_state (медленный путь, только для новых транзакций)
        if (!found && chain_state) {
            state::UTXO prev_utxo;
            if (chain_state->get_utxo(input.prev_tx_hash, input.vout, prev_utxo)) {
                try {
                    Address in_addr(prev_utxo.address);
                    if (keys_.find(in_addr) != keys_.end() ||
                        multisig_addresses_.find(in_addr.str()) != multisig_addresses_.end()) {
                        if (!checked_accumulate_nonnegative(debit, prev_utxo.value)) {
                            wtx.debit_cache = 0;
                            wtx.debit_cached = true;
                            return 0;
                        }
                    }
                } catch (...) {}
            }
        }
    }  // Конец внешнего цикла

    // Кэшируем результат (как в Bitcoin)
    wtx.debit_cache = debit;
    wtx.debit_cached = true;

    return debit;
}

int64_t Wallet::get_credit(const WalletTransaction& wtx) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return get_credit_locked(wtx);
}

int64_t Wallet::get_credit_locked(const WalletTransaction& wtx) const {
    // Возвращаем кэшированное значение, если доступно (как в Bitcoin)
    if (wtx.credit_cached) {
        return wtx.credit_cache;
    }

    int64_t credit = 0;

    // Считаем кредит по каждому выходу
    for (const auto& output : wtx.tx.outputs) {
        try {
            Address out_addr(output.address);
            if (keys_.find(out_addr) != keys_.end() ||
                multisig_addresses_.find(out_addr.str()) != multisig_addresses_.end()) {
                if (!checked_accumulate_nonnegative(credit, output.value)) {
                    wtx.credit_cache = 0;
                    wtx.credit_cached = true;
                    return 0;
                }
            }
        } catch (...) {}
    }

    // Кэшируем результат (как в Bitcoin)
    wtx.credit_cache = credit;
    wtx.credit_cached = true;

    return credit;
}

} // namespace geocoin::wallet
