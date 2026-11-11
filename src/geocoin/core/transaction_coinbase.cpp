#include "geocoin/core/transaction.hpp"
#include "geocoin/core/genesis.hpp"
#include "geocoin/consensus/consensus.hpp"
#include <chrono>
#include <stdexcept>

namespace geocoin::core {

bool Transaction::is_coinbase() const {
    // Coinbase имеет ровно один вход
    if (inputs.size() != 1) {
        return false;
    }

    // Вход должен иметь prev_tx_hash = 0 и vout = 0xFFFFFFFF
    const auto& input = inputs[0];

    bool is_null_hash = true;
    for (auto byte : input.prev_tx_hash) {
        if (byte != 0) {
            is_null_hash = false;
            break;
        }
    }

    return is_null_hash && input.vout == 0xFFFFFFFF;
}

Transaction Transaction::create_coinbase(
    Height height,
    const Address& miner_address,
    Amount reward,
    Amount fees
) {
    // The compatibility overload remains independent of runtime ChainParams so
    // geocoin_core does not acquire a circular dependency on geocoin_consensus.
    // Production mining and genesis paths pass the selected/live foundation
    // address explicitly through the five-argument overload below.
    return create_coinbase(
        height,
        miner_address,
        reward,
        fees,
        Address(consensus::params::FOUNDATION_ADDRESS)
    );
}

Transaction Transaction::create_coinbase(
    Height height,
    const Address& miner_address,
    Amount reward,
    Amount fees,
    const Address& foundation_address
) {
    Transaction tx;
    tx.version = 2;

    // ВАЖНО: Для genesis блока используем фиксированный timestamp!
    // Для обычных блоков - текущее время
    if (height == 0) {
        tx.timestamp = GENESIS_TIMESTAMP;  // Константа!
    } else {
        tx.timestamp = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();
    }

    // Создаем специальный вход для coinbase
    TxInput coinbase_input;
    coinbase_input.prev_tx_hash = {};  // Все нули
    coinbase_input.vout = 0xFFFFFFFF;

    // В script_sig помещаем высоту блока (BIP34)
    coinbase_input.script_sig.clear();
    for (int i = 0; i < 8; i++) {
        coinbase_input.script_sig.push_back((height >> (i * 8)) & 0xFF);
    }

    // Coinbase не имеет подписи
    coinbase_input.signature = {};

    tx.inputs.push_back(coinbase_input);

    // Вычисляем общую награду без signed overflow. В штатном пути эти
    // значения уже проверены майнером, но конструктор также используется RPC
    // и тестовыми/внешними вызывающими сторонами.
    Amount total = 0;
    if (reward < 0 || fees < 0 || !checked_add_amount(reward, fees, total)) {
        throw std::invalid_argument("invalid or overflowing coinbase reward");
    }

    // === ОСОБЫЙ СЛУЧАЙ: Genesis блок (height == 0) ===
    // Genesis блок отправляет 100% награды на адрес фонда
    // Это начальный капитал для развития проекта
    if (height == 0) {
        TxOutput genesis_output;
        genesis_output.value = total;
        genesis_output.address = foundation_address;
        tx.outputs.push_back(genesis_output);
        return tx;
    }

    // === ОБЫЧНЫЕ БЛОКИ (height > 0) ===
    // Вычисляем награды
    Amount foundation_amount = 0;
    Amount miner_amount = total;

    if (consensus::is_foundation_reward_active(height)) {
        foundation_amount = consensus::calculate_foundation_reward_cut(total);
        miner_amount = total - foundation_amount;
    }

    // Выход 0: Майнер (90% или 100% если foundation отключен)
    TxOutput miner_output;
    miner_output.value = miner_amount;
    miner_output.address = miner_address;
    tx.outputs.push_back(miner_output);

    // До активации interest pool: отдельный output на фонд.
    // После активации доля фонда учитывается в state как pool_delta и не попадает в coinbase outputs.
    if (consensus::is_foundation_reward_active(height) &&
        !consensus::is_foundation_reward_redirected_to_pool(height)) {
        TxOutput foundation_output;
        foundation_output.value = foundation_amount;
        foundation_output.address = foundation_address;
        tx.outputs.push_back(foundation_output);
    }

    return tx;
}

} // namespace geocoin::core
