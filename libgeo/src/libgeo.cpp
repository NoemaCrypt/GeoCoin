#include "libgeo/libgeo.h"

#include "geocoin/crypto/addresses.hpp"
#include "geocoin/crypto/geo_area.hpp"
#include "geocoin/crypto/signatures.hpp"
#include "geocoin/core/transaction.hpp"
#include "geocoin/wallet/multisig.hpp"

#include <cstring>
#include <exception>
#include <string>
#include <vector>

struct geo_tx_t {
    geocoin::core::Transaction tx;
};

namespace {
thread_local std::string g_last_error;

geo_status_t set_error(geo_status_t code, const char* msg) {
    g_last_error = (msg != nullptr) ? msg : "";
    return code;
}

geo_status_t write_address(const geocoin::Address& address, char* out, size_t out_len) {
    if (out == nullptr || out_len == 0) {
        return set_error(GEO_ERR_INVALID_ARG, "address_out is null or empty");
    }

    const std::string& s = address.str();
    if (s.size() + 1 > out_len) {
        return set_error(GEO_ERR_BUFFER_TOO_SMALL, "address_out buffer too small");
    }

    std::memcpy(out, s.c_str(), s.size() + 1);
    return GEO_OK;
}

bool load_hash32(const uint8_t* in, geocoin::Hash256& out) {
    if (in == nullptr) {
        return false;
    }
    std::memcpy(out.data(), in, 32);
    return true;
}

}  // namespace

