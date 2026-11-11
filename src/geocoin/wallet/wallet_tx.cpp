#include "geocoin/wallet/wallet.hpp"
#include "geocoin/wallet/address_funds.hpp"
#include "geocoin/wallet/wallet_internal.hpp"
#include "geocoin/crypto/addresses.hpp"
#include "geocoin/utils/logger.hpp"
#include "geocoin/utils/secure_memory.hpp"
#include "geocoin/utils/u64_math.hpp"
#include "geocoin/consensus/consensus.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <limits>
#include <map>
#include <sstream>
#include <vector>

namespace geocoin::wallet {

namespace {
inline bool SyncDebugEnabled() {
    static const bool enabled = (std::getenv("GEOCOIN_SYNC_DEBUG") != nullptr);
    return enabled;
}
}

Amount Wallet::estimate_required_fee(
    Amount amount,
    std::shared_ptr<state::ChainState> chain_state,
    std::span<const core::Transaction> pending,
    Amount minimum_fee
) const {
    if (amount <= 0) {
        return 0;
    }
    // A caller-supplied fee above the minimum can require more inputs than
    // the minimum would. Select with that fee, so the size estimate below
    // describes the transaction that will actually be built.
    const Amount fee_floor = std::max(consensus::params::MIN_TX_FEE, minimum_fee);
    if (!chain_state) {
        return fee_floor;
    }

    Amount fee = fee_floor;

    for (int i = 0; i < 6; ++i) {
        Amount total_required = 0;
        if (!checked_add_amount(amount, fee, total_required)) {
            return 0;
        }

        auto utxos = select_utxos(amount, fee, chain_state, nullptr, pending);
        if (utxos.empty()) {
            return fee;
        }

        size_t input_count = utxos.size();
        Amount total_input = 0;
        for (const auto& utxo : utxos) {
            if (!checked_accumulate_nonnegative(total_input, utxo.value)) {
                return 0;
            }
        }

        Amount change = 0;
        if (total_input < total_required ||
            !checked_sub_amount(total_input, total_required, change)) {
            return 0;
        }
        size_t output_count = 1; // получатель
        if (change >= static_cast<Amount>(consensus::params::DUST_LIMIT)) {
            output_count += 1; // сдача
        }

        size_t tx_size = consensus::params::TX_BASE_SIZE +
            input_count * consensus::params::TX_INPUT_SIZE +
            output_count * consensus::params::TX_OUTPUT_SIZE;

        const uint64_t rate = static_cast<uint64_t>(consensus::params::FEE_RATE_PER_KB);
        const uint64_t calculated_fee = utils::mul_div_u64_ceil(
            static_cast<uint64_t>(tx_size),
            rate,
            1000u
        );
        if (calculated_fee > static_cast<uint64_t>((std::numeric_limits<Amount>::max)())) {
            return 0;
        }
        const Amount size_fee = static_cast<Amount>(calculated_fee);
        const Amount fee_needed = std::max(fee_floor, size_fee);

        if (fee_needed == fee) {
            break;
        }
        fee = fee_needed;
    }

    return fee;
}

bool Wallet::create_transaction(
    const Address& to_address,
    Amount amount,
    Amount fee,
    std::shared_ptr<state::ChainState> chain_state,
    core::Transaction& tx,
    std::span<const core::Transaction> pending
) {
    // Используем адрес источника для сдачи (по умолчанию)
    return create_transaction_with_change(
        to_address, amount, fee, Address(""), chain_state, tx, pending);
}

bool Wallet::create_transaction_with_change(
    const Address& to_address,
    Amount amount,
    Amount fee,
    const Address& change_address,
    std::shared_ptr<state::ChainState> chain_state,
    core::Transaction& tx,
    std::span<const core::Transaction> pending
) {
    if (!chain_state) {
        GEO_LOG_ERROR("wallet", "create_transaction_with_change: chain_state is null");
        return false;
    }
    if (amount <= 0 || fee < 0) {
        return false;
    }

    Amount total_required = 0;
    if (!checked_add_amount(amount, fee, total_required)) {
        return false;
    }

    if (SyncDebugEnabled()) {
        GEO_LOG_DEBUG(
            "wallet",
            "create_transaction_with_change: to_address=" << to_address.str()
                << ", amount=" << amount
                << ", fee=" << fee
        );
    }

    // Выбираем UTXO
    std::map<Address, secure_bytes> used_keys;
    auto utxos = select_utxos(amount, fee, chain_state, &used_keys, pending);
    if (utxos.empty()) {
        GEO_LOG_ERROR("wallet", "create_transaction_with_change: No UTXOs selected");
        return false;
    }

    if (SyncDebugEnabled()) {
        GEO_LOG_DEBUG("wallet", "create_transaction_with_change: Selected " << utxos.size() << " UTXOs");
    }

    // Вычисляем общую сумму входов
    Amount total_input = 0;
    for (const auto& utxo : utxos) {
        if (!checked_accumulate_nonnegative(total_input, utxo.value)) {
            return false;
        }
    }
    if (total_input < total_required) {
        return false;
    }

    // Создаем транзакцию
    tx.inputs.clear();
    tx.outputs.clear();
    tx.version = 2;
    tx.type = core::TransactionType::STANDARD;
    tx.timestamp = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
    tx.lock_time = 0;

    // Добавляем входы
    for (const auto& utxo : utxos) {
        core::TxInput input;
        input.prev_tx_hash = utxo.tx_hash;
        input.vout = utxo.vout;
        tx.inputs.push_back(input);
    }

    // Добавляем выход получателю
    core::TxOutput output_to;
    output_to.value = amount;
    output_to.address = to_address;
    tx.outputs.push_back(output_to);

    // Добавляем сдачу если нужно
    Amount change = 0;
    if (!checked_sub_amount(total_input, total_required, change)) {
        return false;
    }
    if (change >= static_cast<Amount>(consensus::params::DUST_LIMIT)) {
        // Определяем адрес для сдачи
        Address final_change_address;

        if (!change_address.empty()) {
            // Используем указанный адрес для сдачи
            final_change_address = change_address;
        } else {
            // По умолчанию: тот же адрес источника
            final_change_address = utxos[0].address;
        }

        core::TxOutput output_change;
        output_change.value = change;
        output_change.address = final_change_address;
        tx.outputs.push_back(output_change);
    }

    // Подписываем транзакцию
    if (!sign_transaction(tx, used_keys)) {
        return false;
    }
    compact_standard_witnesses_with_cache(tx, chain_state);

    // Отмечаем адреса как использованные
    for (const auto& [address, key] : used_keys) {
        const_cast<Wallet*>(this)->mark_address_used(address);
    }

    // Добавляем транзакцию в историю кошелька
    WalletTransaction wtx;
    wtx.tx = tx;
    wtx.tx_hash = tx.get_hash();
    wtx.block_height = 0;  // Еще не в блоке
    wtx.timestamp = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
    if (!checked_sub_amount(0, total_required, wtx.amount)) {
        return false;
    }
    wtx.fee = fee;
    wtx.is_coinbase = false;
    wtx.memo = "";

    const_cast<Wallet*>(this)->add_transaction(wtx);
    return true;
}

bool Wallet::move_funds(
    const Address& from_address,
    const Address& to_address,
    Amount fee,
    std::shared_ptr<state::ChainState> chain_state,
    core::Transaction& tx,
    std::span<const core::Transaction> pending
) {
    if (!chain_state) {
        return false;
    }
    if (fee < 0) {
        return false;
    }

    // Проверяем что from_address есть в кошельке
    if (!has_key(from_address)) {
        return false;
    }

    // Получаем все UTXO с from_address
    auto utxos = chain_state->get_utxos_for_address(from_address);
    if (utxos.empty()) {
        return false;
    }

    std::vector<state::UTXO> mature_utxos;
    const Height current_height = chain_state->get_height();
    const PendingSpendIndex reserved(pending);
    for (const auto& utxo : utxos) {
        if (!utxo_is_spendable(utxo, current_height) ||
            reserved.contains(utxo.tx_hash, utxo.vout)) {
            continue;
        }
        mature_utxos.push_back(utxo);
    }

    if (mature_utxos.empty()) {
        return false;
    }

    // Вычисляем общую сумму
    Amount total_input = 0;
    for (const auto& utxo : mature_utxos) {
        if (!checked_accumulate_nonnegative(total_input, utxo.value)) {
            return false;
        }
    }

    // Проверяем что хватает на комиссию
    if (total_input <= fee) {
        return false;
    }

    Amount amount_to_send = 0;
    if (!checked_sub_amount(total_input, fee, amount_to_send)) {
        return false;
    }

    // Создаем транзакцию
    tx.inputs.clear();
    tx.outputs.clear();
    tx.version = 2;
    tx.type = core::TransactionType::STANDARD;
    tx.timestamp = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
    tx.lock_time = 0;

    // Добавляем все входы
    for (const auto& utxo : mature_utxos) {
        core::TxInput input;
        input.prev_tx_hash = utxo.tx_hash;
        input.vout = utxo.vout;
        tx.inputs.push_back(input);
    }

    // Один выход: все монеты на to_address
    core::TxOutput output;
    output.value = amount_to_send;
    output.address = to_address;
    tx.outputs.push_back(output);

    // Получаем приватный ключ
    bytes private_key;
    utils::ScopedContainerCleanser private_key_cleanser(private_key);
    if (!get_private_key(from_address, private_key)) {
        return false;
    }

    // Подписываем транзакцию
    std::map<Address, secure_bytes> used_keys;
    used_keys[from_address].assign(private_key.begin(), private_key.end());

    if (!sign_transaction(tx, used_keys)) {
        return false;
    }
    compact_standard_witnesses_with_cache(tx, chain_state);

    // Отмечаем адрес как использованный
    const_cast<Wallet*>(this)->mark_address_used(from_address);
    return true;
}

std::vector<state::UTXO> Wallet::select_utxos(
    Amount amount,
    Amount fee,
    std::shared_ptr<state::ChainState> chain_state,
    std::map<Address, secure_bytes>* used_keys,
    std::span<const core::Transaction> pending
) const {
    if (!chain_state) {
        return {};
    }

    // ВАЖНО: Все входы должны быть от ОДНОГО адреса!
    // Пробуем каждый адрес по отдельности
    // Snapshot addresses under the wallet lock; never retain a keys_ iterator
    // across a chainstate call. Fee estimation does not need private keys.
    for (const auto& address : get_spendable_addresses()) {
        bytes private_key;
        utils::ScopedContainerCleanser private_key_cleanser(private_key);
        auto selected = select_utxos_for_address(
            address, amount, fee, chain_state, used_keys ? &private_key : nullptr, pending);
        if (!selected.empty()) {
            if (used_keys) {
                (*used_keys)[address].assign(private_key.begin(), private_key.end());
            }
            return selected;
        }
    }

    // Не нашли достаточно средств ни на одном адресе
    return {};
}

std::vector<state::UTXO> Wallet::select_utxos_for_address(
    const Address& address,
    Amount amount,
    Amount fee,
    std::shared_ptr<state::ChainState> chain_state,
    bytes* private_key,
    std::span<const core::Transaction> pending
) const {
    if (private_key) {
        utils::cleanse_container(*private_key);
    }
    if (!chain_state || address.empty() || amount < 0 || fee < 0) {
        return {};
    }

    Amount needed = 0;
    if (!checked_add_amount(amount, fee, needed)) {
        return {};
    }

    if (!has_key(address)) {
        return {};
    }

    auto utxos = chain_state->get_utxos_for_address(address);

    std::vector<state::UTXO> mature_utxos;
    const Height current_height = chain_state->get_height();
    const PendingSpendIndex reserved(pending);
    for (const auto& utxo : utxos) {
        if (!utxo_is_spendable(utxo, current_height) ||
            reserved.contains(utxo.tx_hash, utxo.vout)) {
            continue;
        }
        mature_utxos.push_back(utxo);
    }

    std::sort(mature_utxos.begin(), mature_utxos.end(), [](const auto& a, const auto& b) {
        return a.value > b.value;
    });

    std::vector<state::UTXO> selected;
    Amount total = 0;
    for (const auto& utxo : mature_utxos) {
        if (!checked_accumulate_nonnegative(total, utxo.value)) {
            return {};
        }
        selected.push_back(utxo);

        if (total >= needed) {
            // The wallet may have been reset while selecting UTXOs. Copy the
            // selected key under its lock, or fail if it no longer exists.
            if (private_key && !get_private_key(address, *private_key)) {
                return {};
            }
            return selected;
        }
    }

    return {};
}

bool Wallet::sign_transaction(
    core::Transaction& tx,
    const std::map<Address, secure_bytes>& keys
) {
    // Транзакция подписывается ЦЕЛИКОМ одним приватным ключом
    // Все входы должны принадлежать одному владельцу

    if (keys.empty()) {
        return false;
    }

    // Берем первый приватный ключ (все входы от одного владельца)
    bytes private_key(keys.begin()->second.begin(), keys.begin()->second.end());
    utils::ScopedContainerCleanser private_key_cleanser(private_key);

    // Подписываем транзакцию через Falcon-512
    tx.sign(private_key);

    return true;
}

} // namespace geocoin::wallet
