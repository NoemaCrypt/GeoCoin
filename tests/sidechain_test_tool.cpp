// Copyright (c) 2026 The GeoCoin developers
// Distributed under the MIT software license

#include "geocoin/core/transaction.hpp"
#include "geocoin/crypto/geo_area.hpp"
#include "geocoin/crypto/signatures.hpp"
#include "geocoin/utils/bytes.hpp"
#include "nlohmann_json.hpp"

#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {

using json = nlohmann::json;

[[noreturn]] void usage(const char* argv0)
{
    std::cerr
        << "Usage:\n"
        << "  " << argv0 << " key-info --seed N\n"
        << "  " << argv0 << " sign-payload --private-key-hex HEX --payload-hex HEX\n"
        << "  " << argv0 << " sign-inputs --private-key-hex HEX --tx-hex HEX\n";
    throw std::runtime_error("invalid arguments");
}

std::string required_arg(int argc, char** argv, int& index, const std::string& name)
{
    if (index + 1 >= argc) {
        throw std::runtime_error("missing value for " + name);
    }
    return argv[++index];
}

uint8_t parse_seed(const std::string& value)
{
    size_t parsed = 0;
    const unsigned long raw = std::stoul(value, &parsed);
    if (parsed != value.size() || raw > std::numeric_limits<uint8_t>::max()) {
        throw std::runtime_error("seed must be in range 0..255");
    }
    return static_cast<uint8_t>(raw);
}

geocoin::bytes deterministic_key(uint8_t seed)
{
    geocoin::bytes key(32);
    for (size_t i = 0; i < key.size(); ++i) {
        key[i] = static_cast<geocoin::byte>(seed + static_cast<uint8_t>(i));
    }
    return key;
}

geocoin::bytes parse_private_key(const std::string& value)
{
    geocoin::bytes key;
    if (!geocoin::utils::hex_to_bytes_checked(value, key) || key.size() != 32) {
        throw std::runtime_error("private key must be exactly 32 bytes of hex");
    }
    return key;
}

geocoin::bytes parse_hex(const std::string& value, const std::string& label)
{
    geocoin::bytes out;
    if (!geocoin::utils::hex_to_bytes_checked(value, out)) {
        throw std::runtime_error(label + " must be valid even-length hex");
    }
    return out;
}

geocoin::core::Transaction parse_transaction_exact(const std::string& value)
{
    const geocoin::bytes raw = parse_hex(value, "transaction");
    (void)geocoin::core::Transaction::canonical_hash_from_serialized(raw);
    size_t offset = 0;
    geocoin::core::Transaction tx = geocoin::core::Transaction::deserialize(raw, offset);
    if (offset != raw.size()) {
        throw std::runtime_error("trailing bytes after transaction");
    }
    return tx;
}

json key_info(uint8_t seed)
{
    const geocoin::bytes key = deterministic_key(seed);
    geocoin::Address address;
    if (!geocoin::crypto::compute_address_from_private_key(key, address)) {
        throw std::runtime_error("failed to derive address from deterministic key");
    }
    return {
        {"seed", seed},
        {"private_key_hex", geocoin::utils::bytes_to_hex(key)},
        {"address", address.str()}
    };
}

json sign_payload(const geocoin::bytes& key, const std::string& payload_hex)
{
    const geocoin::bytes payload = parse_hex(payload_hex, "payload");
    const geocoin::Signature signature = geocoin::crypto::sign_recoverable_message(
        geocoin::crypto::sha256(payload),
        key
    );
    geocoin::Address signer;
    if (!geocoin::crypto::recover_signer_address_from_signed_message(
            geocoin::crypto::sha256(payload), signature, signer)) {
        throw std::runtime_error("generated payload signature cannot be recovered");
    }
    return {
        {"signature_hex", geocoin::utils::bytes_to_hex(signature)},
        {"signer", signer.str()}
    };
}

json sign_inputs(const geocoin::bytes& key, const std::string& tx_hex)
{
    geocoin::core::Transaction tx = parse_transaction_exact(tx_hex);
    if (tx.inputs.empty()) {
        throw std::runtime_error("transaction has no inputs to sign");
    }
    tx.sign(key);
    if (!tx.verify()) {
        throw std::runtime_error("transaction verification failed after signing inputs");
    }
    const geocoin::bytes raw = tx.serialize();
    return {
        {"tx_hex", geocoin::utils::bytes_to_hex(raw)},
        {"txid", geocoin::utils::hash256_to_hex(tx.get_hash())},
        {"input_count", tx.inputs.size()}
    };
}

} // namespace

int main(int argc, char** argv)
{
    try {
        if (argc < 2) {
            usage(argv[0]);
        }
        const std::string mode = argv[1];
        std::string private_key_hex;
        std::string payload_hex;
        std::string tx_hex;
        uint8_t seed = 0;
        bool has_seed = false;

        for (int i = 2; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--seed") {
                seed = parse_seed(required_arg(argc, argv, i, arg));
                has_seed = true;
            } else if (arg == "--private-key-hex") {
                private_key_hex = required_arg(argc, argv, i, arg);
            } else if (arg == "--payload-hex") {
                payload_hex = required_arg(argc, argv, i, arg);
            } else if (arg == "--tx-hex") {
                tx_hex = required_arg(argc, argv, i, arg);
            } else {
                usage(argv[0]);
            }
        }

        json result;
        if (mode == "key-info") {
            if (!has_seed || !private_key_hex.empty() || !payload_hex.empty() || !tx_hex.empty()) {
                usage(argv[0]);
            }
            result = key_info(seed);
        } else if (mode == "sign-payload") {
            if (private_key_hex.empty() || payload_hex.empty() || has_seed || !tx_hex.empty()) {
                usage(argv[0]);
            }
            result = sign_payload(parse_private_key(private_key_hex), payload_hex);
        } else if (mode == "sign-inputs") {
            if (private_key_hex.empty() || tx_hex.empty() || has_seed || !payload_hex.empty()) {
                usage(argv[0]);
            }
            result = sign_inputs(parse_private_key(private_key_hex), tx_hex);
        } else {
            usage(argv[0]);
        }

        std::cout << result.dump() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "sidechain_test_tool: " << error.what() << '\n';
        return 1;
    }
}
