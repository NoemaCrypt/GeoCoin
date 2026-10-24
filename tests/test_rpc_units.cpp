#include "geocoin/core/block.hpp"
#include "geocoin/core/blockchain.hpp"
#include "geocoin/core/transaction.hpp"
#include "geocoin/consensus/consensus.hpp"
#include "geocoin/consensus/sidechain_checkpoint.hpp"
#include "geocoin/crypto/addresses.hpp"
#include "geocoin/nft/nft_types.hpp"
#include "geocoin/rpc/rpc_dispatch.hpp"
#include "geocoin/rpc/rpc_encoding.hpp"
#include "geocoin/rpc/rpc_params.hpp"
#include "geocoin/rpc/rpc_server.hpp"
#include "geocoin/rpc/rpc_security.hpp"
#include "geocoin/state/state.hpp"
#include "geocoin/storage/block_storage.hpp"
#include "geocoin/types.hpp"
#include "geocoin/wallet/wallet.hpp"
#include "geocoin/rpc/rpc_amount.hpp"
#include "geocoin/rpc/rpc_blockchain_format.hpp"
#include "geocoin/rpc/rpc_wallet_helpers.hpp"
#include "test_chain_mutation_service.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace geocoin::rpc {

struct RPCServerTestAccess {
    static void configure_credentials(RPCServer& server)
    {
        server.rpc_user_ = "admin";
        server.rpc_password_ = "admin-pass";
        server.miner_rpc_user_ = "miner";
        server.miner_rpc_password_ = "miner-pass";
        server.readonly_rpc_user_ = "readonly";
        server.readonly_rpc_password_ = "readonly-pass";
        server.in_warmup_.store(false, std::memory_order_relaxed);
        server.warmup_status_.clear();
    }

    static void set_warmup(RPCServer& server, bool warmup, std::string status = {})
    {
        server.in_warmup_.store(warmup, std::memory_order_relaxed);
        server.warmup_status_ = std::move(status);
    }

    static HTTPResponse handle_http(RPCServer& server, const HTTPRequest& request)
    {
        return server.handle_http_request(request);
    }

    static void request_shutdown(RPCServer& server)
    {
        server.request_shutdown();
    }

    static json handle_call(RPCServer& server, const json& request, RPCAuthRole role)
    {
        return server.handle_rpc_call(request, role);
    }

    static void seed_rate_limit(RPCServer& server, const std::string& key, uint32_t requests)
    {
        std::lock_guard<std::mutex> lock(server.rpc_rate_limit_mutex_);
        auto& state = server.rpc_rate_limits_[key];
        state.requests = requests;
        state.window_start = std::chrono::steady_clock::now();
    }

    static RPCAuthRole authenticate(RPCServer& server, const HTTPRequest& request)
    {
        return server.authenticate(request);
    }

    static std::string auth_client_key(RPCServer& server, const HTTPRequest& request)
    {
        return server.auth_client_key(request);
    }

    static void record_auth_failure(RPCServer& server, const std::string& key)
    {
        server.record_auth_failure(key);
    }

    static void record_auth_success(RPCServer& server, const std::string& key)
    {
        server.record_auth_success(key);
    }

    static bool is_auth_blocked(RPCServer& server, const std::string& key)
    {
        return server.is_auth_temporarily_blocked(key);
    }

    static void record_auth_failure_rate(RPCServer& server, const std::string& key)
    {
        server.record_auth_failure_rate(key);
    }

    static bool is_auth_failure_rate_limited(RPCServer& server, const std::string& key)
    {
        uint32_t retry_after = 0;
        return server.is_auth_failure_rate_limited(key, retry_after) && retry_after > 0;
    }

    static void expire_auth_block(RPCServer& server, const std::string& key)
    {
        std::lock_guard<std::mutex> lock(server.auth_failures_mutex_);
        auto it = server.auth_failures_.find(key);
        if (it != server.auth_failures_.end()) {
            it->second.blocked_until = std::chrono::steady_clock::now() -
                std::chrono::milliseconds(1);
        }
    }

    static uint32_t auth_failures(RPCServer& server, const std::string& key)
    {
        std::lock_guard<std::mutex> lock(server.auth_failures_mutex_);
        const auto it = server.auth_failures_.find(key);
        return it == server.auth_failures_.end() ? 0 : it->second.failures;
    }

    static size_t auth_failure_count(RPCServer& server)
    {
        std::lock_guard<std::mutex> lock(server.auth_failures_mutex_);
        return server.auth_failures_.size();
    }

    static size_t auth_failure_rate_count(RPCServer& server)
    {
        std::lock_guard<std::mutex> lock(server.auth_failures_mutex_);
        return server.auth_failure_rates_.size();
    }

    static void consume_rate_limit(RPCServer& server, const std::string& key)
    {
        uint32_t retry_after = 0;
        (void)server.consume_rate_limit(key, 1, retry_after);
    }

    static size_t rate_limit_count(RPCServer& server)
    {
        std::lock_guard<std::mutex> lock(server.rpc_rate_limit_mutex_);
        return server.rpc_rate_limits_.size();
    }

    static void require_blockchain(RPCServer& server) { (void)server.require_blockchain(); }
    static void require_wallet(RPCServer& server) { (void)server.require_wallet(); }
    static void require_chain_state(RPCServer& server) { (void)server.require_chain_state(); }
    static void require_mempool(RPCServer& server) { (void)server.require_mempool(); }
    static void require_peer_manager(RPCServer& server) { (void)server.require_peer_manager(); }
    static void require_node_info(RPCServer& server) { (void)server.require_node_info(); }
};

} // namespace geocoin::rpc

namespace geocoin::network {
struct MempoolTestAccess {
    static void seed_entry(
        Mempool& mempool,
        const core::Transaction& tx,
        size_t size,
        uint64_t fee_per_byte
    )
    {
        std::lock_guard<std::mutex> lock(mempool.mutex_);
        MempoolEntry entry;
        entry.tx = tx;
        entry.fee = fee_per_byte * size;
        entry.fee_per_byte = fee_per_byte;
        entry.added_at = std::chrono::steady_clock::now();
        entry.size = size;
        const Hash256 tx_hash = tx.get_hash();
        mempool.transactions_[tx_hash] = entry;
        mempool.fee_index_.insert({fee_per_byte, tx_hash});
        mempool.current_size_ += size;
    }
};
} // namespace geocoin::network

namespace geocoin::state {
struct ChainStateTestAccess {
    static void add_utxo(ChainState& state, const UTXO& utxo)
    {
        state.add_utxo(utxo);
    }
};
} // namespace geocoin::state

