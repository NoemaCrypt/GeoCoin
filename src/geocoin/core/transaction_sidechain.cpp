#include "geocoin/core/transaction_codec.hpp"
#include "geocoin/core/transaction_internal.hpp"

#include "geocoin/crypto/geo_area.hpp"
#include "geocoin/crypto/signatures.hpp"

#include <algorithm>
#include <stdexcept>

namespace geocoin::core {

bytes build_sidechain_payload_for_signing(const Transaction& tx) {
    return serialize_protocol_authorization(tx);
}

bytes build_sidechain_checkpoint_payload_for_signing(const Transaction& tx) {
    return serialize_protocol_authorization(tx);
}

bytes build_sidechain_committee_update_payload_for_signing(const Transaction& tx) {
    return serialize_protocol_authorization(tx);
}

bytes build_sidechain_batch_checkpoint_payload_for_signing(const BatchCheckpointItem& item) {
    bytes out;
    out.push_back(static_cast<byte>(TransactionType::BATCH_SIDECHAIN_CHECKPOINT));
    utils::append_hash256(out, item.sidechain_id);
    utils::append_hash256(out, item.checkpoint);
    utils::append_u64_le(out, static_cast<uint64_t>(item.checkpoint_height));
    utils::append_u64_le(out, static_cast<uint64_t>(item.fee));
    out.push_back(item.version);
    utils::append_hash256(out, item.committee_id);
    const auto proof_len = utils::encode_varint(item.proof.size());
    out.insert(out.end(), proof_len.begin(), proof_len.end());
    if (!item.proof.empty()) {
        out.insert(out.end(), item.proof.begin(), item.proof.end());
    }
    return out;
}

} // namespace geocoin::core

