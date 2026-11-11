// Copyright (c) 2026 The GeoCoin developers
// Distributed under the MIT software license

#include "geocoin/consensus/consensus.hpp"
#include "geocoin/consensus/chainparams.hpp"
#include "geocoin/core/transaction.hpp"
#include "geocoin/crypto/addresses.hpp"
#include "geocoin/crypto/geo_area.hpp"
#include "geocoin/types.hpp"
#include "geocoin/utils/bytes.hpp"
#include "geocoin/wallet/multisig.hpp"
#include "geocoin/wallet/multisig_service.hpp"
#include "nlohmann_json.hpp"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using json = nlohmann::json;

constexpr uint8_t kDefaultM = 2;
constexpr uint8_t kDefaultN = 3;
constexpr uint8_t kDefaultFirstSeed = 101;

[[noreturn]] void usage(const char* argv0)
{
    std::cerr
        << "Usage:\n"
        << "  " << argv0 << " describe [--m 2 --n 3] [--first-seed 101] [--network degree]\n"
        << "  " << argv0 << " build --utxos <file> --recipient <GEO...> "
        << "[--amount-angles N] [--fee-angles N] [--m 2 --n 3] [--first-seed 101] "
        << "[--network degree|degree-test|degree-load]\n";
    throw std::runtime_error("invalid arguments");
}

geocoin::bytes make_key(uint8_t seed)
{
    geocoin::bytes key(32);
    for (size_t i = 0; i < key.size(); ++i) {
        key[i] = static_cast<uint8_t>(seed + i);
    }
    return key;
}

geocoin::Address address_from_key(const geocoin::bytes& key)
{
    geocoin::Address address;
    if (!geocoin::crypto::compute_address_from_private_key(key, address)) {
        throw std::runtime_error("failed to derive address from private key");
    }
    return address;
}

std::vector<geocoin::bytes> participant_keys(uint8_t n, uint8_t first_seed)
{
    std::vector<geocoin::bytes> keys;
    keys.reserve(n);
    for (uint8_t i = 0; i < n; ++i) {
        keys.push_back(make_key(static_cast<uint8_t>(first_seed + i)));
    }
    return keys;
}

std::vector<geocoin::Address> participant_addresses(const std::vector<geocoin::bytes>& keys)
{
    std::vector<geocoin::Address> addresses;
    addresses.reserve(keys.size());
    for (const auto& key : keys) {
        addresses.push_back(address_from_key(key));
    }
    return addresses;
}

json config_json(uint8_t m, uint8_t n, uint8_t first_seed)
{
    const auto keys = participant_keys(n, first_seed);
    const auto addresses = participant_addresses(keys);
    const auto multisig = geocoin::wallet::create_multisig_address(m, n, addresses);

    json participants = json::array();
    for (size_t i = 0; i < addresses.size(); ++i) {
        participants.push_back({
            {"address", addresses[i].str()},
            {"private_key_hex", geocoin::utils::bytes_to_hex(keys[i])}
        });
    }

    return {
        {"m", m},
        {"n", n},
        {"multisig_address", multisig.address},
        {"participants", participants}
    };
}

std::string required_arg(int argc, char** argv, int& i, const std::string& name)
{
    if (i + 1 >= argc) {
        throw std::runtime_error("missing value for " + name);
    }
    return argv[++i];
}

json read_json_file(const std::string& path)
{
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("failed to open " + path);
    }
    json data;
    input >> data;
    return data;
}

uint64_t parse_u64(const std::string& value, const std::string& name)
{
    size_t parsed = 0;
    const uint64_t out = std::stoull(value, &parsed);
    if (parsed != value.size()) {
        throw std::runtime_error("invalid integer for " + name);
    }
    return out;
}

uint8_t parse_u8(const std::string& value, const std::string& name)
{
    const uint64_t out = parse_u64(value, name);
    if (out > 255) {
        throw std::runtime_error(name + " out of range");
    }
    return static_cast<uint8_t>(out);
}

struct Options {
    std::string mode;
    std::string network = "degree";
    std::string utxos_path;
    std::string recipient;
    uint64_t amount_angles = geocoin::COIN;
    uint64_t fee_angles = geocoin::consensus::params::MIN_TX_FEE;
    uint8_t m = kDefaultM;
    uint8_t n = kDefaultN;
    uint8_t first_seed = kDefaultFirstSeed;
};

Options parse_options(int argc, char** argv)
{
    if (argc < 2) {
        usage(argv[0]);
    }

    Options opts;
    opts.mode = argv[1];
    if (opts.mode != "describe" && opts.mode != "build") {
        usage(argv[0]);
    }

    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--network") {
            opts.network = required_arg(argc, argv, i, arg);
        } else if (arg == "--utxos") {
            opts.utxos_path = required_arg(argc, argv, i, arg);
        } else if (arg == "--recipient") {
            opts.recipient = required_arg(argc, argv, i, arg);
        } else if (arg == "--amount-angles") {
            opts.amount_angles = parse_u64(required_arg(argc, argv, i, arg), arg);
        } else if (arg == "--fee-angles") {
            opts.fee_angles = parse_u64(required_arg(argc, argv, i, arg), arg);
        } else if (arg == "--m") {
            opts.m = parse_u8(required_arg(argc, argv, i, arg), arg);
        } else if (arg == "--n") {
            opts.n = parse_u8(required_arg(argc, argv, i, arg), arg);
        } else if (arg == "--first-seed") {
            opts.first_seed = parse_u8(required_arg(argc, argv, i, arg), arg);
        } else {
            usage(argv[0]);
        }
    }

    if (opts.m == 0 || opts.n == 0 || opts.m > opts.n || opts.n > 15) {
        throw std::runtime_error("invalid m/n");
    }
    if (static_cast<uint16_t>(opts.first_seed) + static_cast<uint16_t>(opts.n) > 256) {
        throw std::runtime_error("first seed plus n exceeds deterministic key byte range");
    }
    if (opts.mode == "build" && (opts.utxos_path.empty() || opts.recipient.empty())) {
        usage(argv[0]);
    }
    return opts;
}

