#include "geocoin/rpc/rpc_server.hpp"
#include "geocoin/rpc/rpc_encoding.hpp"
#include "geocoin/rpc/rpc_params.hpp"
#include "geocoin/rpc/rpc_wallet_helpers.hpp"
#include "geocoin/network/mempool.hpp"

#include <algorithm>
#include <chrono>
#include <limits>

namespace geocoin::rpc {
namespace {

constexpr size_t kDefaultMempoolPage = 1000;
constexpr size_t kMaxMempoolPage = 5000;
constexpr size_t kDefaultVerboseMempoolPage = 100;
constexpr size_t kMaxVerboseMempoolPage = 250;

bool is_zero_hash(const Hash256& hash) {
    for (const auto byte : hash) {
        if (byte != 0) {
            return false;
        }
    }
    return true;
}

json mempool_entry_to_json(const network::MempoolEntry& entry) {
    const auto txid = transaction_txid_hex(entry.tx);
    const auto now = std::chrono::steady_clock::now();
    const auto age_seconds = std::chrono::duration_cast<std::chrono::seconds>(now - entry.added_at).count();

    json depends = json::array();
    for (const auto& input : entry.tx.inputs) {
        if (!is_zero_hash(input.prev_tx_hash)) {
            depends.push_back(hash256_to_hex_string(input.prev_tx_hash));
        }
    }

    json out;
    out["txid"] = txid;
    out["type"] = static_cast<int>(entry.tx.type);
    out["size"] = entry.size;
    out["fee"] = entry.fee;
    out["fee_per_byte"] = entry.fee_per_byte;
    out["time_in_mempool_seconds"] = age_seconds >= 0 ? age_seconds : 0;
    out["depends"] = std::move(depends);
    out["hex"] = bytes_to_hex_string(entry.tx.serialize());
    return out;
}

} // namespace

void RPCServer::register_mempool_methods() {
    register_method({
        .name = "getrawmempool",
        .method = [this](const json& params) -> json {
            auto& mempool = require_mempool();
            const bool verbose = optional_param<bool>(params, 0, false);
            const size_t offset = u64_to_size_checked(
                optional_u64_param(params, 1, 0, "offset must be a non-negative integer"),
                "offset is too large");
            const size_t limit = optional_bounded_size_param(
                params,
                2,
                verbose ? kDefaultVerboseMempoolPage : kDefaultMempoolPage,
                1,
                verbose ? kMaxVerboseMempoolPage : kMaxMempoolPage,
                "limit");

            auto entries = mempool.get_entries_page(offset, limit);
            if (!verbose) {
                json out = json::array();
                for (const auto& entry : entries) {
                    out.push_back(transaction_txid_hex(entry.tx));
                }
                return out;
            }

            json out = json::object();
            for (const auto& entry : entries) {
                out[transaction_txid_hex(entry.tx)] = mempool_entry_to_json(entry);
            }
            return out;
        },
        .category = "mempool",
        .help = "getrawmempool ( verbose offset limit )\n\nReturns one deterministic txid-ordered mempool page. Defaults: 1000 entries, or 100 for verbose output. Maximum: 5000/250. Use getmempoolinfo.size to obtain the total.\n",
        .requires_wallet = false,
        .required_role = RPCAuthRole::READONLY,
        .param_names = {"verbose", "offset", "limit"}
    });

    register_method({
        .name = "getmempoolentry",
        .method = [this](const json& params) -> json {
            auto& mempool = require_mempool();
            const std::string txid_str = required_param(params, 0, "Missing txid").get<std::string>();

            Hash256 txid;
            if (!hex_to_hash256_checked(txid_str, txid)) {
                throw RPCException(RPC_INVALID_PARAMETER, "Invalid txid");
            }

            network::MempoolEntry entry;
            if (mempool.get_entry(txid, entry)) {
                return mempool_entry_to_json(entry);
            }
            throw RPCException(RPC_INVALID_ADDRESS_OR_KEY, "Transaction not found in mempool");
        },
        .category = "mempool",
        .help = "getmempoolentry \"txid\"\n\nReturns metadata for one transaction currently in the local mempool.\n",
        .requires_wallet = false,
        .required_role = RPCAuthRole::READONLY,
        .param_names = {"txid"}
    });

    register_method({
        .name = "testmempoolaccept",
        .method = [this](const json& params) -> json {
            auto& chain_state = require_chain_state();
            auto& mempool = require_mempool();
            require_min_params(params, 1, "Missing raw transaction hex or array");

            json raw_txs;
            if (params[0].is_array()) {
                raw_txs = params[0];
            } else {
                raw_txs = json::array({params[0]});
            }

            if (raw_txs.empty()) {
                throw RPCException(RPC_INVALID_PARAMETER, "No transactions supplied");
            }
            if (raw_txs.size() > 25) {
                throw RPCException(RPC_INVALID_PARAMETER, "Too many transactions supplied");
            }

            json result = json::array();
            for (const auto& item : raw_txs) {
                if (!item.is_string()) {
                    throw RPCException(RPC_INVALID_PARAMETER, "Transaction must be a hex string");
                }
                core::Transaction tx = deserialize_transaction_hex_or_throw(
                    item.get<std::string>(),
                    "Invalid transaction hex",
                    "tx_hex"
                );
                result.push_back(raw_transaction_preflight_to_json(
                    validate_raw_transaction_for_mempool(tx, chain_state, mempool)
                ));
            }
            return result;
        },
        .category = "mempool",
        .help = "testmempoolaccept \"hexstring\"|[\"hexstring\",...]\n\nChecks whether raw transaction(s) would be accepted by mempool without submitting them.\n",
        .requires_wallet = false,
        .required_role = RPCAuthRole::READONLY,
        .param_names = {"rawtxs"}
    });
}

} // namespace geocoin::rpc
