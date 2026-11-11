#include "geocoin/network/mempool_policy.hpp"
#include "geocoin/consensus/consensus.hpp"
#include "geocoin/crypto/addresses.hpp"
#include "geocoin/crypto/geo_area.hpp"
#include "geocoin/utils/u64_math.hpp"
#include <algorithm>
#include <chrono>
#include <limits>
#include <set>
#include <sstream>
#include <iomanip>

namespace geocoin::network {

static bool is_policy_exempt(const core::Transaction& tx) {
    return tx.type == core::TransactionType::ANGLE_COMMITMENT ||
           tx.type == core::TransactionType::SPEND_DEPOSIT ||
           tx.type == core::TransactionType::UNLOCK_DEPOSIT ||
           tx.type == core::TransactionType::ISSUE_SIDECHAIN ||
           tx.type == core::TransactionType::REGISTER_ACCOUNTING_SIDECHAIN ||
           tx.type == core::TransactionType::MINT_SIDECHAIN ||
           tx.type == core::TransactionType::REDEEM_SIDECHAIN ||
           tx.type == core::TransactionType::SIDECHAIN_CHECKPOINT ||
           tx.type == core::TransactionType::UPDATE_SIDECHAIN_COMMITTEE ||
           tx.type == core::TransactionType::SLASH_DEPOSIT ||
           tx.type == core::TransactionType::BATCH_SIDECHAIN_CHECKPOINT ||
           tx.type == core::TransactionType::UPDATE_FOUNDATION_ADDRESS ||
           tx.type == core::TransactionType::DECOMMISSION_SIDECHAIN;
}

uint64_t MempoolPolicy::required_fee_for_size(
    size_t serialized_size,
    const MempoolPolicyConfig& config
) {
    const utils::U128Parts product = utils::mul_u64_wide(
        static_cast<uint64_t>(serialized_size),
        config.min_relay_fee_rate_per_kb
    );
    uint64_t size_fee = (std::numeric_limits<uint64_t>::max)();
    if (product.hi < 1000u) {
        uint64_t remainder = 0;
        size_fee = utils::div_u128_by_u64_fast(
            product.hi,
            product.lo,
            1000u,
            &remainder
        );
        if (remainder != 0 && size_fee != (std::numeric_limits<uint64_t>::max)()) {
            ++size_fee;
        }
    }
    return std::max(config.min_relay_fee, size_fee);
}

bool MempoolPolicy::check(const core::Transaction& tx,
                          const state::ChainState& chain_state,
                          uint64_t fee,
                          const MempoolPolicyConfig& config,
                          std::string* reason,
                          std::optional<Timestamp> block_time) {
    // Reward and distribution transactions are created by the block assembler.
    // Their exclusion is independent of operator-selected relay fee settings.
    if (tx.is_coinbase() || tx.type == core::TransactionType::INTEREST_DISTRIBUTION) {
        if (reason) *reason = "system-transaction";
        return false;
    }

    if (tx.lock_time != 0) {
        const Height height = chain_state.get_height();
        const Timestamp time = block_time ? *block_time :
            static_cast<Timestamp>(std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        if (height == (std::numeric_limits<Height>::max)() ||
            !consensus::is_final_transaction(tx, height + 1, time)) {
            if (reason) *reason = "non-final";
            return false;
        }
    }

    // Value-bearing outputs in the address-only UTXO model must use a
    // canonical GEO/GEOM address. Consensus still permits explicit burn-like
    // outputs for historical/block compatibility, but such transactions must
    // never enter the relay mempool or be produced by the wallet RPC.
    for (const auto& output : tx.outputs) {
        if (output.value > 0 && !crypto::verify_address(output.address)) {
            if (reason) {
                *reason = "invalid-output-address";
            }
            return false;
        }
    }

    // Hardening soft-fork rules are always enforced for relay, so nothing
    // admitted before the activation height can invalidate a later template.
    // A zero-padded Falcon encoding also verifies, but would let a relayer
    // change the txid of a transaction it did not sign.
    if (!consensus::has_canonical_signature_encodings(tx)) {
        if (reason) *reason = "non-canonical-signature";
        return false;
    }
    if (!consensus::is_revoke_delegation_change_standard(tx)) {
        if (reason) *reason = "revoke-delegation-dust-change";
        return false;
    }

    if (is_policy_exempt(tx)) {
        return true;
    }

    if (config.enforce_coinbase_maturity) {
        const Height current_height = chain_state.get_height();
        for (const auto& input : tx.inputs) {
            state::UTXO utxo;
            if (!chain_state.get_utxo(input.prev_tx_hash, input.vout, utxo)) {
                if (reason) {
                    *reason = "utxo-not-found";
                }
                return false;
            }

            if (current_height < utxo.height) {
                if (reason) {
                    *reason = "utxo-from-future";
                }
                return false;
            }

            if (utxo.is_coinbase) {
                const Height confirmations = current_height - utxo.height + 1;
                if (confirmations < consensus::params::COINBASE_MATURITY) {
                    if (reason) {
                        *reason = "coinbase-not-mature";
                    }
                    return false;
                }
            }
        }
    }

    if (fee < config.min_relay_fee) {
        if (reason) {
            *reason = "fee-too-low";
        }
        return false;
    }

    if (tx.type == core::TransactionType::STANDARD) {
        uint32_t new_keys = 0;
        std::set<std::string> seen;
        for (const auto& w : tx.witnesses) {
            if (w.public_key.empty()) {
                continue;
            }
            if (w.public_key.size() != consensus::params::WITNESS_PUBKEY_SIZE) {
                if (reason) {
                    *reason = "witness-pubkey-size-invalid";
                }
                return false;
            }
            if (crypto::sha256(w.public_key) != w.public_key_hash) {
                if (reason) {
                    *reason = "witness-pubkey-hash-mismatch";
                }
                return false;
            }

            std::ostringstream oss;
            for (auto b : w.public_key_hash) {
                oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(b);
            }
            const std::string hex = oss.str();
            if (!seen.insert(hex).second) {
                continue;
            }
            if (!chain_state.has_witness_pubkey(w.public_key_hash)) {
                ++new_keys;
                if (new_keys > config.max_new_witness_pubkeys_per_tx) {
                    if (reason) {
                        *reason = "too-many-new-witness-pubkeys";
                    }
                    return false;
                }
            }
        }

    }

    // Every user-created UTXO transaction pays for its serialized footprint,
    // not only STANDARD payments. This keeps typed payloads and multisig roots
    // from bypassing the relay fee rate while fees remain deliberately small.
    const uint64_t required_fee = required_fee_for_size(tx.serialize().size(), config);
    if (fee < required_fee) {
        if (reason) {
            *reason = "fee-rate-too-low";
        }
        return false;
    }

    return true;
}

} // namespace geocoin::network