std::vector<geocoin::core::TxInput> select_inputs(
    const json& utxos,
    geocoin::Amount required,
    geocoin::Amount& selected
)
{
    struct Candidate {
        geocoin::Hash256 txid{};
        uint32_t vout = 0;
        geocoin::Amount value = 0;
    };

    std::vector<Candidate> candidates;
    for (const auto& item : utxos) {
        Candidate c;
        if (!geocoin::utils::hex_to_hash256_checked(item.at("txid").get<std::string>(), c.txid)) {
            throw std::runtime_error("invalid utxo txid");
        }
        c.vout = item.at("vout").get<uint32_t>();
        c.value = item.at("value").get<geocoin::Amount>();
        candidates.push_back(c);
    }

    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
        if (a.value != b.value) {
            return a.value < b.value;
        }
        if (a.txid != b.txid) {
            return a.txid < b.txid;
        }
        return a.vout < b.vout;
    });

    std::vector<geocoin::core::TxInput> inputs;
    selected = 0;
    for (const auto& c : candidates) {
        geocoin::core::TxInput input;
        input.prev_tx_hash = c.txid;
        input.vout = c.vout;
        inputs.push_back(input);
        selected += c.value;
        if (selected >= required) {
            break;
        }
    }

    if (selected < required) {
        throw std::runtime_error("insufficient multisig utxo value");
    }
    return inputs;
}

json build_package(const Options& opts)
{
    const auto keys = participant_keys(opts.n, opts.first_seed);
    const auto addresses = participant_addresses(keys);
    const auto multisig = geocoin::wallet::create_multisig_address(opts.m, opts.n, addresses);
    const geocoin::Address multisig_address(multisig.address);
    const geocoin::Address recipient(opts.recipient);

    const geocoin::Amount amount = static_cast<geocoin::Amount>(opts.amount_angles);
    const geocoin::Amount fee = static_cast<geocoin::Amount>(opts.fee_angles);
    const geocoin::Amount required = amount + fee;

    geocoin::Amount selected = 0;
    const std::vector<geocoin::core::TxInput> inputs =
        select_inputs(read_json_file(opts.utxos_path), required, selected);

    std::vector<geocoin::core::TxOutput> outputs;
    outputs.push_back(geocoin::core::TxOutput{amount, recipient, {}});
    const geocoin::Amount change = selected - required;
    if (change > 0) {
        outputs.push_back(geocoin::core::TxOutput{change, multisig_address, {}});
    }

    geocoin::core::Transaction draft =
        geocoin::core::Transaction::create_multisig_spend(inputs, outputs, {}, opts.m, opts.n);
    draft.spend_participants = addresses;
    const geocoin::Hash256 spending_hash = draft.get_hash_without_commitments();

    std::vector<std::string> commitment_hexes;
    std::vector<std::string> commitment_txids;
    std::vector<geocoin::Hash256> commitment_hashes;
    for (uint8_t i = 0; i < opts.m; ++i) {
        geocoin::wallet::CommitmentResult commitment;
        std::string error;
        if (!geocoin::wallet::sign_angle_commitment(multisig_address, spending_hash, keys[i], commitment, error)) {
            throw std::runtime_error("failed to sign angle commitment: " + error);
        }
        commitment_hexes.push_back(geocoin::utils::bytes_to_hex(commitment.commitment_tx.serialize()));
        commitment_txids.push_back(geocoin::utils::hash256_to_hex(commitment.commitment_hash));
        commitment_hashes.push_back(commitment.commitment_hash);
    }

    geocoin::core::Transaction final_tx;
    std::string error;
    if (!geocoin::wallet::finalize_multisig_spend(draft, commitment_hashes, opts.m, final_tx, error)) {
        throw std::runtime_error("failed to finalize multisig spend: " + error);
    }

    json out = config_json(opts.m, opts.n, opts.first_seed);
    out["selected_angles"] = selected;
    out["amount_angles"] = amount;
    out["fee_angles"] = fee;
    out["change_angles"] = change;
    out["spending_tx_hash"] = geocoin::utils::hash256_to_hex(spending_hash);
    out["commitments"] = commitment_hexes;
    out["commitment_txids"] = commitment_txids;
    out["spend"] = geocoin::utils::bytes_to_hex(final_tx.serialize());
    out["spend_txid"] = geocoin::utils::hash256_to_hex(final_tx.get_hash());
    return out;
}

} // namespace

int main(int argc, char** argv)
{
    try {
        const Options opts = parse_options(argc, argv);
        if (!geocoin::consensus::SelectParamsByNetworkId(opts.network)) {
            throw std::runtime_error("unknown network: " + opts.network);
        }
        const json out = opts.mode == "describe" ? config_json(opts.m, opts.n, opts.first_seed) : build_package(opts);
        std::cout << out.dump(2) << "\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "multisig_package_builder: " << ex.what() << "\n";
        return 1;
    }
}