namespace geocoin::core::tx_codec {

void append_sidechain_payload(bytes& data, const Transaction& tx) {
    if (tx.type == TransactionType::BATCH_SIDECHAIN_CHECKPOINT) {
        auto count = utils::encode_varint(tx.sidechain_batch_checkpoints.size());
        data.insert(data.end(), count.begin(), count.end());
        for (const auto& item : tx.sidechain_batch_checkpoints) {
            data.insert(data.end(), item.sidechain_id.begin(), item.sidechain_id.end());
            data.insert(data.end(), item.checkpoint.begin(), item.checkpoint.end());
            for (int i = 0; i < 8; i++) {
                data.push_back((static_cast<uint64_t>(item.checkpoint_height) >> (i * 8)) & 0xFF);
            }
            for (int i = 0; i < 8; i++) {
                data.push_back((static_cast<uint64_t>(item.fee) >> (i * 8)) & 0xFF);
            }
            data.push_back(item.version);
            data.insert(data.end(), item.committee_id.begin(), item.committee_id.end());
            auto sig_count = utils::encode_varint(item.committee_signatures.size());
            data.insert(data.end(), sig_count.begin(), sig_count.end());
            for (const auto& sig : item.committee_signatures) {
                auto slen = utils::encode_varint(sig.size());
                data.insert(data.end(), slen.begin(), slen.end());
                if (!sig.empty()) {
                    data.insert(data.end(), sig.begin(), sig.end());
                }
            }
            auto proof_len = utils::encode_varint(item.proof.size());
            data.insert(data.end(), proof_len.begin(), proof_len.end());
            if (!item.proof.empty()) {
                data.insert(data.end(), item.proof.begin(), item.proof.end());
            }
        }
        return;
    }

    utils::append_hash256(data, tx.sidechain_id);
    append_varbytes(data, tx.sidechain_owner.str());
    utils::append_u64_le(data, static_cast<uint64_t>(tx.sidechain_amount));
    utils::append_u64_le(data, static_cast<uint64_t>(tx.sidechain_unlock_height));
    utils::append_hash256(data, tx.sidechain_checkpoint);

    if (tx.type == TransactionType::ISSUE_SIDECHAIN) {
        append_varbytes(data, tx.sidechain_token_symbol);
        utils::append_u32_le(data, tx.sidechain_multiplier);
        utils::append_u32_le(data, tx.sidechain_fee_bps);
        utils::append_u32_le(data, tx.sidechain_max_multiplier);
        data.push_back(tx.sidechain_checkpoint_policy_id);
        data.push_back(tx.sidechain_checkpoint_version);
        append_committee_config(
            data,
            tx.sidechain_committee_m,
            tx.sidechain_committee_n,
            tx.sidechain_committee_members
        );
    } else if (tx.type == TransactionType::REGISTER_ACCOUNTING_SIDECHAIN) {
        append_varbytes(data, tx.sidechain_namespace);
        utils::append_hash256(data, tx.sidechain_schema_hash);
        data.push_back(tx.sidechain_checkpoint_policy_id);
        data.push_back(tx.sidechain_checkpoint_version);
        append_committee_config(
            data,
            tx.sidechain_committee_m,
            tx.sidechain_committee_n,
            tx.sidechain_committee_members
        );
    }

    if (tx.type == TransactionType::ISSUE_SIDECHAIN ||
        tx.type == TransactionType::REGISTER_ACCOUNTING_SIDECHAIN) {
        utils::append_hash256(data, tx.sidechain_committee_id);
    }

    if (tx.type == TransactionType::MINT_SIDECHAIN ||
        tx.type == TransactionType::REDEEM_SIDECHAIN ||
        tx.type == TransactionType::SLASH_DEPOSIT) {
        utils::append_hash256(data, tx.sidechain_committee_id);
        append_signature_list(data, tx.sidechain_committee_signatures);
        if (tx.type == TransactionType::SLASH_DEPOSIT) {
            utils::append_hash256(data, tx.sidechain_fraud_checkpoint_a);
            utils::append_hash256(data, tx.sidechain_fraud_checkpoint_b);
            data.push_back(static_cast<byte>(tx.sidechain_fraud_checkpoint_index_a & 0xFFu));
            data.push_back(static_cast<byte>((tx.sidechain_fraud_checkpoint_index_a >> 8) & 0xFFu));
            data.push_back(static_cast<byte>(tx.sidechain_fraud_checkpoint_index_b & 0xFFu));
            data.push_back(static_cast<byte>((tx.sidechain_fraud_checkpoint_index_b >> 8) & 0xFFu));
        }
    } else if (tx.type == TransactionType::SIDECHAIN_CHECKPOINT) {
        utils::append_u64_le(data, static_cast<uint64_t>(tx.sidechain_checkpoint_height));
        utils::append_hash256(data, tx.sidechain_committee_id);
        append_signature_list(data, tx.sidechain_committee_signatures);
    } else if (tx.type == TransactionType::UPDATE_SIDECHAIN_COMMITTEE) {
        append_committee_config(
            data,
            tx.sidechain_committee_m,
            tx.sidechain_committee_n,
            tx.sidechain_committee_members
        );
        utils::append_hash256(data, tx.sidechain_committee_id);
        append_signature_list(data, tx.sidechain_committee_signatures);
    } else if (tx.type == TransactionType::DECOMMISSION_SIDECHAIN) {
        append_len_prefixed_bytes(data, tx.sidechain_signature);
        utils::append_hash256(data, tx.sidechain_committee_id);
        append_signature_list(data, tx.sidechain_committee_signatures);
    } else {
        append_len_prefixed_bytes(data, tx.sidechain_signature);
    }
}

void read_sidechain_payload(Transaction& tx, const bytes& data, size_t& offset) {
    auto require = [&](uint64_t len) { require_remaining(data, offset, len); };
    auto read_vi = [&]() { return read_varint(data, offset); };

    if (tx.type == TransactionType::BATCH_SIDECHAIN_CHECKPOINT) {
        uint64_t count = read_vi();
        if (count > consensus::params::MAX_BATCH_SIDECHAIN_CHECKPOINTS) {
            throw std::runtime_error("Invalid transaction data: batch too large");
        }
        tx.sidechain_batch_checkpoints.clear();
        tx.sidechain_batch_checkpoints.reserve(static_cast<size_t>(count));
        for (uint64_t i = 0; i < count; i++) {
            BatchCheckpointItem item{};
            if (offset + 32 > data.size()) throw std::runtime_error("Invalid transaction data");
            std::copy(data.begin() + offset, data.begin() + offset + 32, item.sidechain_id.begin());
            offset += 32;
            if (offset + 32 > data.size()) throw std::runtime_error("Invalid transaction data");
            std::copy(data.begin() + offset, data.begin() + offset + 32, item.checkpoint.begin());
            offset += 32;
            if (offset + 8 > data.size()) throw std::runtime_error("Invalid transaction data");
            item.checkpoint_height = 0;
            for (int j = 0; j < 8; j++) {
                item.checkpoint_height |= (uint64_t)data[offset++] << (j * 8);
            }
            if (offset + 8 > data.size()) throw std::runtime_error("Invalid transaction data");
            item.fee = 0;
            for (int j = 0; j < 8; j++) {
                item.fee |= (uint64_t)data[offset++] << (j * 8);
            }
            if (offset + 1 > data.size()) throw std::runtime_error("Invalid transaction data");
            item.version = data[offset++];
            if (offset + 32 > data.size()) throw std::runtime_error("Invalid transaction data");
            std::copy(data.begin() + offset, data.begin() + offset + 32, item.committee_id.begin());
            offset += 32;
            uint64_t sig_count = read_vi();
            if (sig_count > consensus::params::MAX_SIDECHAIN_COMMITTEE_SIGNATURES) {
                throw std::runtime_error("Invalid transaction data: too many committee signatures");
            }
            item.committee_signatures.clear();
            item.committee_signatures.reserve(static_cast<size_t>(sig_count));
            for (uint64_t j = 0; j < sig_count; j++) {
                uint64_t sig_len = read_vi();
                if (sig_len > consensus::params::MAX_SIGNATURE_SIZE) {
                    throw std::runtime_error("Invalid transaction data: committee signature too large");
                }
                require(sig_len);
                Signature sig;
                sig.resize(sig_len);
                if (sig_len > 0) {
                    std::copy(data.begin() + offset, data.begin() + offset + sig_len, sig.begin());
                    offset += sig_len;
                }
                item.committee_signatures.push_back(std::move(sig));
            }
            uint64_t proof_len = read_vi();
            if (proof_len > consensus::params::MAX_BATCH_CHECKPOINT_PROOF) {
                throw std::runtime_error("Invalid transaction data: proof too large");
            }
            require(proof_len);
            if (proof_len > 0) {
                item.proof.assign(data.begin() + offset, data.begin() + offset + proof_len);
                offset += proof_len;
            }
            tx.sidechain_batch_checkpoints.push_back(std::move(item));
        }
        return;
    }

    if (offset + 32 > data.size()) throw std::runtime_error("Invalid transaction data");
    std::copy(data.begin() + offset, data.begin() + offset + 32, tx.sidechain_id.begin());
    offset += 32;

    uint64_t owner_len = read_vi();
    if (owner_len > consensus::params::MAX_ADDRESS_SIZE) {
        throw std::runtime_error("Invalid transaction data: sidechain owner too long");
    }
    require(owner_len);
    if (owner_len > 0) {
        std::string owner_str(data.begin() + offset, data.begin() + offset + owner_len);
        tx.sidechain_owner = Address(owner_str);
        offset += owner_len;
    }

    if (offset + 8 > data.size()) throw std::runtime_error("Invalid transaction data");
    tx.sidechain_amount = 0;
    for (int i = 0; i < 8; i++) {
        tx.sidechain_amount |= (uint64_t)data[offset++] << (i * 8);
    }

    if (offset + 8 > data.size()) throw std::runtime_error("Invalid transaction data");
    tx.sidechain_unlock_height = 0;
    for (int i = 0; i < 8; i++) {
        tx.sidechain_unlock_height |= (uint64_t)data[offset++] << (i * 8);
    }

    if (offset + 32 > data.size()) throw std::runtime_error("Invalid transaction data");
    std::copy(data.begin() + offset, data.begin() + offset + 32, tx.sidechain_checkpoint.begin());
    offset += 32;

    if (tx.type == TransactionType::ISSUE_SIDECHAIN) {
        uint64_t sym_len = read_vi();
        if (sym_len > consensus::params::MAX_SIDECHAIN_TOKEN_SYMBOL_SIZE) {
            throw std::runtime_error("Invalid transaction data: token symbol too long");
        }
        require(sym_len);
        if (sym_len > 0) {
            tx.sidechain_token_symbol.assign(data.begin() + offset, data.begin() + offset + sym_len);
            offset += sym_len;
        }

        if (offset + 4 > data.size()) throw std::runtime_error("Invalid transaction data");
        tx.sidechain_multiplier = 0;
        for (int i = 0; i < 4; i++) {
            tx.sidechain_multiplier |= (uint32_t)data[offset++] << (i * 8);
        }

        if (offset + 4 > data.size()) throw std::runtime_error("Invalid transaction data");
        tx.sidechain_fee_bps = 0;
        for (int i = 0; i < 4; i++) {
            tx.sidechain_fee_bps |= (uint32_t)data[offset++] << (i * 8);
        }

        if (offset + 4 > data.size()) throw std::runtime_error("Invalid transaction data");
        tx.sidechain_max_multiplier = 0;
        for (int i = 0; i < 4; i++) {
            tx.sidechain_max_multiplier |= (uint32_t)data[offset++] << (i * 8);
        }
        if (offset + 2 > data.size()) throw std::runtime_error("Invalid transaction data");
        tx.sidechain_checkpoint_policy_id = data[offset++];
        tx.sidechain_checkpoint_version = data[offset++];

        if (offset + 2 > data.size()) throw std::runtime_error("Invalid transaction data");
        tx.sidechain_committee_m = data[offset++];
        tx.sidechain_committee_n = data[offset++];

        uint64_t count = read_vi();
        if (count > consensus::params::MAX_SIDECHAIN_COMMITTEE_MEMBERS) {
            throw std::runtime_error("Invalid transaction data: committee too large");
        }
        tx.sidechain_committee_members.clear();
        tx.sidechain_committee_members.reserve(static_cast<size_t>(count));
        for (uint64_t i = 0; i < count; i++) {
            uint64_t len = read_vi();
            if (len > consensus::params::MAX_ADDRESS_SIZE) {
                throw std::runtime_error("Invalid transaction data: committee address too long");
            }
            require(len);
            std::string s(data.begin() + offset, data.begin() + offset + len);
            offset += len;
            tx.sidechain_committee_members.emplace_back(Address(s));
        }
    } else if (tx.type == TransactionType::REGISTER_ACCOUNTING_SIDECHAIN) {
        uint64_t namespace_len = read_vi();
        if (namespace_len > consensus::params::MAX_SIDECHAIN_NAMESPACE) {
            throw std::runtime_error("Invalid transaction data: sidechain namespace too long");
        }
        require(namespace_len);
        tx.sidechain_namespace.assign(
            data.begin() + offset,
            data.begin() + offset + namespace_len
        );
        offset += namespace_len;

        require(32);
        std::copy(
            data.begin() + offset,
            data.begin() + offset + 32,
            tx.sidechain_schema_hash.begin()
        );
        offset += 32;

        require(4);
        tx.sidechain_checkpoint_policy_id = data[offset++];
        tx.sidechain_checkpoint_version = data[offset++];
        tx.sidechain_committee_m = data[offset++];
        tx.sidechain_committee_n = data[offset++];

        uint64_t count = read_vi();
        if (count > consensus::params::MAX_SIDECHAIN_COMMITTEE_MEMBERS) {
            throw std::runtime_error("Invalid transaction data: committee too large");
        }
        tx.sidechain_committee_members.clear();
        tx.sidechain_committee_members.reserve(static_cast<size_t>(count));
        for (uint64_t i = 0; i < count; i++) {
            uint64_t len = read_vi();
            if (len > consensus::params::MAX_ADDRESS_SIZE) {
                throw std::runtime_error("Invalid transaction data: committee address too long");
            }
            require(len);
            std::string s(data.begin() + offset, data.begin() + offset + len);
            offset += len;
            tx.sidechain_committee_members.emplace_back(Address(s));
        }
    }

    if (tx.type == TransactionType::ISSUE_SIDECHAIN ||
        tx.type == TransactionType::REGISTER_ACCOUNTING_SIDECHAIN) {
        if (offset + 32 > data.size()) throw std::runtime_error("Invalid transaction data");
        std::copy(
            data.begin() + offset,
            data.begin() + offset + 32,
            tx.sidechain_committee_id.begin()
        );
        offset += 32;
    }

    if (tx.type == TransactionType::MINT_SIDECHAIN ||
        tx.type == TransactionType::REDEEM_SIDECHAIN ||
        tx.type == TransactionType::SLASH_DEPOSIT) {
        if (offset + 32 > data.size()) throw std::runtime_error("Invalid transaction data");
        std::copy(data.begin() + offset, data.begin() + offset + 32, tx.sidechain_committee_id.begin());
        offset += 32;

        uint64_t sig_count = read_vi();
        if (sig_count > consensus::params::MAX_SIDECHAIN_COMMITTEE_SIGNATURES) {
            throw std::runtime_error("Invalid transaction data: too many committee signatures");
        }
        tx.sidechain_committee_signatures.clear();
        tx.sidechain_committee_signatures.reserve(static_cast<size_t>(sig_count));
        for (uint64_t i = 0; i < sig_count; i++) {
            uint64_t sig_len = read_vi();
            if (sig_len > consensus::params::MAX_SIGNATURE_SIZE) {
                throw std::runtime_error("Invalid transaction data: committee signature too large");
            }
            require(sig_len);
            Signature sig;
            sig.resize(sig_len);
            if (sig_len > 0) {
                std::copy(data.begin() + offset, data.begin() + offset + sig_len, sig.begin());
                offset += sig_len;
            }
            tx.sidechain_committee_signatures.push_back(std::move(sig));
        }
        if (tx.type == TransactionType::SLASH_DEPOSIT) {
            if (offset + 32 > data.size()) throw std::runtime_error("Invalid transaction data");
            std::copy(data.begin() + offset, data.begin() + offset + 32, tx.sidechain_fraud_checkpoint_a.begin());
            offset += 32;
            if (offset + 32 > data.size()) throw std::runtime_error("Invalid transaction data");
            std::copy(data.begin() + offset, data.begin() + offset + 32, tx.sidechain_fraud_checkpoint_b.begin());
            offset += 32;
            if (offset + 4 <= data.size()) {
                tx.sidechain_fraud_checkpoint_index_a =
                    static_cast<uint16_t>(data[offset]) |
                    (static_cast<uint16_t>(data[offset + 1]) << 8);
                offset += 2;
                tx.sidechain_fraud_checkpoint_index_b =
                    static_cast<uint16_t>(data[offset]) |
                    (static_cast<uint16_t>(data[offset + 1]) << 8);
                offset += 2;
            } else {
                tx.sidechain_fraud_checkpoint_index_a = 0xFFFF;
                tx.sidechain_fraud_checkpoint_index_b = 0xFFFF;
            }
        }
    } else if (tx.type == TransactionType::SIDECHAIN_CHECKPOINT) {
        if (offset + 8 > data.size()) throw std::runtime_error("Invalid transaction data");
        tx.sidechain_checkpoint_height = 0;
        for (int i = 0; i < 8; i++) {
            tx.sidechain_checkpoint_height |= (uint64_t)data[offset++] << (i * 8);
        }

        if (offset + 32 > data.size()) throw std::runtime_error("Invalid transaction data");
        std::copy(data.begin() + offset, data.begin() + offset + 32, tx.sidechain_committee_id.begin());
        offset += 32;

        uint64_t sig_count = read_vi();
        if (sig_count > consensus::params::MAX_SIDECHAIN_COMMITTEE_SIGNATURES) {
            throw std::runtime_error("Invalid transaction data: too many committee signatures");
        }
        tx.sidechain_committee_signatures.clear();
        tx.sidechain_committee_signatures.reserve(static_cast<size_t>(sig_count));
        for (uint64_t i = 0; i < sig_count; i++) {
            uint64_t sig_len = read_vi();
            if (sig_len > consensus::params::MAX_SIGNATURE_SIZE) {
                throw std::runtime_error("Invalid transaction data: committee signature too large");
            }
            require(sig_len);
            Signature sig;
            sig.resize(sig_len);
            if (sig_len > 0) {
                std::copy(data.begin() + offset, data.begin() + offset + sig_len, sig.begin());
                offset += sig_len;
            }
            tx.sidechain_committee_signatures.push_back(std::move(sig));
        }
    } else if (tx.type == TransactionType::UPDATE_SIDECHAIN_COMMITTEE) {
        if (offset + 2 > data.size()) throw std::runtime_error("Invalid transaction data");
        tx.sidechain_committee_m = data[offset++];
        tx.sidechain_committee_n = data[offset++];

        uint64_t count = read_vi();
        if (count > consensus::params::MAX_SIDECHAIN_COMMITTEE_MEMBERS) {
            throw std::runtime_error("Invalid transaction data: committee too large");
        }
        tx.sidechain_committee_members.clear();
        tx.sidechain_committee_members.reserve(static_cast<size_t>(count));
        for (uint64_t i = 0; i < count; i++) {
            uint64_t len = read_vi();
            if (len > consensus::params::MAX_ADDRESS_SIZE) {
                throw std::runtime_error("Invalid transaction data: committee address too long");
            }
            require(len);
            std::string s(data.begin() + offset, data.begin() + offset + len);
            offset += len;
            tx.sidechain_committee_members.emplace_back(Address(s));
        }

        if (offset + 32 > data.size()) throw std::runtime_error("Invalid transaction data");
        std::copy(data.begin() + offset, data.begin() + offset + 32, tx.sidechain_committee_id.begin());
        offset += 32;

        uint64_t sig_count = read_vi();
        if (sig_count > consensus::params::MAX_SIDECHAIN_COMMITTEE_SIGNATURES) {
            throw std::runtime_error("Invalid transaction data: too many committee signatures");
        }
        tx.sidechain_committee_signatures.clear();
        tx.sidechain_committee_signatures.reserve(static_cast<size_t>(sig_count));
        for (uint64_t i = 0; i < sig_count; i++) {
            uint64_t sig_len = read_vi();
            if (sig_len > consensus::params::MAX_SIGNATURE_SIZE) {
                throw std::runtime_error("Invalid transaction data: committee signature too large");
            }
            require(sig_len);
            Signature sig;
            sig.resize(sig_len);
            if (sig_len > 0) {
                std::copy(data.begin() + offset, data.begin() + offset + sig_len, sig.begin());
                offset += sig_len;
            }
            tx.sidechain_committee_signatures.push_back(std::move(sig));
        }
    } else if (tx.type == TransactionType::DECOMMISSION_SIDECHAIN) {
        uint64_t sig_len = read_vi();
        if (sig_len > consensus::params::MAX_SIGNATURE_SIZE) {
            throw std::runtime_error("Invalid transaction data: sidechain signature too large");
        }
        require(sig_len);
        tx.sidechain_signature.resize(sig_len);
        if (sig_len > 0) {
            std::copy(data.begin() + offset, data.begin() + offset + sig_len, tx.sidechain_signature.begin());
            offset += sig_len;
        }

        if (offset + 32 > data.size()) throw std::runtime_error("Invalid transaction data");
        std::copy(data.begin() + offset, data.begin() + offset + 32, tx.sidechain_committee_id.begin());
        offset += 32;

        uint64_t sig_count = read_vi();
        if (sig_count > consensus::params::MAX_SIDECHAIN_COMMITTEE_SIGNATURES) {
            throw std::runtime_error("Invalid transaction data: too many committee signatures");
        }
        tx.sidechain_committee_signatures.clear();
        tx.sidechain_committee_signatures.reserve(static_cast<size_t>(sig_count));
        for (uint64_t i = 0; i < sig_count; i++) {
            uint64_t committee_sig_len = read_vi();
            if (committee_sig_len > consensus::params::MAX_SIGNATURE_SIZE) {
                throw std::runtime_error("Invalid transaction data: committee signature too large");
            }
            require(committee_sig_len);
            Signature sig;
            sig.resize(committee_sig_len);
            if (committee_sig_len > 0) {
                std::copy(
                    data.begin() + offset,
                    data.begin() + offset + committee_sig_len,
                    sig.begin()
                );
                offset += committee_sig_len;
            }
            tx.sidechain_committee_signatures.push_back(std::move(sig));
        }
    } else {
        uint64_t sig_len = read_vi();
        if (sig_len > consensus::params::MAX_SIGNATURE_SIZE) {
            throw std::runtime_error("Invalid transaction data: sidechain signature too large");
        }
        require(sig_len);
        tx.sidechain_signature.resize(sig_len);
        if (sig_len > 0) {
            std::copy(data.begin() + offset, data.begin() + offset + sig_len, tx.sidechain_signature.begin());
            offset += sig_len;
        }
    }
}

} // namespace geocoin::core::tx_codec

namespace geocoin::core {
namespace {

bool verify_sidechain_signature(
    const bytes& unsigned_payload,
    const Signature& signature,
    const Address& expected_signer
) {
    Hash256 msg32 = crypto::sha256(unsigned_payload);

    Address recovered;
    if (!crypto::recover_signer_address_from_signed_message(msg32, signature, recovered)) {
        return false;
    }

    if (expected_signer.str().empty()) {
        return false;
    }
    return recovered.str() == expected_signer.str();
}

} // namespace

bool verify_sidechain_owner_transaction(const Transaction& tx) {
    const bytes unsigned_payload = build_sidechain_payload_for_signing(tx);
    if (!verify_sidechain_signature(unsigned_payload, tx.sidechain_signature, tx.sidechain_owner)) {
        return false;
    }
    if (tx.type == TransactionType::LOCK_DEPOSIT ||
        tx.type == TransactionType::TOP_UP_SIDECHAIN_DEPOSIT) {
        return verify_all_inputs(tx);
    }
    return true;
}

} // namespace geocoin::core