extern "C" {

uint32_t geo_api_version(void) {
    return LIBGEO_API_VERSION;
}

const char* geo_last_error(void) {
    return g_last_error.c_str();
}

geo_status_t geo_verify_address(const char* address, int* is_valid) {
    try {
        g_last_error.clear();
        if (address == nullptr || is_valid == nullptr) {
            return set_error(GEO_ERR_INVALID_ARG, "address/is_valid is null");
        }

        const geocoin::Address addr(address);
        *is_valid = geocoin::crypto::verify_address(addr) ? 1 : 0;
        return GEO_OK;
    } catch (const std::exception& e) {
        return set_error(GEO_ERR_INTERNAL, e.what());
    } catch (...) {
        return set_error(GEO_ERR_INTERNAL, "unknown error");
    }
}

geo_status_t geo_derive_address_from_private_key(
    const uint8_t* private_key,
    size_t private_key_len,
    char* address_out,
    size_t address_out_len
) {
    try {
        g_last_error.clear();
        if (private_key == nullptr || private_key_len != 32) {
            return set_error(GEO_ERR_INVALID_ARG, "private_key must be 32 bytes");
        }

        geocoin::bytes sk(private_key, private_key + private_key_len);
        geocoin::Address address;
        if (!geocoin::crypto::derive_identity_address_from_private_key(sk, address)) {
            return set_error(GEO_ERR_CRYPTO, "derive_identity_address_from_private_key failed");
        }

        return write_address(address, address_out, address_out_len);
    } catch (const std::exception& e) {
        return set_error(GEO_ERR_INTERNAL, e.what());
    } catch (...) {
        return set_error(GEO_ERR_INTERNAL, "unknown error");
    }
}

geo_status_t geo_sign_digest32_recoverable(
    const uint8_t msg32[32],
    const uint8_t* private_key,
    size_t private_key_len,
    uint8_t* signature_out,
    size_t signature_out_len,
    size_t* signature_written
) {
    try {
        g_last_error.clear();
        if (msg32 == nullptr || private_key == nullptr || signature_out == nullptr || signature_written == nullptr) {
            return set_error(GEO_ERR_INVALID_ARG, "null input/output pointer");
        }
        if (private_key_len != 32) {
            return set_error(GEO_ERR_INVALID_ARG, "private_key must be 32 bytes");
        }

        geocoin::Hash256 msg_hash;
        if (!load_hash32(msg32, msg_hash)) {
            return set_error(GEO_ERR_INVALID_ARG, "msg32 is null");
        }

        geocoin::bytes sk(private_key, private_key + private_key_len);
        const geocoin::Signature sig = geocoin::crypto::sign_recoverable_message(msg_hash, sk);

        if (sig.empty()) {
            return set_error(GEO_ERR_CRYPTO, "sign_recoverable_message failed");
        }
        if (sig.size() > signature_out_len) {
            return set_error(GEO_ERR_BUFFER_TOO_SMALL, "signature_out buffer too small");
        }

        std::memcpy(signature_out, sig.data(), sig.size());
        *signature_written = sig.size();
        return GEO_OK;
    } catch (const std::exception& e) {
        return set_error(GEO_ERR_INTERNAL, e.what());
    } catch (...) {
        return set_error(GEO_ERR_INTERNAL, "unknown error");
    }
}

geo_status_t geo_verify_signed_message(
    const uint8_t msg32[32],
    const uint8_t* signature,
    size_t signature_len,
    int* is_valid
) {
    try {
        g_last_error.clear();
        if (msg32 == nullptr || signature == nullptr || is_valid == nullptr) {
            return set_error(GEO_ERR_INVALID_ARG, "msg32/signature/is_valid is null");
        }

        geocoin::Hash256 msg_hash;
        if (!load_hash32(msg32, msg_hash)) {
            return set_error(GEO_ERR_INVALID_ARG, "msg32 is null");
        }

        geocoin::Signature sig(signature, signature + signature_len);
        *is_valid = geocoin::crypto::verify_signed_message(msg_hash, sig) ? 1 : 0;
        return GEO_OK;
    } catch (const std::exception& e) {
        return set_error(GEO_ERR_INTERNAL, e.what());
    } catch (...) {
        return set_error(GEO_ERR_INTERNAL, "unknown error");
    }
}

geo_status_t geo_recover_signer_address(
    const uint8_t msg32[32],
    const uint8_t* signature,
    size_t signature_len,
    char* address_out,
    size_t address_out_len
) {
    try {
        g_last_error.clear();
        if (msg32 == nullptr || signature == nullptr || address_out == nullptr) {
            return set_error(GEO_ERR_INVALID_ARG, "msg32/signature/address_out is null");
        }

        geocoin::Hash256 msg_hash;
        if (!load_hash32(msg32, msg_hash)) {
            return set_error(GEO_ERR_INVALID_ARG, "msg32 is null");
        }

        geocoin::Signature sig(signature, signature + signature_len);
        geocoin::Address signer;
        if (!geocoin::crypto::recover_signer_address_from_signed_message(msg_hash, sig, signer)) {
            return set_error(GEO_ERR_CRYPTO, "recover_signer_address_from_signed_message failed");
        }

        return write_address(signer, address_out, address_out_len);
    } catch (const std::exception& e) {
        return set_error(GEO_ERR_INTERNAL, e.what());
    } catch (...) {
        return set_error(GEO_ERR_INTERNAL, "unknown error");
    }
}

geo_status_t geo_multisig_create(
    uint8_t m,
    uint8_t n,
    const char* const* participant_addresses,
    char* multisig_address_out,
    size_t multisig_address_out_len
) {
    try {
        g_last_error.clear();
        if (participant_addresses == nullptr || multisig_address_out == nullptr) {
            return set_error(GEO_ERR_INVALID_ARG, "participant_addresses/multisig_address_out is null");
        }

        std::vector<geocoin::Address> participants;
        participants.reserve(n);
        for (uint8_t i = 0; i < n; ++i) {
            const char* p = participant_addresses[i];
            if (p == nullptr) {
                return set_error(GEO_ERR_INVALID_ARG, "participant address is null");
            }
            geocoin::Address addr(p);
            if (!geocoin::crypto::verify_address(addr)) {
                return set_error(GEO_ERR_INVALID_ADDRESS, "participant address is invalid");
            }
            participants.push_back(addr);
        }

        geocoin::wallet::MultisigAddress ms = geocoin::wallet::create_multisig_address(m, n, participants);
        geocoin::Address ms_addr(ms.address);
        return write_address(ms_addr, multisig_address_out, multisig_address_out_len);
    } catch (const std::exception& e) {
        return set_error(GEO_ERR_INTERNAL, e.what());
    } catch (...) {
        return set_error(GEO_ERR_INTERNAL, "unknown error");
    }
}

geo_status_t geo_multisig_verify(
    const char* multisig_address,
    int* is_valid
) {
    try {
        g_last_error.clear();
        if (multisig_address == nullptr || is_valid == nullptr) {
            return set_error(GEO_ERR_INVALID_ARG, "multisig_address/is_valid is null");
        }

        const std::string ms(multisig_address);
        const geocoin::Address addr(ms);
        *is_valid = (geocoin::wallet::is_multisig_address(ms) && geocoin::crypto::verify_address(addr)) ? 1 : 0;
        return GEO_OK;
    } catch (const std::exception& e) {
        return set_error(GEO_ERR_INTERNAL, e.what());
    } catch (...) {
        return set_error(GEO_ERR_INTERNAL, "unknown error");
    }
}

geo_status_t geo_tx_hash_from_raw(
    const uint8_t* raw_tx,
    size_t raw_tx_len,
    uint8_t out_tx_hash32[32]
) {
    try {
        g_last_error.clear();
        if (raw_tx == nullptr || out_tx_hash32 == nullptr) {
            return set_error(GEO_ERR_INVALID_ARG, "raw_tx/out_tx_hash32 is null");
        }

        geocoin::bytes raw(raw_tx, raw_tx + raw_tx_len);
        const geocoin::Hash256 h = geocoin::core::Transaction::canonical_hash_from_serialized(raw);
        std::memcpy(out_tx_hash32, h.data(), h.size());
        return GEO_OK;
    } catch (const std::exception& e) {
        return set_error(GEO_ERR_INTERNAL, e.what());
    } catch (...) {
        return set_error(GEO_ERR_INTERNAL, "unknown error");
    }
}

geo_status_t geo_tx_sign_input(
    const uint8_t tx_hash32[32],
    uint64_t input_index,
    const uint8_t* private_key,
    size_t private_key_len,
    uint8_t* signature_out,
    size_t signature_out_len,
    size_t* signature_written
) {
    try {
        g_last_error.clear();
        if (tx_hash32 == nullptr || private_key == nullptr || signature_out == nullptr || signature_written == nullptr) {
            return set_error(GEO_ERR_INVALID_ARG, "null input/output pointer");
        }
        if (private_key_len != 32) {
            return set_error(GEO_ERR_INVALID_ARG, "private_key must be 32 bytes");
        }

        geocoin::Hash256 tx_hash{};
        std::memcpy(tx_hash.data(), tx_hash32, tx_hash.size());
        geocoin::bytes sk(private_key, private_key + private_key_len);
        const geocoin::Signature sig = geocoin::crypto::sign_transaction_input(
            tx_hash,
            static_cast<size_t>(input_index),
            sk
        );

        if (sig.empty()) {
            return set_error(GEO_ERR_CRYPTO, "sign_transaction_input failed");
        }
        if (sig.size() > signature_out_len) {
            return set_error(GEO_ERR_BUFFER_TOO_SMALL, "signature_out buffer too small");
        }

        std::memcpy(signature_out, sig.data(), sig.size());
        *signature_written = sig.size();
        return GEO_OK;
    } catch (const std::exception& e) {
        return set_error(GEO_ERR_INTERNAL, e.what());
    } catch (...) {
        return set_error(GEO_ERR_INTERNAL, "unknown error");
    }
}

geo_status_t geo_tx_verify_input_signature(
    const uint8_t tx_hash32[32],
    uint64_t input_index,
    const uint8_t* signature,
    size_t signature_len,
    int* is_valid
) {
    try {
        g_last_error.clear();
        if (tx_hash32 == nullptr || signature == nullptr || is_valid == nullptr) {
            return set_error(GEO_ERR_INVALID_ARG, "tx_hash32/signature/is_valid is null");
        }

        geocoin::Hash256 tx_hash{};
        std::memcpy(tx_hash.data(), tx_hash32, tx_hash.size());
        geocoin::Signature sig(signature, signature + signature_len);

        *is_valid = geocoin::crypto::verify_transaction_input(
            tx_hash,
            static_cast<size_t>(input_index),
            sig
        ) ? 1 : 0;
        return GEO_OK;
    } catch (const std::exception& e) {
        return set_error(GEO_ERR_INTERNAL, e.what());
    } catch (...) {
        return set_error(GEO_ERR_INTERNAL, "unknown error");
    }
}

geo_status_t geo_tx_recover_input_signer_address(
    const uint8_t tx_hash32[32],
    uint64_t input_index,
    const uint8_t* signature,
    size_t signature_len,
    char* address_out,
    size_t address_out_len
) {
    try {
        g_last_error.clear();
        if (tx_hash32 == nullptr || signature == nullptr || address_out == nullptr) {
            return set_error(GEO_ERR_INVALID_ARG, "tx_hash32/signature/address_out is null");
        }

        geocoin::Hash256 tx_hash{};
        std::memcpy(tx_hash.data(), tx_hash32, tx_hash.size());
        geocoin::Signature sig(signature, signature + signature_len);
        geocoin::Address signer;
        if (!geocoin::crypto::recover_signer_address_from_transaction_input(
                tx_hash,
                static_cast<size_t>(input_index),
                sig,
                signer)) {
            return set_error(GEO_ERR_CRYPTO, "recover_signer_address_from_transaction_input failed");
        }

        return write_address(signer, address_out, address_out_len);
    } catch (const std::exception& e) {
        return set_error(GEO_ERR_INTERNAL, e.what());
    } catch (...) {
        return set_error(GEO_ERR_INTERNAL, "unknown error");
    }
}

geo_status_t geo_tx_new(geo_tx_t** tx_out) {
    try {
        g_last_error.clear();
        if (tx_out == nullptr) {
            return set_error(GEO_ERR_INVALID_ARG, "tx_out is null");
        }
        *tx_out = new geo_tx_t();
        return GEO_OK;
    } catch (const std::exception& e) {
        return set_error(GEO_ERR_INTERNAL, e.what());
    } catch (...) {
        return set_error(GEO_ERR_INTERNAL, "unknown error");
    }
}

void geo_tx_free(geo_tx_t* tx) {
    delete tx;
}

geo_status_t geo_tx_deserialize(
    const uint8_t* raw_tx,
    size_t raw_tx_len,
    geo_tx_t** tx_out
) {
    try {
        g_last_error.clear();
        if (raw_tx == nullptr || tx_out == nullptr) {
            return set_error(GEO_ERR_INVALID_ARG, "raw_tx/tx_out is null");
        }
        geocoin::bytes raw(raw_tx, raw_tx + raw_tx_len);
        size_t offset = 0;
        auto* out = new geo_tx_t();
        out->tx = geocoin::core::Transaction::deserialize(raw, offset);
        if (offset != raw.size()) {
            delete out;
            return set_error(GEO_ERR_INVALID_ARG, "raw_tx has trailing bytes");
        }
        *tx_out = out;
        return GEO_OK;
    } catch (const std::exception& e) {
        return set_error(GEO_ERR_INVALID_ARG, e.what());
    } catch (...) {
        return set_error(GEO_ERR_INTERNAL, "unknown error");
    }
}

geo_status_t geo_tx_serialize(
    const geo_tx_t* tx,
    uint8_t* out_raw_tx,
    size_t out_raw_tx_len,
    size_t* out_written
) {
    try {
        g_last_error.clear();
        if (tx == nullptr || out_written == nullptr) {
            return set_error(GEO_ERR_INVALID_ARG, "tx/out_written is null");
        }

        geocoin::bytes raw = tx->tx.serialize();
        *out_written = raw.size();

        if (out_raw_tx == nullptr || out_raw_tx_len < raw.size()) {
            return set_error(GEO_ERR_BUFFER_TOO_SMALL, "out_raw_tx buffer too small");
        }

        std::memcpy(out_raw_tx, raw.data(), raw.size());
        return GEO_OK;
    } catch (const std::exception& e) {
        return set_error(GEO_ERR_INTERNAL, e.what());
    } catch (...) {
        return set_error(GEO_ERR_INTERNAL, "unknown error");
    }
}

geo_status_t geo_tx_set_header(
    geo_tx_t* tx,
    uint32_t version,
    uint8_t type,
    uint64_t timestamp,
    uint32_t lock_time
) {
    try {
        g_last_error.clear();
        if (tx == nullptr) {
            return set_error(GEO_ERR_INVALID_ARG, "tx is null");
        }
        tx->tx.version = version;
        tx->tx.type = static_cast<geocoin::core::TransactionType>(type);
        tx->tx.timestamp = timestamp;
        tx->tx.lock_time = lock_time;
        return GEO_OK;
    } catch (const std::exception& e) {
        return set_error(GEO_ERR_INTERNAL, e.what());
    } catch (...) {
        return set_error(GEO_ERR_INTERNAL, "unknown error");
    }
}

geo_status_t geo_tx_add_input(
    geo_tx_t* tx,
    const uint8_t prev_tx_hash32[32],
    uint32_t vout,
    uint32_t sequence
) {
    try {
        g_last_error.clear();
        if (tx == nullptr || prev_tx_hash32 == nullptr) {
            return set_error(GEO_ERR_INVALID_ARG, "tx/prev_tx_hash32 is null");
        }
        geocoin::core::TxInput in;
        std::memcpy(in.prev_tx_hash.data(), prev_tx_hash32, in.prev_tx_hash.size());
        in.vout = vout;
        in.sequence = sequence;
        tx->tx.inputs.push_back(std::move(in));
        return GEO_OK;
    } catch (const std::exception& e) {
        return set_error(GEO_ERR_INTERNAL, e.what());
    } catch (...) {
        return set_error(GEO_ERR_INTERNAL, "unknown error");
    }
}

geo_status_t geo_tx_add_output(
    geo_tx_t* tx,
    int64_t value,
    const char* address
) {
    try {
        g_last_error.clear();
        if (tx == nullptr || address == nullptr) {
            return set_error(GEO_ERR_INVALID_ARG, "tx/address is null");
        }
        geocoin::Address out_addr(address);
        if (!geocoin::crypto::verify_address(out_addr)) {
            return set_error(GEO_ERR_INVALID_ADDRESS, "invalid output address");
        }
        geocoin::core::TxOutput out;
        out.value = static_cast<geocoin::Amount>(value);
        out.address = out_addr;
        tx->tx.outputs.push_back(std::move(out));
        return GEO_OK;
    } catch (const std::exception& e) {
        return set_error(GEO_ERR_INTERNAL, e.what());
    } catch (...) {
        return set_error(GEO_ERR_INTERNAL, "unknown error");
    }
}

geo_status_t geo_tx_set_input_signature(
    geo_tx_t* tx,
    uint64_t input_index,
    const uint8_t* signature,
    size_t signature_len
) {
    try {
        g_last_error.clear();
        if (tx == nullptr || signature == nullptr) {
            return set_error(GEO_ERR_INVALID_ARG, "tx/signature is null");
        }
        if (input_index >= tx->tx.inputs.size()) {
            return set_error(GEO_ERR_INVALID_ARG, "input_index out of range");
        }
        tx->tx.inputs[static_cast<size_t>(input_index)].signature.assign(
            signature,
            signature + signature_len
        );
        return GEO_OK;
    } catch (const std::exception& e) {
        return set_error(GEO_ERR_INTERNAL, e.what());
    } catch (...) {
        return set_error(GEO_ERR_INTERNAL, "unknown error");
    }
}

geo_status_t geo_tx_sign_input_inplace(
    geo_tx_t* tx,
    uint64_t input_index,
    const uint8_t* private_key,
    size_t private_key_len
) {
    try {
        g_last_error.clear();
        if (tx == nullptr || private_key == nullptr) {
            return set_error(GEO_ERR_INVALID_ARG, "tx/private_key is null");
        }
        if (private_key_len != 32) {
            return set_error(GEO_ERR_INVALID_ARG, "private_key must be 32 bytes");
        }
        if (input_index >= tx->tx.inputs.size()) {
            return set_error(GEO_ERR_INVALID_ARG, "input_index out of range");
        }
        geocoin::bytes sk(private_key, private_key + private_key_len);
        tx->tx.sign_input(static_cast<size_t>(input_index), sk);
        return GEO_OK;
    } catch (const std::exception& e) {
        return set_error(GEO_ERR_INTERNAL, e.what());
    } catch (...) {
        return set_error(GEO_ERR_INTERNAL, "unknown error");
    }
}

geo_status_t geo_tx_hash(
    const geo_tx_t* tx,
    uint8_t out_tx_hash32[32]
) {
    try {
        g_last_error.clear();
        if (tx == nullptr || out_tx_hash32 == nullptr) {
            return set_error(GEO_ERR_INVALID_ARG, "tx/out_tx_hash32 is null");
        }
        const geocoin::Hash256 h = tx->tx.get_hash();
        std::memcpy(out_tx_hash32, h.data(), h.size());
        return GEO_OK;
    } catch (const std::exception& e) {
        return set_error(GEO_ERR_INTERNAL, e.what());
    } catch (...) {
        return set_error(GEO_ERR_INTERNAL, "unknown error");
    }
}

geo_status_t geo_tx_verify(
    const geo_tx_t* tx,
    int* is_valid
) {
    try {
        g_last_error.clear();
        if (tx == nullptr || is_valid == nullptr) {
            return set_error(GEO_ERR_INVALID_ARG, "tx/is_valid is null");
        }
        *is_valid = tx->tx.verify() ? 1 : 0;
        return GEO_OK;
    } catch (const std::exception& e) {
        return set_error(GEO_ERR_INTERNAL, e.what());
    } catch (...) {
        return set_error(GEO_ERR_INTERNAL, "unknown error");
    }
}

geo_status_t geo_tx_verify_input(
    const geo_tx_t* tx,
    uint64_t input_index,
    int* is_valid
) {
    try {
        g_last_error.clear();
        if (tx == nullptr || is_valid == nullptr) {
            return set_error(GEO_ERR_INVALID_ARG, "tx/is_valid is null");
        }
        if (input_index >= tx->tx.inputs.size()) {
            return set_error(GEO_ERR_INVALID_ARG, "input_index out of range");
        }
        *is_valid = tx->tx.verify_input(static_cast<size_t>(input_index)) ? 1 : 0;
        return GEO_OK;
    } catch (const std::exception& e) {
        return set_error(GEO_ERR_INTERNAL, e.what());
    } catch (...) {
        return set_error(GEO_ERR_INTERNAL, "unknown error");
    }
}

}  // extern "C"