namespace {

using geocoin::rpc::RPCErrorCode;
using geocoin::rpc::RPCException;
using geocoin::rpc::json;

void expect(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void expect_rpc_error(RPCErrorCode code, const char* message, const std::function<void()>& fn)
{
    try {
        fn();
    } catch (const RPCException& e) {
        if (e.code() != code) {
            throw std::runtime_error(std::string(message) + ": wrong RPC error code");
        }
        return;
    }
    throw std::runtime_error(message);
}

void expect_response_error(const json& response, RPCErrorCode code, const char* message)
{
    expect(response.is_object(), message);
    expect(response.contains("error") && response["error"].is_object(), message);
    expect(response["error"]["code"].get<int>() == static_cast<int>(code), message);
}

geocoin::bytes make_key(uint8_t seed)
{
    geocoin::bytes key(32);
    for (size_t i = 0; i < key.size(); ++i) {
        key[i] = static_cast<uint8_t>(seed + i);
    }
    return key;
}

geocoin::Address address_from_key(uint8_t seed)
{
    geocoin::Address address;
    expect(geocoin::crypto::compute_address_from_private_key(make_key(seed), address), "address derives");
    return address;
}

geocoin::Hash256 hash_from_seed(uint8_t seed)
{
    geocoin::Hash256 hash{};
    for (size_t i = 0; i < hash.size(); ++i) {
        hash[i] = static_cast<uint8_t>(seed + i * 13);
    }
    return hash;
}

geocoin::core::TxInput input_from_seed(uint8_t seed, uint32_t vout)
{
    geocoin::core::TxInput input;
    input.prev_tx_hash = hash_from_seed(seed);
    input.vout = vout;
    return input;
}

geocoin::core::TxOutput output_to(const geocoin::Address& address, geocoin::Amount value)
{
    geocoin::core::TxOutput output;
    output.value = value;
    output.address = address;
    return output;
}

geocoin::core::Transaction make_signed_payment(
    const std::shared_ptr<geocoin::state::ChainState>& chain_state,
    uint8_t sender_seed,
    uint8_t recipient_seed,
    geocoin::Amount amount,
    geocoin::Amount fee
)
{
    geocoin::wallet::Wallet wallet;
    const geocoin::bytes private_key = make_key(sender_seed);
    expect(wallet.import_private_key(private_key, "sender"), "wallet imports sender key");
    const geocoin::Address sender = address_from_key(sender_seed);

    geocoin::state::UTXO funding{};
    funding.tx_hash = hash_from_seed(static_cast<uint8_t>(sender_seed + 70));
    funding.vout = 0;
    funding.value = amount + fee + geocoin::COIN;
    funding.address = sender;
    funding.height = 0;
    funding.is_coinbase = false;
    geocoin::state::ChainStateTestAccess::add_utxo(*chain_state, funding);

    geocoin::core::Transaction tx;
    expect(
        wallet.create_transaction(address_from_key(recipient_seed), amount, fee, chain_state, tx),
        "wallet creates signed payment for RPC helper"
    );
    return tx;
}

std::string hex_hash(uint8_t seed)
{
    return geocoin::rpc::hash256_to_hex_string(hash_from_seed(seed));
}

void test_amount_helpers()
{
    using namespace geocoin;
    using namespace geocoin::rpc;

    expect(rpc_coins_to_positive_amount(1.5, "coins") == 150000000,
           "coin amount converts to angles");
    expect(rpc_coins_to_positive_amount("0.00000001", "coins") == 1,
           "exact decimal string converts one angle");
    expect(rpc_coins_to_positive_amount(json::parse("1e-8"), "coins") == 1,
           "scientific JSON amount converts exactly");
    expect(rpc_coins_to_positive_amount("1.230000000", "coins") == 123000000,
           "extra zero precision is accepted without rounding");
    expect(rpc_coins_to_positive_amount("92233720368.54775807", "coins") ==
               (std::numeric_limits<Amount>::max)(),
           "maximum representable amount converts exactly");
    expect(rpc_angles_to_positive_amount(42, "angles") == 42, "integer angles convert");
    expect(rpc_required_amount_field(json{{"amount", 123}}, "amount", "amount") == 123,
           "required amount field parses");
    expect(rpc_optional_amount_field(json::object(), "amount", 99, "amount") == 99,
           "optional amount field returns default");
    expect(rpc_optional_amount_field(json{{"amount", nullptr}}, "amount", 99, "amount") == 99,
           "optional null amount returns default");

    expect_rpc_error(RPC_INVALID_PARAMETER, "non-scalar coin amount rejects", [] {
        (void)rpc_coins_to_positive_amount(json::object(), "coins");
    });
    expect_rpc_error(RPC_INVALID_PARAMETER, "zero coin amount rejects", [] {
        (void)rpc_coins_to_positive_amount(0, "coins");
    });
    expect_rpc_error(RPC_INVALID_PARAMETER, "huge coin amount rejects", [] {
        (void)rpc_coins_to_positive_amount(1.0e100, "coins");
    });
    expect_rpc_error(RPC_INVALID_PARAMETER, "sub-angle precision rejects", [] {
        (void)rpc_coins_to_positive_amount("0.000000001", "coins");
    });
    expect_rpc_error(RPC_INVALID_PARAMETER, "fractional angle is not rounded", [] {
        (void)rpc_coins_to_positive_amount("0.000000015", "coins");
    });
    expect_rpc_error(RPC_INVALID_PARAMETER, "amount overflow rejects", [] {
        (void)rpc_coins_to_positive_amount("92233720368.54775808", "coins");
    });
    expect_rpc_error(RPC_INVALID_PARAMETER, "whitespace amount rejects", [] {
        (void)rpc_coins_to_positive_amount(" 1", "coins");
    });
    expect_rpc_error(RPC_INVALID_PARAMETER, "zero angle amount rejects", [] {
        (void)rpc_angles_to_positive_amount(0, "angles");
    });
    expect_rpc_error(RPC_INVALID_PARAMETER, "negative angle amount rejects", [] {
        (void)rpc_angles_to_positive_amount(-1, "angles");
    });
    expect_rpc_error(RPC_INVALID_PARAMETER, "string angle amount rejects", [] {
        (void)rpc_angles_to_positive_amount("42", "angles");
    });

    expect(checked_add_amount_or_throw(10, 20, "add") == 30, "checked add succeeds");
    expect_rpc_error(RPC_INVALID_PARAMETER, "checked add overflow rejects", [] {
        (void)checked_add_amount_or_throw((std::numeric_limits<Amount>::max)(), 1, "add");
    });
    expect_rpc_error(RPC_INVALID_PARAMETER, "checked add underflow rejects", [] {
        (void)checked_add_amount_or_throw((std::numeric_limits<Amount>::min)(), -1, "add");
    });

    expect(saturating_mul_u64(21, 2) == 42, "saturating multiply normal case");
    expect(saturating_mul_u64((std::numeric_limits<uint64_t>::max)(), 2) ==
               (std::numeric_limits<uint64_t>::max)(),
           "saturating multiply clamps overflow");
    expect(mul_div_amount_ceil_clamped(10, 1, 3) == 4, "ceil mul/div rounds up");
    expect(mul_div_amount_ceil_clamped(0, 100, 3) == 0, "non-positive amount returns zero");
    expect(mul_div_amount_ceil_clamped(10, 1, 0) == (std::numeric_limits<Amount>::max)(),
           "zero divisor clamps");
    expect(
        mul_div_amount_ceil_clamped((std::numeric_limits<Amount>::max)(), 2, 1) ==
            (std::numeric_limits<Amount>::max)(),
        "overflowing mul/div clamps"
    );
    expect(calc_sidechain_fee_checked(0, 100) == consensus::params::MIN_TX_FEE,
           "non-positive sidechain fee returns minimum fee");
    expect(calc_sidechain_fee_checked(100000, 10000) == 100000,
           "10000 bps sidechain fee equals amount");
    expect_rpc_error(RPC_INVALID_PARAMETER, "sidechain fee overflow rejects", [] {
        (void)calc_sidechain_fee_checked((std::numeric_limits<Amount>::max)(), (std::numeric_limits<uint32_t>::max)());
    });
}

void test_params_encoding_and_dispatch()
{
    using namespace geocoin;
    using namespace geocoin::rpc;

    json params = json::array({1, nullptr, "x"});
    expect(has_param(params, 0), "present non-null param is detected");
    expect(!has_param(params, 1), "null param is treated as absent");
    expect(!has_param(params, 3), "out of range param is absent");
    require_min_params(params, 1, "min");
    expect(required_param(params, 2, "required") == "x", "required param returns value");
    expect_rpc_error(RPC_INVALID_PARAMS, "missing required param rejects", [&] {
        (void)required_param(params, 4, "required");
    });
    expect_rpc_error(RPC_INVALID_PARAMS, "required object param rejects scalar", [&] {
        (void)required_object_param(params, 0, "object");
    });

    json object = {
        {"s", "value"},
        {"o", json::object({{"x", 1}})},
        {"a", json::array({1, 2})},
        {"u64", 42},
        {"u32", 7},
        {"u8", 3},
    };
    expect(required_string_field(object, "s", "s") == "value", "required string field returns value");
    expect(required_object_field(object, "o", "o").contains("x"), "required object field returns object");
    expect(required_array_field(object, "a", "a").size() == 2, "required array field returns array");
    expect(required_u64_field(object, "u64", "u64") == 42, "required u64 field returns value");
    expect(required_u32_field(object, "u32", "u32") == 7, "required u32 field returns value");
    expect(required_u8_field(object, "u8", "u8") == 3, "required u8 field returns value");
    expect(optional_u64_field(object, "missing", 9, "missing") == 9, "optional u64 default");
    expect(optional_u32_field(object, "missing", 8, "missing") == 8, "optional u32 default");
    expect(optional_u8_field(object, "missing", 7, "missing") == 7, "optional u8 default");
    expect_rpc_error(RPC_INVALID_PARAMETER, "negative u64 rejects", [&] {
        (void)required_u64_value(-1, "u64");
    });
    expect_rpc_error(RPC_INVALID_PARAMETER, "u32 overflow rejects", [] {
        (void)u64_to_u32_checked(static_cast<uint64_t>((std::numeric_limits<uint32_t>::max)()) + 1, "u32");
    });
    expect_rpc_error(RPC_INVALID_PARAMETER, "u8 overflow rejects", [] {
        (void)u64_to_u8_checked(256, "u8");
    });

    bytes decoded;
    expect(hex_to_bytes_checked("00ff", decoded) && decoded == bytes({0x00, 0xff}), "hex bytes parse");
    expect(!hex_to_bytes_checked("0xz", decoded), "bad hex bytes reject");
    expect(bytes_to_hex_string(bytes{0xab, 0xcd}) == "abcd", "bytes hex encodes");
}

void test_encoding_and_dispatch()
{
    using namespace geocoin;
    using namespace geocoin::rpc;

    bytes decoded;
    expect(hex_to_bytes_checked("00ff", decoded), "hex bytes parse");
    expect(decoded == bytes({0x00, 0xff}), "hex bytes content matches");
    expect(!hex_to_bytes_checked("0xz", decoded), "invalid hex rejects");
    expect(bytes_to_hex_string(bytes{0xab, 0xcd}) == "abcd", "bytes encode to hex");

    Hash256 hash = hash_from_seed(1);
    Hash256 parsed_hash{};
    expect(hex_to_hash256_checked(hash256_to_hex_string(hash), parsed_hash), "hash hex parses");
    expect(parsed_hash == hash, "hash hex roundtrips");
    expect(parse_hex_bytes_or_throw("0a", "hex") == bytes({0x0a}), "parse hex throws helper succeeds");
    expect_rpc_error(RPC_INVALID_PARAMETER, "parse hex throws helper rejects", [] {
        (void)parse_hex_bytes_or_throw("xx", "hex");
    });

    uint256 value;
    value.data[0] = 1;
    value.data[3] = 0x1234;
    expect(uint256_to_hex_string(value).starts_with("0000000000001234"), "uint256 hex is big-endian");

    json success = make_json_rpc_success(json{{"ok", true}}, 10);
    expect(success["id"] == 10 && !success.contains("error"), "success response shape");
    json error = make_json_rpc_error(RPC_INVALID_PARAMS, "bad", nullptr);
    expect(error["error"]["code"] == RPC_INVALID_PARAMS, "error response shape");
    json invalid = make_json_rpc_invalid_request("bad");
    expect(invalid["error"]["code"] == RPC_INVALID_REQUEST, "invalid request helper shape");

    RPCCommand command;
    command.name = "example";
    command.param_names = {"first", "second"};
    expect(normalize_rpc_params(command, nullptr).empty(), "null params normalize to empty array");
    expect(normalize_rpc_params(command, json::array({1, 2})).size() == 2, "array params pass through");
    expect_rpc_error(RPC_INVALID_PARAMS, "extra positional params reject", [&] {
        (void)normalize_rpc_params(command, json::array({1, 2, 3}));
    });
    expect(normalize_rpc_params(command, json{{"first", 1}, {"second", 2}}).size() == 2,
           "named params normalize in order");
    expect_rpc_error(RPC_INVALID_PARAMS, "unknown named params reject", [&] {
        (void)normalize_rpc_params(command, json{{"first", 1}, {"typo", 2}});
    });
    expect(normalize_rpc_params(command, json{{"second", 2}}) == json::array({nullptr, 2}),
           "named params preserve omitted defaults as null placeholders");
    RPCCommand no_named;
    no_named.name = "nonamed";
    expect_rpc_error(RPC_INVALID_PARAMS, "named params without names reject", [&] {
        (void)normalize_rpc_params(no_named, json{{"x", 1}});
    });
    expect_rpc_error(RPC_INVALID_PARAMS, "scalar params reject", [&] {
        (void)normalize_rpc_params(command, 1);
    });

    auto handler = [](const json& request, RPCAuthRole) {
        if (!request.is_object()) {
            return make_json_rpc_invalid_request("bad item", nullptr);
        }
        const json id = request.contains("id") ? request["id"] : json(nullptr);
        return make_json_rpc_success(request.value("method", ""), id);
    };
    json batch = json::array({
        json{{"method", "a"}, {"id", 1}},
        json{{"method", "b"}, {"id", 2}},
    });
    json batch_result = dispatch_json_rpc_payload(batch, RPCAuthRole::ADMIN, handler);
    expect(batch_result.is_array() && batch_result.size() == 2, "batch dispatch returns array");
    expect(dispatch_json_rpc_payload(json{{"method", "one"}, {"id", 3}}, RPCAuthRole::READONLY, handler)["result"] == "one",
           "single dispatch returns object");
    expect(dispatch_json_rpc_payload(json::array(), RPCAuthRole::ADMIN, handler)["error"]["code"] == RPC_INVALID_REQUEST,
           "empty batch rejects");
    json too_large = json::array();
    for (int i = 0; i < 101; ++i) {
        too_large.push_back(json{{"method", "x"}, {"id", i}});
    }
    expect(dispatch_json_rpc_payload(too_large, RPCAuthRole::ADMIN, handler)["error"]["code"] == RPC_INVALID_REQUEST,
           "oversized batch rejects");
}

void test_transaction_formatting()
{
    using namespace geocoin;
    using namespace geocoin::core;
    using namespace geocoin::rpc;

    state::ChainState chain_state(std::make_unique<state::MemoryStorage>());
    const Address addr1 = address_from_key(21);
    const Address addr2 = address_from_key(22);

    Transaction standard;
    standard.version = 2;
    standard.type = TransactionType::STANDARD;
    standard.timestamp = 1000;
    standard.inputs = {input_from_seed(1, 2)};
    standard.outputs = {output_to(addr1, 500)};
    json standard_json = transaction_to_verbose_json(standard, hash_from_seed(2), hex_hash(2), chain_state);
    expect(standard_json["txid"] == hex_hash(2), "standard txid formats");
    expect(standard_json["inputs"][0]["vout"] == 2, "standard inputs format");
    expect(standard_json["outputs"][0]["address"] == addr1.str(), "standard outputs format");
    expect(!standard_json.contains("memo"), "raw transaction JSON has no wallet-local note");

    Transaction cadastral;
    cadastral.type = TransactionType::CADASTRAL_DOC;
    cadastral.cadastral_doc_hash = hash_from_seed(3);
    cadastral.cadastral_number = "77:01:1";
    cadastral.cadastral_owner = addr1;
    cadastral.cadastral_nft_id = hash_from_seed(42);
    json cadastral_json = transaction_to_verbose_json(cadastral, hash_from_seed(4), hex_hash(4), chain_state);
    expect(cadastral_json["doc_hash"] == hex_hash(3), "cadastral doc hash formats");
    expect(cadastral_json["owner_address"] == addr1.str(), "cadastral owner formats");

    Transaction program;
    program.type = TransactionType::CARD_PROGRAM_DEFINE;
    program.card_program_id = hash_from_seed(5);
    program.card_issuer = addr1;
    program.card_chip_vendor = "vendor";
    program.card_chip_os = "os";
    program.card_applet_aid = "aid";
    program.card_applet_version = 9;
    program.card_applet_package_hash = hash_from_seed(6);
    program.card_policy_id = 12;
    program.card_scope_id = hash_from_seed(7);
    expect(transaction_to_verbose_json(program, hash_from_seed(8), hex_hash(8), chain_state)
               ["card_program_define"]["chip_vendor"] == "vendor",
           "card program formats");

    Transaction issue;
    issue.type = TransactionType::CARD_ISSUE;
    issue.card_program_id = hash_from_seed(9);
    issue.card_issuer = addr1;
    issue.card_uid_hash = hash_from_seed(10);
    issue.card_pubkey_hash = hash_from_seed(11);
    issue.card_valid_from = 1;
    issue.card_valid_to = 2;
    issue.card_status = 1;
    issue.card_metadata_hash = hash_from_seed(12);
    expect(transaction_to_verbose_json(issue, hash_from_seed(13), hex_hash(13), chain_state)
               ["card_issue"]["status"] == 1,
           "card issue formats");

    Transaction status;
    status.type = TransactionType::CARD_UPDATE_STATUS;
    status.card_program_id = hash_from_seed(14);
    status.card_issuer = addr1;
    status.card_uid_hash = hash_from_seed(15);
    status.card_status = 2;
    status.card_metadata_hash = hash_from_seed(16);
    expect(transaction_to_verbose_json(status, hash_from_seed(17), hex_hash(17), chain_state)
               ["card_update_status"]["status"] == 2,
           "card update formats");

    Transaction angle = Transaction::create_angle_commitment(
        Address("GEOMTEST"), hash_from_seed(18), make_key(23), Address(""), "degree");
    json angle_json = transaction_to_verbose_json(angle, angle.get_hash(), hash256_to_hex_string(angle.get_hash()), chain_state);
    expect(angle_json["multisig_address"] == "GEOMTEST", "angle commitment multisig formats");
    expect(angle_json["spending_tx_hash"] == hex_hash(18), "angle commitment spend hash formats");

    Transaction multisig = Transaction::create_multisig_spend(
        {input_from_seed(19, 0)},
        {output_to(addr2, 700)},
        {hash_from_seed(20), hash_from_seed(21)},
        2,
        3
    );
    multisig.spend_participants = {addr1, addr2, address_from_key(24)};
    json multisig_json = transaction_to_verbose_json(multisig, hash_from_seed(22), hex_hash(22), chain_state);
    expect(multisig_json["m"] == 2 && multisig_json["n"] == 3, "multisig m/n formats");
    expect(multisig_json["angle_commitment_hashes"].size() == 2, "multisig commitment hashes format");
    expect(multisig_json["spend_participants"].size() == 3, "multisig spend participants format");

    multisig.delegation_proof_txid = hash_from_seed(23);
    json hierarchical_json = transaction_to_verbose_json(multisig, hash_from_seed(24), hex_hash(24), chain_state);
    expect(hierarchical_json["delegation_proof_txid"] == hex_hash(23), "hierarchical delegation txid formats");

    Transaction proof;
    proof.type = TransactionType::DELEGATION_PROOF;
    proof.primary_multisig_address = Address("GEOMPRIMARY");
    proof.delegated_multisig_address = Address("GEOMDELEGATED");
    proof.primary_m = 2;
    proof.primary_n = 3;
    proof.primary_participants = {addr1, addr2, address_from_key(25)};
    proof.delegated_m = 1;
    proof.delegated_n = 2;
    proof.delegated_participants = {addr1, addr2};
    proof.valid_from = 1;
    proof.valid_until = 2;
    proof.nonce = 3;
    proof.max_total_delegated = 4 * COIN;
    proof.max_per_transaction = 2 * COIN;
    proof.max_percent_of_balance = 25;
    json proof_json = transaction_to_verbose_json(proof, hash_from_seed(26), hex_hash(26), chain_state);
    expect(proof_json["primary_participants"].size() == 3, "delegation participants format");
    expect(proof_json["max_total_delegated"] == 4 * COIN, "delegation angles format");

    const uint64_t now = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
    proof.valid_from = now + 1000;
    proof.valid_until = 0;
    json future_proof = delegation_proof_to_json(proof, hash_from_seed(27), hex_hash(27), chain_state);
    expect(!future_proof["is_valid"].get<bool>() && !future_proof["is_expired"].get<bool>(),
           "future delegation proof validity formats");
    proof.valid_from = 0;
    proof.valid_until = now > 0 ? now - 1 : 1;
    json expired_proof = delegation_proof_to_json(proof, hash_from_seed(28), hex_hash(28), chain_state);
    expect(!expired_proof["is_valid"].get<bool>() && expired_proof["is_expired"].get<bool>(),
           "expired delegation proof validity formats");
    expect(expired_proof["max_total_delegated"] == 4.0, "delegation coin amount formats");

    Transaction revoke;
    revoke.type = TransactionType::REVOKE_DELEGATION;
    revoke.revoked_delegation_proof_txid = hash_from_seed(29);
    expect(transaction_to_verbose_json(revoke, hash_from_seed(30), hex_hash(30), chain_state)
               ["revoked_delegation_proof_txid"] == hex_hash(29),
           "revoke delegation txid formats");

    nft::NFTMetadata metadata;
    metadata.name = "parcel";
    metadata.description = "desc";
    metadata.image_uri = "ipfs://image";
    metadata.content_hash = std::string(64, 'a');
    metadata.geometry.polygons.front().exterior = {{1000LL, 2000LL}, {3000LL, 4000LL}, {5000LL, 6000LL}};
    metadata.area_bits = 123;
    metadata.created_at = 456;
    metadata.block_height = 7;
    metadata.block_hash = hash_from_seed(31);
    metadata.location_name = "loc";
    metadata.creator = addr1.str();
    metadata.tags = {"land", "test"};
    nft::NFTMetadata::CadastralData cadastral_data;
    cadastral_data.cadastral_number = "77:01";
    cadastral_data.owner_name = "owner";
    cadastral_data.land_category = "cat";
    cadastral_data.permitted_use = "use";
    cadastral_data.area_sqcm_official = 100;
    cadastral_data.ownership_document_hash = hash_from_seed(32);
    cadastral_data.technical_plan_hash = hash_from_seed(33);
    cadastral_data.photo_hashes = {hash_from_seed(34)};
    cadastral_data.market_value = 1000;
    cadastral_data.cadastral_value = 900;
    cadastral_data.last_valuation_date = 800;
    cadastral_data.restrictions = {"none"};
    cadastral_data.has_buildings = true;
    cadastral_data.building_count = 2;
    metadata.cadastral = cadastral_data;

    nft::NFTCreationTransaction nft_create;
    nft_create.nft_id = hash_from_seed(35);
    nft_create.metadata = metadata;
    nft_create.creator_address = addr1;
    nft_create.royalty_percent = 500;
    nft_create.pol_data = nft::ProofOfLocationData{10, 20, 30, 40, 1};
    Transaction nft_create_tx;
    nft_create_tx.type = TransactionType::NFT_CREATE;
    nft_create_tx.nft_create = nft_create;
    json nft_create_json = transaction_to_verbose_json(nft_create_tx, hash_from_seed(36), hex_hash(36), chain_state);
    expect(nft_create_json["nft_create"]["metadata"]["cadastral"]["building_count"] == 2,
           "nft create cadastral formats");
    expect(nft_create_json["nft_create"]["pol_data"]["gps_accuracy_cm"] == 40,
           "nft create proof-of-location formats");

    nft::NFTTransferTransaction nft_transfer;
    nft_transfer.nft_id = hash_from_seed(37);
    nft_transfer.from = addr1;
    nft_transfer.to = addr2;
    nft_transfer.fee = 12;
    Transaction nft_transfer_tx;
    nft_transfer_tx.type = TransactionType::NFT_TRANSFER;
    nft_transfer_tx.nft_transfer = nft_transfer;
    expect(transaction_to_verbose_json(nft_transfer_tx, hash_from_seed(38), hex_hash(38), chain_state)
               ["nft_transfer"]["fee"] == 12,
           "nft transfer formats");

    nft::NFTBurnTransaction nft_burn;
    nft_burn.nft_id = hash_from_seed(39);
    nft_burn.owner = addr1;
    nft_burn.fee = 13;
    Transaction nft_burn_tx;
    nft_burn_tx.type = TransactionType::NFT_BURN;
    nft_burn_tx.nft_burn = nft_burn;
    expect(transaction_to_verbose_json(nft_burn_tx, hash_from_seed(40), hex_hash(40), chain_state)
               ["nft_burn"]["fee"] == 13,
           "nft burn formats");
}

void test_block_formatting_unknown_height()
{
    using namespace geocoin;
    using namespace geocoin::core;
    using namespace geocoin::rpc;

    auto chain_state = std::make_shared<state::ChainState>(std::make_unique<state::MemoryStorage>());
    core::Blockchain blockchain(chain_state, nullptr);

    Block block;
    block.header.version = 2;
    block.header.timestamp = 100;
    block.header.merkle_root = hash_from_seed(41);
    block.header.witness_commitment = hash_from_seed(42);
    block.header.bits = 0x1d00ffff;
    block.header.nonce = 99;
    block.transactions.push_back(Transaction::create_coinbase(0, address_from_key(43), 50 * COIN));

    const Hash256 block_hash = block.get_hash();
    const std::string block_hash_hex = hash256_to_hex_string(block_hash);
    json out = block_to_verbose_json(block, block_hash, block_hash_hex, blockchain);
    expect(out["hash"] == block_hash_hex, "block hash formats");
    expect(out["confirmations"] == 0, "unknown block confirmations are zero");
    expect(out["height"].is_null(), "unknown block height is null");
    expect(out["tx"].size() == 1, "block transaction ids format");
}

void test_wallet_tx_helpers()
{
    using namespace geocoin;
    using namespace geocoin::core;
    using namespace geocoin::rpc;

    Transaction coinbase = Transaction::create_coinbase(1, address_from_key(50), 10 * COIN);
    const bytes serialized = coinbase.serialize();
    const Transaction decoded = deserialize_transaction_exact(serialized, "coinbase");
    expect(decoded.get_hash() == coinbase.get_hash(), "deserialize_transaction_exact roundtrips tx");
    expect(deserialize_transaction_hex_or_throw(bytes_to_hex_string(serialized), "bad hex", "coinbase").get_hash() ==
               coinbase.get_hash(),
           "deserialize_transaction_hex_or_throw roundtrips tx");
    expect(transaction_txid_hex(coinbase) == hash256_to_hex_string(coinbase.get_hash()),
           "transaction_txid_hex formats hash");
    expect(!requires_basic_signature_verification(TransactionType::STANDARD),
           "STANDARD does not require basic signature verification helper");
    expect(requires_basic_signature_verification(TransactionType::NFT_CREATE),
           "NFT_CREATE requires basic signature verification helper");

    bytes trailing = serialized;
    trailing.push_back(0);
    expect_rpc_error(RPC_INVALID_PARAMETER, "trailing tx bytes reject", [&] {
        (void)deserialize_transaction_exact(trailing, "trailing");
    });
    expect_rpc_error(RPC_INVALID_PARAMETER, "bad tx hex rejects", [] {
        (void)deserialize_transaction_hex_or_throw("zz", "bad hex", "tx_hex");
    });
    expect_rpc_error(RPC_INVALID_PARAMETER, "oversized tx rejects before deserialization", [] {
        bytes oversized(consensus::params::MAX_TX_SIZE + 1, 0);
        (void)deserialize_transaction_exact(oversized, "oversized");
    });

    RawTransactionPreflight preflight;
    preflight.txid = hash256_to_hex_string(hash_from_seed(51));
    preflight.type = static_cast<int>(TransactionType::STANDARD);
    preflight.verified = true;
    preflight.valid = false;
    preflight.contextual = false;
    preflight.reject_reason = "state-invalid";
    json preflight_json = raw_transaction_preflight_to_json(preflight);
    expect(!preflight.allowed(), "preflight allowed reflects all checks");
    expect(!preflight_json["allowed"].get<bool>(), "preflight JSON allowed is false");
    expect(preflight_json["reject_reason"] == "state-invalid", "preflight JSON includes reject_reason");
    const json preflight_without_type = raw_transaction_preflight_to_json(preflight, false);
    expect(preflight_without_type.find("type") == preflight_without_type.end(), "preflight JSON can omit type");

    auto chain_state = std::make_shared<state::ChainState>(std::make_unique<state::MemoryStorage>());
    network::Mempool mempool(chain_state);
    auto mutation_service = std::make_shared<test::MempoolMutationService>(mempool);

    expect_rpc_error(RPC_INTERNAL_ERROR, "submit rejects null chainstate", [&] {
        (void)submit_transaction_to_mempool(
            coinbase,
            nullptr,
            "",
            false,
            false,
            mutation_service
        );
    });
    expect_rpc_error(RPC_INTERNAL_ERROR, "submit rejects null mutation service", [&] {
        (void)submit_transaction_to_mempool(
            coinbase,
            chain_state,
            "",
            false,
            false,
            nullptr
        );
    });

    Transaction unsigned_deposit;
    unsigned_deposit.type = TransactionType::LOCK_DEPOSIT;
    unsigned_deposit.sidechain_id = hash_from_seed(52);
    unsigned_deposit.sidechain_owner = address_from_key(53);
    expect_rpc_error(RPC_VERIFY_ERROR, "submit verifies signature when requested", [&] {
        (void)submit_transaction_to_mempool(
            unsigned_deposit,
            chain_state,
            "deposit",
            true,
            false,
            nullptr
        );
    });

    Transaction state_invalid;
    state_invalid.type = TransactionType::STANDARD;
    state_invalid.inputs.push_back(input_from_seed(54, 0));
    state_invalid.outputs.push_back(output_to(address_from_key(55), 1 * COIN));
    expect_rpc_error(RPC_VERIFY_ERROR, "submit includes type in validation error", [&] {
        (void)submit_transaction_to_mempool(
            state_invalid,
            chain_state,
            "standard",
            false,
            true,
            nullptr
        );
    });
    expect_rpc_error(RPC_INVALID_PARAMETER, "submit raw invalid hex rejects with label", [&] {
        (void)submit_raw_transaction_hex_to_mempool(
            "not-hex",
            chain_state,
            "raw",
            false,
            false,
            nullptr
        );
    });

    Transaction signed_payment = make_signed_payment(
        chain_state,
        57,
        58,
        1 * COIN,
        consensus::params::MIN_TX_FEE * 10
    );
    const std::string signed_payment_hex = bytes_to_hex_string(signed_payment.serialize());
    expect(
        submit_raw_transaction_hex_to_mempool(
            signed_payment_hex,
            chain_state,
            "signed",
            true,
            false,
            mutation_service
        ) == transaction_txid_hex(signed_payment),
        "submit_raw_transaction_hex_to_mempool accepts signed standard tx"
    );
    expect(mempool.has_transaction(signed_payment.get_hash()), "submitted signed tx enters mempool");

    json hex_result = transaction_hex_result(coinbase);
    expect(hex_result["txid"] == transaction_txid_hex(coinbase), "transaction_hex_result includes txid");
    expect(hex_result["tx_hex"].get<std::string>() == bytes_to_hex_string(serialized),
           "transaction_hex_result includes serialized tx");

    std::vector<bytes> signatures;
    set_committee_signatures_from_rpc(signatures, "aabb");
    expect(signatures.size() == 1 && signatures[0] == bytes({0xaa, 0xbb}),
           "set_committee_signatures_from_rpc accepts string");
    set_committee_signatures_from_rpc(signatures, json::array({"cc", "dd"}));
    expect(signatures.size() == 2 && signatures[0] == bytes({0xcc}) && signatures[1] == bytes({0xdd}),
           "set_committee_signatures_from_rpc accepts array");
    expect_rpc_error(RPC_INVALID_PARAMETER, "committee signatures reject non-string", [&] {
        set_committee_signatures_from_rpc(signatures, json::array({1}));
    });

    Transaction batch;
    batch.type = TransactionType::BATCH_SIDECHAIN_CHECKPOINT;
    BatchCheckpointItem item;
    batch.sidechain_batch_checkpoints.push_back(item);
    set_batch_checkpoint_signatures_from_rpc(batch, json::array({json::array({"aa", "bb"})}));
    expect(batch.sidechain_batch_checkpoints[0].committee_signatures.size() == 2,
           "set_batch_checkpoint_signatures_from_rpc fills item signatures");
    expect_rpc_error(RPC_INVALID_PARAMETER, "batch signatures reject wrong item count", [&] {
        set_batch_checkpoint_signatures_from_rpc(batch, json::array());
    });

    auto package_chain_state = std::make_shared<state::ChainState>(std::make_unique<state::MemoryStorage>());
    network::Mempool package_mempool(package_chain_state);
    auto package_mutation_service =
        std::make_shared<test::MempoolMutationService>(package_mempool);
    Transaction commitment_tx = make_signed_payment(
        package_chain_state,
        59,
        60,
        1 * COIN,
        consensus::params::MIN_TX_FEE * 10
    );
    Transaction final_tx = make_signed_payment(
        package_chain_state,
        61,
        62,
        1 * COIN,
        consensus::params::MIN_TX_FEE * 10
    );
    json package_result = submit_commitment_package_from_rpc(
        json{
            {"commitments", json::array({bytes_to_hex_string(commitment_tx.serialize())})},
            {"final_tx", bytes_to_hex_string(final_tx.serialize())},
        },
        "final_tx",
        "final_txid",
        package_chain_state,
        true,
        package_mutation_service
    );
    expect(package_result["commitment_txids"].size() == 1, "commitment package returns commitment txid");
    expect(package_result["final_txid"] == transaction_txid_hex(final_tx),
           "commitment package returns final txid");
    expect_rpc_error(RPC_INVALID_PARAMETER, "commitment package rejects non-string commitment", [&] {
        (void)submit_commitment_package_from_rpc(
            json{{"commitments", json::array({1})}, {"final_tx", bytes_to_hex_string(final_tx.serialize())}},
            "final_tx",
            "final_txid",
            package_chain_state,
            true,
            package_mutation_service
        );
    });

    nft::NFTCreationTransaction nft_create;
    nft_create.nft_id = hash_from_seed(63);
    nft_create.creator_address = address_from_key(64);
    nft_create.royalty_percent = 100;
    nft_create.metadata.name = "parcel";
    nft_create.metadata.content_hash = std::string(64, 'a');
    nft_create.metadata.geometry.polygons.front().exterior = {{1000LL, 2000LL}, {3000LL, 4000LL}, {5000LL, 6000LL}};
    Transaction nft_create_tx;
    nft_create_tx.type = TransactionType::NFT_CREATE;
    nft_create_tx.nft_create = nft_create;
    const std::string falcon_like_sig_hex(20, 'a');
    json finalized_create = finalize_nft_transaction_signature(
        json::array({bytes_to_hex_string(nft_create_tx.serialize()), falcon_like_sig_hex}),
        TransactionType::NFT_CREATE,
        "NFT_CREATE"
    );
    Transaction decoded_create = deserialize_transaction_hex_or_throw(
        finalized_create["tx_hex"].get<std::string>(),
        "bad finalized nft",
        "nft"
    );
    expect(decoded_create.nft_create.has_value() && decoded_create.nft_create->signature.size() == 10,
           "finalize_nft_transaction_signature attaches create signature");

    nft::NFTTransferTransaction nft_transfer;
    nft_transfer.nft_id = hash_from_seed(65);
    nft_transfer.from = address_from_key(66);
    nft_transfer.to = address_from_key(67);
    nft_transfer.fee = 10;
    Transaction nft_transfer_tx;
    nft_transfer_tx.type = TransactionType::NFT_TRANSFER;
    nft_transfer_tx.nft_transfer = nft_transfer;
    json finalized_transfer = finalize_nft_transaction_signature(
        json::array({bytes_to_hex_string(nft_transfer_tx.serialize()), falcon_like_sig_hex}),
        TransactionType::NFT_TRANSFER,
        "NFT_TRANSFER"
    );
    Transaction decoded_transfer = deserialize_transaction_hex_or_throw(
        finalized_transfer["tx_hex"].get<std::string>(),
        "bad finalized transfer",
        "nft"
    );
    expect(decoded_transfer.nft_transfer.has_value() && decoded_transfer.nft_transfer->signature.size() == 10,
           "finalize_nft_transaction_signature attaches transfer signature");

    nft::NFTBurnTransaction nft_burn;
    nft_burn.nft_id = hash_from_seed(68);
    nft_burn.owner = address_from_key(69);
    nft_burn.fee = 11;
    Transaction nft_burn_tx;
    nft_burn_tx.type = TransactionType::NFT_BURN;
    nft_burn_tx.nft_burn = nft_burn;
    json finalized_burn = finalize_nft_transaction_signature(
        json::array({bytes_to_hex_string(nft_burn_tx.serialize()), falcon_like_sig_hex}),
        TransactionType::NFT_BURN,
        "NFT_BURN"
    );
    Transaction decoded_burn = deserialize_transaction_hex_or_throw(
        finalized_burn["tx_hex"].get<std::string>(),
        "bad finalized burn",
        "nft"
    );
    expect(decoded_burn.nft_burn.has_value() && decoded_burn.nft_burn->signature.size() == 10,
           "finalize_nft_transaction_signature attaches burn signature");
    expect_rpc_error(RPC_INVALID_PARAMETER, "NFT finalize rejects short signature", [&] {
        (void)finalize_nft_transaction_signature(
            json::array({bytes_to_hex_string(nft_burn_tx.serialize()), "aa"}),
            TransactionType::NFT_BURN,
            "NFT_BURN"
        );
    });
    expect_rpc_error(RPC_INVALID_PARAMETER, "NFT finalize rejects wrong type", [&] {
        (void)finalize_nft_transaction_signature(
            json::array({bytes_to_hex_string(coinbase.serialize()), falcon_like_sig_hex}),
            TransactionType::NFT_CREATE,
            "NFT_CREATE"
        );
    });
    expect(compute_recommended_fee_rate_per_kb(nullptr) == static_cast<uint64_t>(consensus::params::FEE_RATE_PER_KB),
           "recommended fee falls back without mempool");
    network::Mempool empty_limit_mempool(chain_state);
    empty_limit_mempool.set_max_size(0);
    expect(compute_recommended_fee_rate_per_kb(&empty_limit_mempool) ==
               static_cast<uint64_t>(consensus::params::FEE_RATE_PER_KB),
           "recommended fee falls back with zero max mempool");

    auto recommended_for_load = [&](size_t current_size, uint64_t fee_per_byte) {
        network::Mempool loaded(chain_state);
        loaded.set_max_size(1000);
        Transaction marker;
        marker.timestamp = static_cast<uint64_t>(current_size + fee_per_byte);
        network::MempoolTestAccess::seed_entry(loaded, marker, current_size, fee_per_byte);
        return compute_recommended_fee_rate_per_kb(&loaded);
    };
    const uint64_t base_rate = static_cast<uint64_t>(consensus::params::FEE_RATE_PER_KB);
    expect(recommended_for_load(499, 1) == base_rate, "fee rate stays at base below 50% mempool load");
    expect(recommended_for_load(970, 1) == base_rate * 16, "fee rate scales at 97% mempool load");
    expect(recommended_for_load(900, 1) == base_rate * 8, "fee rate scales at 90% mempool load");
    expect(recommended_for_load(750, 1) == base_rate * 4, "fee rate scales at 75% mempool load");
    expect(recommended_for_load(500, 1) == base_rate * 2, "fee rate scales at 50% mempool load");
    expect(recommended_for_load(100, 1000000) == base_rate,
           "one high-fee transaction cannot poison wallet recommendations");
    expect(network::fee_policy::scale_fee_to_rate(
               consensus::params::MIN_TX_FEE,
               base_rate * network::fee_policy::MAX_CONGESTION_MULTIPLIER
           ) == consensus::params::MIN_TX_FEE *
                    static_cast<Amount>(network::fee_policy::MAX_CONGESTION_MULTIPLIER),
           "fee scaling honors the bounded congestion multiplier");
    expect(network::fee_policy::scale_fee_to_rate(
               (std::numeric_limits<Amount>::max)(),
               base_rate * network::fee_policy::MAX_CONGESTION_MULTIPLIER
           ) == (std::numeric_limits<Amount>::max)(),
           "fee scaling saturates instead of overflowing");
    expect(compute_dynamic_wallet_fee(nullptr, chain_state, 1 * COIN, nullptr) == consensus::params::MIN_TX_FEE,
           "dynamic wallet fee handles null wallet");
    wallet::Wallet fee_wallet;
    expect(fee_wallet.import_private_key(make_key(70), "fee"), "fee wallet imports key");
    state::UTXO fee_utxo{};
    fee_utxo.tx_hash = hash_from_seed(141);
    fee_utxo.vout = 0;
    fee_utxo.value = 5 * COIN;
    fee_utxo.address = address_from_key(70);
    fee_utxo.height = 0;
    fee_utxo.is_coinbase = false;
    state::ChainStateTestAccess::add_utxo(*chain_state, fee_utxo);
    network::Mempool high_load(chain_state);
    high_load.set_max_size(1000);
    Transaction high_load_marker;
    high_load_marker.timestamp = 12345;
    network::MempoolTestAccess::seed_entry(high_load, high_load_marker, 970, 1);
    const Amount base_wallet_fee = fee_wallet.estimate_required_fee(1 * COIN, chain_state);
    expect(compute_dynamic_wallet_fee(&fee_wallet, chain_state, 1 * COIN, &high_load) ==
               base_wallet_fee * static_cast<Amount>(network::fee_policy::MAX_CONGESTION_MULTIPLIER),
           "dynamic wallet fee uses the bounded high-load multiplier");
}

void test_sidechain_helper_guards()
{
    using namespace geocoin;
    using namespace geocoin::core;
    using namespace geocoin::rpc;

    expect(get_validate_flag(json::object(), "validate-test"), "missing validate defaults to true");
    expect(!get_validate_flag(json{{"validate", false}}, "validate-test"), "false validate parses");
    expect_rpc_error(RPC_INVALID_PARAMETER, "non-bool validate rejects", [] {
        (void)get_validate_flag(json{{"validate", "false"}}, "validate-test");
    });

    expect_rpc_error(RPC_INVALID_PARAMETER, "committee missing m rejects", [] {
        (void)parse_sidechain_committee_from_rpc(json{{"committee_members", json::array()}});
    });
    expect_rpc_error(RPC_INVALID_PARAMETER, "committee missing members rejects", [] {
        (void)parse_sidechain_committee_from_rpc(json{{"committee_m", 1}});
    });
    json too_many_members = {{"committee_m", 1}, {"committee_members", json::array()}};
    for (size_t i = 0; i <= consensus::params::MAX_SIDECHAIN_COMMITTEE_MEMBERS; ++i) {
        too_many_members["committee_members"].push_back(address_from_key(static_cast<uint8_t>(70 + i)).str());
    }
    expect_rpc_error(RPC_INVALID_PARAMETER, "too many committee members reject", [&] {
        (void)parse_sidechain_committee_from_rpc(too_many_members);
    });

    std::vector<TxInput> inputs;
    expect_rpc_error(RPC_INVALID_PARAMETER, "input item must be object", [&] {
        append_transaction_inputs_from_rpc(json::array({1}), inputs);
    });
    expect_rpc_error(RPC_INVALID_PARAMETER, "input txid required", [&] {
        append_transaction_inputs_from_rpc(json::array({json{{"vout", 0}}}), inputs);
    });
    expect_rpc_error(RPC_INVALID_PARAMETER, "input script hex rejects", [&] {
        append_transaction_inputs_from_rpc(
            json::array({json{{"txid", hex_hash(80)}, {"vout", 0}, {"script_sig_hex", "xx"}}}),
            inputs
        );
    });
    expect_rpc_error(RPC_INVALID_PARAMETER, "legacy input signature hex rejects", [&] {
        append_transaction_inputs_from_rpc(
            json::array({json{{"txid", hex_hash(80)}, {"vout", 0}, {"signature_hex", "aa"}}}),
            inputs
        );
    });

    std::vector<TxOutput> outputs;
    expect_rpc_error(RPC_INVALID_PARAMETER, "output item must be object", [&] {
        (void)append_transaction_outputs_from_rpc(json::array({1}), outputs);
    });
    expect_rpc_error(RPC_INVALID_PARAMETER, "output address required", [&] {
        (void)append_transaction_outputs_from_rpc(json::array({json{{"value", 1}}}), outputs);
    });

    Transaction tx;
    expect_rpc_error(RPC_INVALID_PARAMETER, "bad optional checkpoint rejects", [&] {
        apply_optional_sidechain_checkpoint_from_rpc(json{{"checkpoint", "bad"}}, tx);
    });
    expect_rpc_error(RPC_INVALID_PARAMETER, "bad optional committee id rejects", [&] {
        apply_optional_sidechain_committee_id_from_rpc(json{{"committee_id", "bad"}}, tx);
    });

    state::SidechainRegistryEntry entry;
    entry.committee_m = 1;
    entry.committee_n = 1;
    entry.committee_members = {address_from_key(81)};
    entry.committee_id = consensus::compute_sidechain_committee_id(1, 1, entry.committee_members);
    Hash256 committee_id{};
    apply_active_sidechain_committee_id_or_throw(committee_id, entry);
    expect(committee_id == entry.committee_id, "empty committee id is filled from active registry");
    Hash256 wrong_committee = hash_from_seed(82);
    expect_rpc_error(RPC_INVALID_PARAMETER, "wrong committee id rejects", [&] {
        apply_active_sidechain_committee_id_or_throw(wrong_committee, entry);
    });

    fill_active_sidechain_committee_id_if_available(tx, nullptr, hash_from_seed(83));

    {
        auto storage = std::make_unique<state::MemoryStorage>();
        auto* storage_ptr = storage.get();
        const Hash256 sidechain_id = hash_from_seed(90);
        const Hash256 other_sidechain_id = hash_from_seed(91);

        Transaction single_checkpoint;
        single_checkpoint.type = TransactionType::SIDECHAIN_CHECKPOINT;
        single_checkpoint.sidechain_id = sidechain_id;
        single_checkpoint.sidechain_checkpoint = hash_from_seed(92);
        single_checkpoint.sidechain_checkpoint_height = 10;
        const Hash256 single_txid = single_checkpoint.get_hash();
        expect(storage_ptr->put_transaction(single_txid, single_checkpoint),
               "test stores single checkpoint tx");

        Transaction batch_checkpoint;
        batch_checkpoint.type = TransactionType::BATCH_SIDECHAIN_CHECKPOINT;
        BatchCheckpointItem batch_item;
        batch_item.sidechain_id = sidechain_id;
        batch_item.checkpoint = hash_from_seed(93);
        batch_item.checkpoint_height = 11;
        batch_checkpoint.sidechain_batch_checkpoints.push_back(batch_item);
        BatchCheckpointItem other_item = batch_item;
        other_item.sidechain_id = other_sidechain_id;
        other_item.checkpoint = hash_from_seed(94);
        batch_checkpoint.sidechain_batch_checkpoints.push_back(other_item);
        const Hash256 batch_txid = batch_checkpoint.get_hash();
        expect(storage_ptr->put_transaction(batch_txid, batch_checkpoint),
               "test stores batch checkpoint tx");

        Transaction duplicate_batch = batch_checkpoint;
        duplicate_batch.sidechain_batch_checkpoints[1].sidechain_id = sidechain_id;
        const Hash256 duplicate_batch_txid = duplicate_batch.get_hash();
        expect(storage_ptr->put_transaction(duplicate_batch_txid, duplicate_batch),
               "test stores duplicate sidechain batch tx");

        Transaction standard_tx;
        standard_tx.type = TransactionType::STANDARD;
        const Hash256 standard_txid = standard_tx.get_hash();
        expect(storage_ptr->put_transaction(standard_txid, standard_tx),
               "test stores non-checkpoint tx");

        state::ChainState chain_state(std::move(storage));
        ResolvedFraudRef ref{};
        std::string err;

        expect(!resolve_fraud_checkpoint_ref(nullptr, sidechain_id, single_txid, false, 0, ref, err),
               "resolve fraud ref rejects null ChainState");
        expect(resolve_fraud_checkpoint_ref(&chain_state, sidechain_id, single_txid, false, 0, ref, err),
               "resolve fraud ref accepts single checkpoint");
        expect(ref.index == 0xFFFF && ref.checkpoint == single_checkpoint.sidechain_checkpoint,
               "single checkpoint fraud ref resolves payload");
        expect(!resolve_fraud_checkpoint_ref(&chain_state, sidechain_id, single_txid, true, 1, ref, err),
               "single checkpoint rejects non-sentinel index");
        expect(!resolve_fraud_checkpoint_ref(&chain_state, other_sidechain_id, single_txid, false, 0, ref, err),
               "single checkpoint rejects different sidechain");

        expect(resolve_fraud_checkpoint_ref(&chain_state, sidechain_id, batch_txid, true, 0, ref, err),
               "batch checkpoint resolves explicit matching index");
        expect(ref.index == 0 && ref.checkpoint == batch_item.checkpoint,
               "batch checkpoint explicit index resolves payload");
        expect(!resolve_fraud_checkpoint_ref(&chain_state, sidechain_id, batch_txid, true, 0xFFFF, ref, err),
               "batch checkpoint rejects sentinel index");
        expect(!resolve_fraud_checkpoint_ref(&chain_state, sidechain_id, batch_txid, true, 9, ref, err),
               "batch checkpoint rejects out-of-range index");
        expect(!resolve_fraud_checkpoint_ref(&chain_state, sidechain_id, batch_txid, true, 1, ref, err),
               "batch checkpoint rejects index for another sidechain");
        expect(resolve_fraud_checkpoint_ref(&chain_state, sidechain_id, batch_txid, false, 0, ref, err),
               "batch checkpoint auto-resolves single matching item");
        expect(!resolve_fraud_checkpoint_ref(&chain_state, sidechain_id, duplicate_batch_txid, false, 0, ref, err),
               "batch checkpoint rejects ambiguous auto-resolve");
        expect(!resolve_fraud_checkpoint_ref(&chain_state, hash_from_seed(95), batch_txid, false, 0, ref, err),
               "batch checkpoint rejects missing sidechain item");
        expect(!resolve_fraud_checkpoint_ref(&chain_state, sidechain_id, standard_txid, false, 0, ref, err),
               "fraud ref rejects non-checkpoint tx");
        expect(!resolve_fraud_checkpoint_ref(&chain_state, sidechain_id, hash_from_seed(96), false, 0, ref, err),
               "fraud ref rejects missing tx");
    }

    const bytes proof = consensus::sidechain_checkpoint::build_financial_proof_v2(
        hash_from_seed(84),
        7,
        1000,
        1,
        std::vector<Hash256>{hash_from_seed(85), hash_from_seed(86)}
    );
    expect(proof.size() > 64, "checkpoint proof with merkle path serializes siblings");

    expect_rpc_error(RPC_INVALID_PARAMETER, "sidechain common requires id", [&] {
        Transaction common_tx;
        (void)build_sidechain_tx_common(json{{"owner_address", address_from_key(87).str()}}, common_tx, "common");
    });
    expect_rpc_error(RPC_INVALID_PARAMETER, "sidechain common requires owner", [&] {
        Transaction common_tx;
        (void)build_sidechain_tx_common(json{{"sidechain_id", hex_hash(88)}}, common_tx, "common");
    });
    expect_rpc_error(RPC_INVALID_PARAMETER, "sidechain common rejects bad owner", [&] {
        Transaction common_tx;
        (void)build_sidechain_tx_common(
            json{{"sidechain_id", hex_hash(88)}, {"owner_address", "bad-address"}},
            common_tx,
            "common"
        );
    });
    expect_rpc_error(RPC_INVALID_PARAMETER, "sidechain common rejects bad id hex", [&] {
        Transaction common_tx;
        (void)build_sidechain_tx_common(
            json{{"sidechain_id", "bad"}, {"owner_address", address_from_key(87).str()}},
            common_tx,
            "common"
        );
    });
    expect_rpc_error(RPC_INVALID_PARAMETER, "sidechain common rejects mismatched genesis", [&] {
        Transaction common_tx;
        (void)build_sidechain_tx_common(
            json{{"sidechain_id", hex_hash(88)}, {"genesis_hash", hex_hash(89)}, {"owner_address", address_from_key(87).str()}},
            common_tx,
            "common"
        );
    });

    Transaction issue_tx;
    issue_tx.type = TransactionType::ISSUE_SIDECHAIN;
    const std::string issue_hex = bytes_to_hex_string(issue_tx.serialize());
    const std::string falcon_like_sig_hex(20, 'a');
    json finalized = finalize_sidechain_tx(
        json::array({issue_hex, falcon_like_sig_hex}),
        TransactionType::ISSUE_SIDECHAIN,
        "finalizeissue"
    );
    expect(finalized["tx_hex"].get<std::string>().size() >= issue_hex.size(),
           "finalize_sidechain_tx attaches signature");
    expect_rpc_error(RPC_INVALID_PARAMETER, "finalize rejects bad tx hex", [&] {
        (void)finalize_sidechain_tx(
            json::array({"bad", falcon_like_sig_hex}),
            TransactionType::ISSUE_SIDECHAIN,
            "finalizeissue"
        );
    });
    expect_rpc_error(RPC_INVALID_PARAMETER, "finalize rejects short signature", [&] {
        (void)finalize_sidechain_tx(
            json::array({issue_hex, "aa"}),
            TransactionType::ISSUE_SIDECHAIN,
            "finalizeissue"
        );
    });
    Transaction lock_tx;
    lock_tx.type = TransactionType::LOCK_DEPOSIT;
    expect_rpc_error(RPC_INVALID_PARAMETER, "finalize rejects wrong sidechain tx type", [&] {
        (void)finalize_sidechain_tx(
            json::array({bytes_to_hex_string(lock_tx.serialize()), falcon_like_sig_hex}),
            TransactionType::ISSUE_SIDECHAIN,
            "finalizeissue"
        );
    });
}

void test_rpc_server_dispatch_and_component_guards()
{
    using namespace geocoin::rpc;
    using geocoin::rpc::RPCAuthRole;
    using geocoin::rpc::RPCCommand;
    using geocoin::rpc::RPCServer;
    using geocoin::rpc::RPCServerTestAccess;

    RPCServer server;
    RPCServerTestAccess::configure_credentials(server);

    RPCCommand ok;
    ok.name = "unit.ok";
    ok.category = "unit";
    ok.help = "unit.ok";
    ok.required_role = RPCAuthRole::READONLY;
    ok.param_names = {"value"};
    ok.method = [](const json& params) {
        return json{{"params", params}};
    };
    server.register_method(ok);

    RPCCommand admin_only = ok;
    admin_only.name = "unit.admin";
    admin_only.required_role = RPCAuthRole::ADMIN;
    admin_only.capabilities = {"admin"};
    admin_only.method = [](const json&) {
        return json{{"admin", true}};
    };
    server.register_method(admin_only);

    RPCCommand wallet_required = admin_only;
    wallet_required.name = "unit.wallet";
    wallet_required.requires_wallet = true;
    server.register_method(wallet_required);

    auto register_throwing = [&](const std::string& name, geocoin::rpc::RPCMethod method) {
        RPCCommand command = admin_only;
        command.name = name;
        command.method = std::move(method);
        server.register_method(command);
    };

    register_throwing("unit.rpc_exception", [](const json&) -> json {
        throw RPCException(RPC_INVALID_PARAMETER, "unit rpc exception");
    });
    register_throwing("unit.type_error", [](const json&) -> json {
        json scalar = 1;
        (void)scalar.get<std::string>();
        return nullptr;
    });
    register_throwing("unit.json_out_of_range", [](const json&) -> json {
        json object = json::object();
        (void)object.at("missing");
        return nullptr;
    });
    register_throwing("unit.invalid_argument", [](const json&) -> json {
        throw std::invalid_argument("unit invalid argument");
    });
    register_throwing("unit.out_of_range", [](const json&) -> json {
        std::vector<int> values;
        (void)values.at(1);
        return nullptr;
    });
    register_throwing("unit.std_exception", [](const json&) -> json {
        throw std::runtime_error("unit std exception");
    });
    for (const auto status : {geocoin::storage::ReadStatus::CORRUPT, geocoin::storage::ReadStatus::IO_ERROR}) {
        const std::string method = "unit.database_" + std::to_string(static_cast<int>(status));
        register_throwing(method, [status](const json&) -> json {
            throw geocoin::storage::ReadError(status, "unit database detail");
        });
        const auto response = RPCServerTestAccess::handle_call(server, json{{"method", method}, {"id", 14}}, RPCAuthRole::ADMIN);
        expect_response_error(response, RPC_DATABASE_ERROR, "database reads have a distinct RPC error");
        expect(response["id"] == 14 && response["error"]["message"] == "Database read failed", "database error preserves request id");
    }

    const json ok_response = RPCServerTestAccess::handle_call(
        server,
        json{{"jsonrpc", "2.0"}, {"method", "unit.ok"}, {"params", json{{"value", 7}}}, {"id", 1}},
        RPCAuthRole::READONLY
    );
    expect(!ok_response.contains("error"), "registered read-only RPC method succeeds");
    expect(ok_response["result"]["params"].is_array() && ok_response["result"]["params"][0] == 7,
           "named RPC params normalize into positional array");

    expect_response_error(
        RPCServerTestAccess::handle_call(server, json{1}, RPCAuthRole::ADMIN),
        RPC_INVALID_REQUEST,
        "scalar RPC request is rejected"
    );
    expect_response_error(
        RPCServerTestAccess::handle_call(server, json{{"id", 2}}, RPCAuthRole::ADMIN),
        RPC_INVALID_REQUEST,
        "RPC request without string method is rejected"
    );
    expect_response_error(
        RPCServerTestAccess::handle_call(
            server,
            json{{"method", "unit.ok"}, {"params", 42}, {"id", 3}},
            RPCAuthRole::ADMIN
        ),
        RPC_INVALID_PARAMS,
        "scalar RPC params are rejected"
    );
    expect_response_error(
        RPCServerTestAccess::handle_call(
            server,
            json{{"method", std::string(129, 'x')}, {"id", 4}},
            RPCAuthRole::ADMIN
        ),
        RPC_INVALID_REQUEST,
        "overlong RPC method name is rejected"
    );
    expect_response_error(
        RPCServerTestAccess::handle_call(server, json{{"method", "unit.missing"}, {"id", 5}}, RPCAuthRole::ADMIN),
        RPC_METHOD_NOT_FOUND,
        "missing RPC method is rejected"
    );
    expect_response_error(
        RPCServerTestAccess::handle_call(server, json{{"method", "unit.admin"}, {"id", 6}}, RPCAuthRole::READONLY),
        RPC_FORBIDDEN_BY_SAFE_MODE,
        "read-only RPC role cannot call admin method"
    );
    expect_response_error(
        RPCServerTestAccess::handle_call(server, json{{"method", "unit.wallet"}, {"id", 7}}, RPCAuthRole::ADMIN),
        RPC_WALLET_ERROR,
        "wallet-required RPC method rejects unloaded wallet"
    );

    expect_response_error(
        RPCServerTestAccess::handle_call(server, json{{"method", "unit.rpc_exception"}, {"id", 8}}, RPCAuthRole::ADMIN),
        RPC_INVALID_PARAMETER,
        "RPCException handler maps code"
    );
    expect_response_error(
        RPCServerTestAccess::handle_call(server, json{{"method", "unit.type_error"}, {"id", 9}}, RPCAuthRole::ADMIN),
        RPC_INVALID_PARAMS,
        "json type_error maps to invalid params"
    );
    expect_response_error(
        RPCServerTestAccess::handle_call(server, json{{"method", "unit.json_out_of_range"}, {"id", 10}}, RPCAuthRole::ADMIN),
        RPC_INVALID_PARAMS,
        "json out_of_range maps to invalid params"
    );
    expect_response_error(
        RPCServerTestAccess::handle_call(server, json{{"method", "unit.invalid_argument"}, {"id", 11}}, RPCAuthRole::ADMIN),
        RPC_INVALID_PARAMETER,
        "std invalid_argument maps to invalid parameter"
    );
    expect_response_error(
        RPCServerTestAccess::handle_call(server, json{{"method", "unit.out_of_range"}, {"id", 12}}, RPCAuthRole::ADMIN),
        RPC_INVALID_PARAMETER,
        "std out_of_range maps to invalid parameter"
    );
    const json internal_error = RPCServerTestAccess::handle_call(
        server,
        json{{"method", "unit.std_exception"}, {"id", 13}},
        RPCAuthRole::ADMIN
    );
    expect_response_error(
        internal_error,
        RPC_INTERNAL_ERROR,
        "std exception maps to internal error"
    );
    expect(internal_error["error"]["message"] == "Internal RPC error",
           "std exception details are not exposed to RPC clients");

    RPCServer component_server;
    expect_rpc_error(RPC_INTERNAL_ERROR, "require_blockchain rejects unloaded component", [&] {
        RPCServerTestAccess::require_blockchain(component_server);
    });
    expect_rpc_error(RPC_WALLET_ERROR, "require_wallet rejects unloaded component", [&] {
        RPCServerTestAccess::require_wallet(component_server);
    });
    expect_rpc_error(RPC_INTERNAL_ERROR, "require_chain_state rejects unloaded component", [&] {
        RPCServerTestAccess::require_chain_state(component_server);
    });
    expect_rpc_error(RPC_INTERNAL_ERROR, "require_mempool rejects unloaded component", [&] {
        RPCServerTestAccess::require_mempool(component_server);
    });
    expect_rpc_error(RPC_INTERNAL_ERROR, "require_peer_manager rejects unloaded component", [&] {
        RPCServerTestAccess::require_peer_manager(component_server);
    });
    expect_rpc_error(RPC_INTERNAL_ERROR, "require_node_info rejects unloaded component", [&] {
        RPCServerTestAccess::require_node_info(component_server);
    });
}

void test_rpc_rate_limit_zero_disables_limiter()
{
#ifdef _WIN32
    _putenv_s("GEOCOIN_RPC_RATE_LIMIT_MAX_REQUESTS_PER_WINDOW", "");
    _putenv_s("GEOCOIN_RPC_RATE_LIMIT_MAX_REQUESTS", "");
#else
    unsetenv("GEOCOIN_RPC_RATE_LIMIT_MAX_REQUESTS_PER_WINDOW");
    unsetenv("GEOCOIN_RPC_RATE_LIMIT_MAX_REQUESTS");
#endif

    using geocoin::rpc::RPCServer;
    using geocoin::rpc::RPCServerTestAccess;

    RPCServer server;
    for (int i = 0; i < 200; ++i) {
        RPCServerTestAccess::consume_rate_limit(server, "10.0.0.1");
    }
    expect(RPCServerTestAccess::rate_limit_count(server) == 0,
           "disabled RPC rate limit does not track or throttle clients");
    expect(geocoin::rpc::rpc_rate_limit_max_requests_per_window(0) == 0,
           "rate-limit helper keeps 0 as unlimited");
}

void test_rpc_server_http_security_paths()
{
    using namespace geocoin::rpc;
    using geocoin::rpc::HTTPRequest;
    using geocoin::rpc::RPCAuthRole;
    using geocoin::rpc::RPCCommand;
    using geocoin::rpc::RPCServer;
    using geocoin::rpc::RPCServerTestAccess;

#ifdef _WIN32
    _putenv_s("GEOCOIN_RPC_RATE_LIMIT_MAX_REQUESTS_PER_WINDOW", "120");
#else
    setenv("GEOCOIN_RPC_RATE_LIMIT_MAX_REQUESTS_PER_WINDOW", "120", 1);
#endif

    RPCServer server;
    RPCServerTestAccess::configure_credentials(server);

    expect(secure_string_equals("same-secret", "same-secret"),
           "constant-time credential comparison accepts equality");
    expect(!secure_string_equals("", std::string(256, '\0')),
           "credential comparison keeps full-width length differences");

    RPCCommand ok;
    ok.name = "unit.http.ok";
    ok.category = "unit";
    ok.help = "unit.http.ok";
    ok.required_role = RPCAuthRole::READONLY;
    ok.param_names = {"first", "second"};
    ok.method = [](const json& params) {
        return json{{"count", params.size()}};
    };
    server.register_method(ok);

    auto make_request = [](std::string body,
                           std::string user = "admin",
                           std::string password = "admin-pass",
                           std::string remote = "10.0.0.1") {
        HTTPRequest request;
        request.method = "POST";
        request.uri = "/";
        request.version = "HTTP/1.1";
        request.headers["content-type"] = "application/json";
        request.body = std::move(body);
        request.remote_addr = std::move(remote);
        request.auth_user = std::move(user);
        request.auth_password = std::move(password);
        return request;
    };

    HTTPRequest get_request = make_request("{}");
    get_request.method = "GET";
    const auto method_response = RPCServerTestAccess::handle_http(server, get_request);
    expect(method_response.status_code == 405 && method_response.headers.at("Allow") == "POST",
           "RPC HTTP handler rejects non-POST requests");

    HTTPRequest bad_auth = make_request("{}", "bad", "bad-pass", "10.0.0.2");
    expect(RPCServerTestAccess::handle_http(server, bad_auth).status_code == 401,
           "RPC HTTP handler rejects bad credentials");
    HTTPRequest blocked_auth = make_request("{}", "admin", "admin-pass", "10.0.0.2");
    expect(RPCServerTestAccess::handle_http(server, blocked_auth).status_code == 200,
           "failure for an unknown username does not block valid roles on the same network");
    HTTPRequest wrong_admin = make_request("{}", "admin", "wrong-pass", "10.0.0.2");
    expect(RPCServerTestAccess::handle_http(server, wrong_admin).status_code == 401,
           "RPC HTTP handler rejects a wrong password for a known principal");
    expect(RPCServerTestAccess::handle_http(server, blocked_auth).status_code == 401,
           "RPC HTTP handler throttles repeated attempts against that principal");

    const std::string escalating_key = "10.0.0.9";
    RPCServerTestAccess::record_auth_failure(server, escalating_key);
    expect(RPCServerTestAccess::is_auth_blocked(server, escalating_key),
           "RPC authentication failure starts a temporary block");
    RPCServerTestAccess::expire_auth_block(server, escalating_key);
    expect(!RPCServerTestAccess::is_auth_blocked(server, escalating_key) &&
               RPCServerTestAccess::auth_failures(server, escalating_key) == 1,
           "expired auth block retains failure history for escalation");
    RPCServerTestAccess::record_auth_success(server, escalating_key);
    expect(RPCServerTestAccess::auth_failures(server, escalating_key) == 1,
           "one valid RPC role cannot reset another role's failure history");
    RPCServerTestAccess::record_auth_failure(server, escalating_key);
    expect(RPCServerTestAccess::auth_failures(server, escalating_key) == 2,
           "subsequent authentication failure escalates instead of restarting");

    const std::string spray_key = "10.0.0.10";
    for (size_t i = 0; i < 60; ++i) {
        RPCServerTestAccess::record_auth_failure_rate(server, spray_key);
    }
    expect(RPCServerTestAccess::is_auth_failure_rate_limited(server, spray_key),
           "rotating usernames cannot bypass the per-network auth failure budget");

    HTTPRequest readonly_auth = make_request(
        R"({"jsonrpc":"2.0","method":"unit.http.ok","params":[1,2],"id":1})",
        "readonly",
        "readonly-pass",
        "10.0.0.3"
    );
    expect(RPCServerTestAccess::authenticate(server, readonly_auth) == RPCAuthRole::READONLY,
           "readonly credentials authenticate with readonly role");
    const auto ok_http = RPCServerTestAccess::handle_http(server, readonly_auth);
    expect(ok_http.status_code == 200, "authenticated RPC HTTP request returns 200");
    json ok_body = json::parse(ok_http.body);
    expect(!ok_body.contains("error") && ok_body["result"]["count"] == 2,
           "authenticated RPC HTTP request dispatches method");

    RPCServerTestAccess::set_warmup(server, true, "loading chainstate");
    HTTPRequest warmup = make_request(
        R"({"jsonrpc":"2.0","id":91,"method":"getrpcinfo"})", "admin", "admin-pass", "10.0.0.4");
    const auto warmup_response = RPCServerTestAccess::handle_http(server, warmup);
    const auto warmup_json = json::parse(warmup_response.body);
    expect_response_error(warmup_json, RPC_IN_WARMUP,
                          "RPC HTTP handler returns warmup error for a valid call");
    expect(warmup_json["id"] == 91 && warmup_json["jsonrpc"] == "2.0",
           "Warmup error preserves the request id and protocol version");
    warmup.body = R"({"jsonrpc":"2.0","method":"getrpcinfo"})";
    const auto warmup_notification = RPCServerTestAccess::handle_http(server, warmup);
    expect(warmup_notification.status_code == 204 && warmup_notification.body.empty(),
           "Warmup notifications receive no JSON response");
    warmup.body = R"([{"jsonrpc":"2.0","method":"getrpcinfo"},{"jsonrpc":"2.0","id":92,"method":"getrpcinfo"}])";
    const auto warmup_batch = json::parse(RPCServerTestAccess::handle_http(server, warmup).body);
    expect(warmup_batch.is_array() && warmup_batch.size() == 1 && warmup_batch[0]["id"] == 92,
           "Warmup batch preserves individual ids and omits notifications");
    expect_response_error(warmup_batch[0], RPC_IN_WARMUP, "Warmup batch item reports warmup");
    RPCServerTestAccess::set_warmup(server, false);

    HTTPRequest parse_error = make_request("{", "admin", "admin-pass", "10.0.0.5");
    expect_response_error(json::parse(RPCServerTestAccess::handle_http(server, parse_error).body),
                          RPC_PARSE_ERROR,
                          "RPC HTTP handler maps JSON parse errors");

    HTTPRequest batch = make_request(
        R"([{"jsonrpc":"2.0","method":"unit.http.ok","params":[],"id":1},{"jsonrpc":"2.0","method":"unit.http.missing","id":2}])",
        "admin",
        "admin-pass",
        "10.0.0.6"
    );
    json batch_body = json::parse(RPCServerTestAccess::handle_http(server, batch).body);
    expect(batch_body.is_array() && batch_body.size() == 2,
           "RPC HTTP handler dispatches JSON-RPC batch payload");
    expect(!batch_body[0].contains("error"), "RPC batch keeps successful item");
    expect(batch_body[1]["error"]["code"].get<int>() == RPC_METHOD_NOT_FOUND,
           "RPC batch keeps per-item method error");

    RPCServerTestAccess::seed_rate_limit(server, "10.0.0.8", 119);
    HTTPRequest batch_over_budget = make_request(
        R"([{"jsonrpc":"2.0","method":"unit.http.ok","id":10},{"jsonrpc":"2.0","method":"unit.http.ok","id":11}])",
        "admin",
        "admin-pass",
        "10.0.0.8"
    );
    expect(RPCServerTestAccess::handle_http(server, batch_over_budget).status_code == 429,
           "RPC batch consumes one rate-limit token per method");

    RPCServerTestAccess::seed_rate_limit(server, "10.0.0.7", (std::numeric_limits<uint32_t>::max)());
    HTTPRequest rate_limited = make_request(
        R"({"jsonrpc":"2.0","method":"unit.http.ok","id":3})",
        "admin",
        "admin-pass",
        "10.0.0.7"
    );
    const auto rate_response = RPCServerTestAccess::handle_http(server, rate_limited);
    expect(rate_response.status_code == 429 && rate_response.headers.count("Retry-After") == 1,
           "RPC HTTP handler returns explicit rate-limit response");

    HTTPRequest unknown_remote = make_request(
        R"({"jsonrpc":"2.0","method":"unit.http.ok","id":4})",
        "admin",
        "admin-pass",
        ""
    );
    expect(RPCServerTestAccess::handle_http(server, unknown_remote).status_code == 200,
           "RPC HTTP handler accepts authenticated request without remote address");

    HTTPRequest ipv6_a = make_request("{}", "bad", "bad", "2001:db8:1:2::1");
    HTTPRequest ipv6_b = make_request("{}", "bad", "bad", "2001:db8:1:2:ffff::99");
    expect(RPCServerTestAccess::auth_client_key(server, ipv6_a) ==
               RPCServerTestAccess::auth_client_key(server, ipv6_b),
           "RPC throttling groups rotating IPv6 interface identifiers by /64");
    expect(rpc_client_network_key("2001:db8:1:2::1") !=
               rpc_client_network_key("2001:db8:1:3::1"),
           "RPC client grouping keeps different IPv6 /64 networks separate");
    expect(rpc_client_network_key("::ffff:192.0.2.44") ==
               rpc_client_network_key("192.0.2.44"),
           "RPC client grouping canonicalizes IPv4-mapped IPv6 addresses");

    for (size_t i = 0; i < 4200; ++i) {
        const std::string key = "synthetic-client-" + std::to_string(i);
        RPCServerTestAccess::record_auth_failure(server, key);
        RPCServerTestAccess::record_auth_failure_rate(server, key);
        RPCServerTestAccess::consume_rate_limit(server, key);
    }
    expect(RPCServerTestAccess::auth_failure_count(server) <= 4096,
           "RPC authentication failure tracking has a hard memory bound");
    expect(RPCServerTestAccess::rate_limit_count(server) <= 4096,
           "RPC request-rate tracking has a hard memory bound");
    expect(RPCServerTestAccess::auth_failure_rate_count(server) <= 4096,
           "RPC auth-failure rate tracking has a hard memory bound");
}

void test_rpc_context_owns_components()
{
    std::weak_ptr<geocoin::wallet::Wallet> wallet_lifetime;
    {
        geocoin::rpc::RPCServer server;
        auto wallet = std::make_shared<geocoin::wallet::Wallet>();
        wallet_lifetime = wallet;

        expect(server.set_context(geocoin::rpc::RPCServerContext{
                   .wallet = wallet,
               }),
               "RPC context installs atomically");
        wallet.reset();

        expect(!wallet_lifetime.expired(),
               "RPC context keeps configured components alive for request handlers");
    }

    expect(wallet_lifetime.expired(),
           "RPC context releases configured components with the server");
}

void test_rpc_shutdown_callback_rearms_after_stop()
{
    geocoin::rpc::RPCServer server;
    std::atomic<unsigned> callback_count{0};
    server.set_shutdown_callback([&callback_count]() {
        callback_count.fetch_add(1, std::memory_order_release);
    });

    const auto wait_for_callbacks = [&callback_count](unsigned expected) {
        for (unsigned attempt = 0; attempt < 100; ++attempt) {
            if (callback_count.load(std::memory_order_acquire) >= expected) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    };

    geocoin::rpc::RPCServerTestAccess::request_shutdown(server);
    expect(wait_for_callbacks(1), "first RPC shutdown callback runs");
    server.stop();

    geocoin::rpc::RPCServerTestAccess::request_shutdown(server);
    expect(wait_for_callbacks(2),
           "RPC shutdown callback is rearmed after the previous lifecycle stops");
    server.stop();

    server.set_shutdown_callback([&server, &callback_count]() {
        server.stop();
        callback_count.fetch_add(1, std::memory_order_release);
    });
    geocoin::rpc::RPCServerTestAccess::request_shutdown(server);
    expect(wait_for_callbacks(3),
           "RPC shutdown callback may stop its own server without joining itself");
}

void test_rpc_context_rejects_conflicting_mutation_services()
{
    auto chain_state = std::make_shared<geocoin::state::ChainState>(
        std::make_unique<geocoin::state::MemoryStorage>()
    );
    geocoin::network::Mempool mempool(chain_state);
    auto direct_mutations =
        std::make_shared<geocoin::test::MempoolMutationService>(mempool);
    auto node_mutations =
        std::make_shared<geocoin::test::MempoolMutationService>(mempool);

    geocoin::rpc::RPCServerContext context;
    context.chain_mutations = direct_mutations;
    context.node_services.mutations = node_mutations;

    geocoin::rpc::RPCServer server;
    expect(!server.set_context(std::move(context)),
           "RPC context rejects conflicting chain mutation writers");
}

void test_rpc_command_profiles()
{
    using geocoin::rpc::RPCAuthRole;
    using geocoin::rpc::RPCCommand;
    using geocoin::rpc::RPCCommandProfile;
    using geocoin::rpc::RPCCommandRegistry;

    RPCCommand readonly;
    readonly.name = "unit.read";
    readonly.category = "unit";
    readonly.help = "unit.read";
    readonly.required_role = RPCAuthRole::READONLY;
    readonly.method = [](const json&) { return true; };

    RPCCommand miner = readonly;
    miner.name = "unit.mine";
    miner.required_role = RPCAuthRole::MINER;
    miner.method = [](const json&) { return true; };

    RPCCommand admin = readonly;
    admin.name = "unit.admin.profile";
    admin.required_role = RPCAuthRole::ADMIN;
    admin.capabilities = {"admin"};
    admin.method = [](const json&) { return true; };

    RPCCommandRegistry mining_registry(RPCCommandProfile::MINING_ONLY);
    expect(!mining_registry.add(readonly),
           "mining-only command profile rejects read-only handlers");
    expect(!mining_registry.add(admin),
           "mining-only command profile rejects admin handlers");
    expect(mining_registry.add(miner) && mining_registry.size() == 1,
           "mining-only command profile retains miner handlers");
    expect(mining_registry.find("unit.mine") != nullptr &&
               mining_registry.find("unit.admin.profile") == nullptr,
           "profile filtering changes the physical dispatch catalog");

    RPCCommandRegistry readonly_registry(RPCCommandProfile::READ_ONLY);
    expect(readonly_registry.add(readonly),
           "read-only command profile retains side-effect-free read handlers");
    expect(!readonly_registry.add(miner) && !readonly_registry.add(admin),
           "read-only command profile rejects miner and admin handlers");

    admin.mutates = true;
    admin.side_effects = "changes unit-test state";
    RPCCommandRegistry full_registry;
    expect(full_registry.add(admin), "full profile retains mutating admin handlers");
}

} // namespace

void test_content_length_contract()
{
    size_t length = 0;
    for (const std::string value : {"0", " 0\t", "12", "\t12 "}) {
        expect(geocoin::rpc::parse_rpc_content_length(value, length),
               "Content-Length accepts digits with surrounding HTTP whitespace");
        expect(length == (value.find("12") == std::string::npos ? 0u : 12u),
               "Content-Length preserves the parsed value");
    }
    for (const std::string value : {"", " \t", "1 2", "1\tx", "+1", "-1", "1\n", "1.0"}) {
        length = 99;
        expect(!geocoin::rpc::parse_rpc_content_length(value, length),
               "Content-Length rejects malformed values completely");
        expect(length == 99, "Invalid Content-Length leaves the output unchanged");
    }
    const auto maximum = std::to_string(std::numeric_limits<size_t>::max());
    expect(geocoin::rpc::parse_rpc_content_length(maximum, length) &&
           length == std::numeric_limits<size_t>::max(), "Content-Length accepts size_t max");
    expect(!geocoin::rpc::parse_rpc_content_length(maximum + "0", length),
           "Content-Length rejects overflow");
}

int main()
{
    try {
        test_amount_helpers();
        test_content_length_contract();
        test_params_encoding_and_dispatch();
        test_encoding_and_dispatch();
        test_wallet_tx_helpers();
        test_sidechain_helper_guards();
        test_rpc_server_dispatch_and_component_guards();
        test_rpc_rate_limit_zero_disables_limiter();
        test_rpc_server_http_security_paths();
        test_rpc_context_owns_components();
        test_rpc_shutdown_callback_rearms_after_stop();
        test_rpc_context_rejects_conflicting_mutation_services();
        test_rpc_command_profiles();
        test_transaction_formatting();
        test_block_formatting_unknown_height();
        std::cout << "test_rpc_units: ok\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "test_rpc_units failed: " << e.what() << "\n";
        return 1;
    }
}
