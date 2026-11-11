#include "geocoin/wallet/wallet.hpp"
#include "geocoin/wallet/wallet_internal.hpp"
#include "geocoin/crypto/addresses.hpp"
#include "geocoin/crypto/encryption.hpp"
#include "geocoin/crypto/signatures.hpp"
#include "geocoin/crypto/geo_area.hpp"
#include "geocoin/utils/logger.hpp"
#include "geocoin/utils/secure_memory.hpp"
#include "geocoin/utils/u64_math.hpp"
#include "geocoin/consensus/consensus.hpp"
#include <cerrno>
#include <cstdlib>
#if defined(_WIN32)
#include "geocoin/utils/windows_secure_file.hpp"
#include <fcntl.h>
#include <io.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif
#include <sstream>
#include <openssl/rand.h>
#include "geocoin/crypto/ioke_sha256.h"
#include <openssl/crypto.h>
#include <openssl/hmac.h>
#include <openssl/evp.h>
#include <algorithm>
#include <array>
#include <set>
#include <filesystem>
#include <limits>
#include <new>
#include <chrono>

namespace geocoin::wallet {

bool Wallet::create_nft_create(
    const nft::NFTMetadata& metadata,
    uint32_t royalty_percent,
    Amount fee,
    std::shared_ptr<state::ChainState> chain_state,
    core::Transaction& tx,
    std::span<const core::Transaction> pending
) {
    if (!chain_state) {
        return false;
    }
    if (fee < consensus::params::MIN_TX_FEE) {
        return false;
    }

    const Address creator = get_address();
    bytes owner_key;
    utils::ScopedContainerCleanser owner_key_cleanser(owner_key);
    auto utxos = select_utxos_for_address(creator, 0, fee, chain_state, &owner_key, pending);
    if (utxos.empty() || owner_key.empty()) {
        return false;
    }

    Amount total_input = 0;
    for (const auto& u : utxos) {
        if (!checked_accumulate_nonnegative(total_input, u.value)) {
            return false;
        }
    }
    if (total_input <= fee) {
        return false;
    }

    tx.inputs.clear();
    tx.outputs.clear();
    tx.version = 2;
    tx.type = core::TransactionType::NFT_CREATE;
    tx.timestamp = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
    tx.lock_time = 0;

    for (const auto& u : utxos) {
        core::TxInput input;
        input.prev_tx_hash = u.tx_hash;
        input.vout = u.vout;
        tx.inputs.push_back(input);
    }

    Amount change = 0;
    if (!checked_sub_amount(total_input, fee, change)) {
        return false;
    }
    if (change < static_cast<Amount>(consensus::params::DUST_LIMIT)) {
        return false;
    }
    if (change > 0) {
        core::TxOutput out_change;
        out_change.value = change;
        out_change.address = utxos[0].address;
        tx.outputs.push_back(out_change);
    }

    nft::NFTCreationTransaction p;
    p.metadata = metadata;
    p.nft_id = metadata.calculate_hash();
    p.creator_address = creator;
    p.royalty_percent = royalty_percent;
    p.signature.clear();

    const bytes unsigned_payload = p.serialize_for_signing();
    const Hash256 msg32 = crypto::sha256(unsigned_payload);
    p.signature = crypto::sign_recoverable_message(msg32, owner_key);
    tx.nft_create = p;

    tx.sign(owner_key);

    WalletTransaction wtx;
    wtx.tx = tx;
    wtx.tx_hash = tx.get_hash();
    wtx.block_height = 0;
    wtx.timestamp = tx.timestamp;
    if (!checked_sub_amount(0, fee, wtx.amount)) {
        return false;
    }
    wtx.fee = fee;
    wtx.is_coinbase = false;
    wtx.memo = "";
    const_cast<Wallet*>(this)->add_transaction(wtx);
    const_cast<Wallet*>(this)->mark_address_used(creator);

    return true;
}

bool Wallet::create_nft_transfer(
    const Hash256& nft_id,
    const Address& to,
    Amount fee,
    std::shared_ptr<state::ChainState> chain_state,
    core::Transaction& tx,
    std::span<const core::Transaction> pending
) {
    if (!chain_state) {
        return false;
    }
    if (fee < consensus::params::MIN_TX_FEE) {
        return false;
    }
    if (to.empty()) {
        return false;
    }

    const Address owner = chain_state->get_nft_owner(nft_id);
    if (owner.empty()) {
        return false;
    }

    bytes owner_key;
    utils::ScopedContainerCleanser owner_key_cleanser(owner_key);
    auto utxos = select_utxos_for_address(owner, 0, fee, chain_state, &owner_key, pending);
    if (utxos.empty() || owner_key.empty()) {
        return false;
    }

    Amount total_input = 0;
    for (const auto& u : utxos) {
        if (!checked_accumulate_nonnegative(total_input, u.value)) {
            return false;
        }
    }
    if (total_input <= fee) {
        return false;
    }

    tx.inputs.clear();
    tx.outputs.clear();
    tx.version = 2;
    tx.type = core::TransactionType::NFT_TRANSFER;
    tx.timestamp = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
    tx.lock_time = 0;

    for (const auto& u : utxos) {
        core::TxInput input;
        input.prev_tx_hash = u.tx_hash;
        input.vout = u.vout;
        tx.inputs.push_back(input);
    }

    Amount change = 0;
    if (!checked_sub_amount(total_input, fee, change)) {
        return false;
    }
    if (change < static_cast<Amount>(consensus::params::DUST_LIMIT)) {
        return false;
    }
    if (change > 0) {
        core::TxOutput out_change;
        out_change.value = change;
        out_change.address = utxos[0].address;
        tx.outputs.push_back(out_change);
    }

    nft::NFTTransferTransaction p;
    p.nft_id = nft_id;
    p.from = owner;
    p.to = to;
    p.fee = fee;
    p.signature.clear();

    const bytes unsigned_payload = p.serialize_for_signing();
    const Hash256 msg32 = crypto::sha256(unsigned_payload);
    p.signature = crypto::sign_recoverable_message(msg32, owner_key);
    tx.nft_transfer = p;

    tx.sign(owner_key);

    WalletTransaction wtx;
    wtx.tx = tx;
    wtx.tx_hash = tx.get_hash();
    wtx.block_height = 0;
    wtx.timestamp = tx.timestamp;
    if (!checked_sub_amount(0, fee, wtx.amount)) {
        return false;
    }
    wtx.fee = fee;
    wtx.is_coinbase = false;
    wtx.memo = "";
    const_cast<Wallet*>(this)->add_transaction(wtx);

    const_cast<Wallet*>(this)->mark_address_used(owner);

    return true;
}

bool Wallet::create_nft_burn(
    const Hash256& nft_id,
    Amount fee,
    std::shared_ptr<state::ChainState> chain_state,
    core::Transaction& tx,
    std::span<const core::Transaction> pending
) {
    if (!chain_state) {
        return false;
    }
    if (fee < consensus::params::MIN_TX_FEE) {
        return false;
    }

    const Address owner = chain_state->get_nft_owner(nft_id);
    if (owner.empty()) {
        return false;
    }

    bytes owner_key;
    utils::ScopedContainerCleanser owner_key_cleanser(owner_key);
    auto utxos = select_utxos_for_address(owner, 0, fee, chain_state, &owner_key, pending);
    if (utxos.empty() || owner_key.empty()) {
        return false;
    }

    Amount total_input = 0;
    for (const auto& u : utxos) {
        if (!checked_accumulate_nonnegative(total_input, u.value)) {
            return false;
        }
    }
    if (total_input <= fee) {
        return false;
    }

    tx.inputs.clear();
    tx.outputs.clear();
    tx.version = 2;
    tx.type = core::TransactionType::NFT_BURN;
    tx.timestamp = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
    tx.lock_time = 0;

    for (const auto& u : utxos) {
        core::TxInput input;
        input.prev_tx_hash = u.tx_hash;
        input.vout = u.vout;
        tx.inputs.push_back(input);
    }

    Amount change = 0;
    if (!checked_sub_amount(total_input, fee, change)) {
        return false;
    }
    if (change > 0 && change < static_cast<Amount>(consensus::params::DUST_LIMIT)) {
        fee = total_input;
        change = 0;
    }
    if (change > 0) {
        core::TxOutput out_change;
        out_change.value = change;
        out_change.address = utxos[0].address;
        tx.outputs.push_back(out_change);
    }

    nft::NFTBurnTransaction p;
    p.nft_id = nft_id;
    p.owner = owner;
    p.fee = fee;
    p.signature.clear();

    const bytes unsigned_payload = p.serialize_for_signing();
    const Hash256 msg32 = crypto::sha256(unsigned_payload);
    p.signature = crypto::sign_recoverable_message(msg32, owner_key);
    tx.nft_burn = p;

    tx.sign(owner_key);

    WalletTransaction wtx;
    wtx.tx = tx;
    wtx.tx_hash = tx.get_hash();
    wtx.block_height = 0;
    wtx.timestamp = tx.timestamp;
    if (!checked_sub_amount(0, fee, wtx.amount)) {
        return false;
    }
    wtx.fee = fee;
    wtx.is_coinbase = false;
    wtx.memo = "";
    const_cast<Wallet*>(this)->add_transaction(wtx);

    const_cast<Wallet*>(this)->mark_address_used(owner);

    return true;
}

} // namespace geocoin::wallet
