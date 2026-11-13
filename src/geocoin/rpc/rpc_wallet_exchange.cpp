#include "geocoin/rpc/rpc_wallet_helpers.hpp"
#include "geocoin/rpc/rpc_params.hpp"
#include "geocoin/rpc/rpc_amount.hpp"
#include "geocoin/wallet/address_funds.hpp"
#include "geocoin/core/blockchain.hpp"
#include "geocoin/utils/secure_memory.hpp"
#include "rpc_blockchain_format.hpp"

#include <filesystem>
#include <set>
#include <map>

namespace geocoin::rpc {
namespace {
Hash256 required_hash(const json& params, size_t index) {
    const auto& value = required_param(params, index, "A transaction or block hash is required");
    Hash256 hash{};
    if (!value.is_string() || !hex_to_hash256_checked(value.get_ref<const std::string&>(), hash))
        throw RPCException(RPC_INVALID_PARAMETER, "Expected a 64-character hexadecimal hash");
    return hash;
}

struct HistoryView {
    state::ChainState& state;
    core::Blockchain& chain;
    core::ActiveTipSnapshot tip;
    std::vector<wallet::WalletTransaction> transactions;
    std::map<Hash256, size_t> index;
    std::set<Address> owned;
    std::set<Address> spendable;

    HistoryView(wallet::Wallet& w, state::ChainState& s, core::Blockchain& b)
        : state(s), chain(b), tip(b.get_active_tip_snapshot()) {
        auto snapshot = w.get_history_snapshot();
        transactions = std::move(snapshot.transactions);
        for (size_t i = 0; i < transactions.size(); ++i) index.emplace(transactions[i].tx_hash, i);
        owned.insert(snapshot.addresses.begin(), snapshot.addresses.end());
        spendable.insert(snapshot.spendable_addresses.begin(), snapshot.spendable_addresses.end());
        if (!owned.empty() && (snapshot.scanned_height != tip.height || snapshot.scanned_hash != tip.hash))
            throw RPCException(RPC_IN_WARMUP, "Wallet has not processed the current chain tip; retry the request");
    }

    void validate() const {
        if (!chain.is_active_tip(tip.hash) || state.get_tip_hash() != tip.hash)
            throw RPCException(RPC_IN_WARMUP, "Chain changed while reading wallet history; retry the request");
    }

    bool previous_output(const core::TxInput& input, core::TxOutput& out) const {
        const auto found = index.find(input.prev_tx_hash);
        if (found != index.end()) {
            const auto& tx = transactions[found->second].tx;
            if (input.vout >= tx.outputs.size()) return false;
            out = tx.outputs[input.vout]; return true;
        }
        const auto tx = state.get_transaction(input.prev_tx_hash);
        if (!tx || input.vout >= tx->outputs.size()) return false;
        out = tx->outputs[input.vout]; return true;
    }

    int64_t confirmations(const wallet::WalletTransaction& tx) const {
        if (tx.block_hash == Hash256{}) return 0;
        const auto position = chain.get_block_position(tx.block_hash);
        if (!position.indexed) throw RPCException(RPC_WALLET_ERROR, "Wallet confirmation history has been pruned or is unavailable");
        return position.in_active_chain && position.height <= tip.height ?
            static_cast<int64_t>(tip.height - position.height + 1) : 0;
    }

    json transaction(const wallet::WalletTransaction& tx, bool include_watchonly = true, bool include_change = false) const {
        Amount debit = 0, credit = 0, input_total = 0, output_total = 0;
        std::set<Address> input_addresses;
        bool inputs_known = !tx.is_coinbase;
        for (const auto& input : tx.tx.inputs) {
            core::TxOutput previous;
            if (!previous_output(input, previous)) { inputs_known = false; continue; }
            input_total = checked_add_amount_or_throw(input_total, previous.value, "Input amount overflow");
            if (owned.contains(previous.address)) input_addresses.insert(previous.address);
            if (owned.contains(previous.address) && (include_watchonly || spendable.contains(previous.address)))
                debit = checked_add_amount_or_throw(debit, previous.value, "Debit overflow");
        }
        for (const auto& output : tx.tx.outputs) {
            if (output.value < 0) throw RPCException(RPC_WALLET_ERROR, "Negative wallet output");
            output_total = checked_add_amount_or_throw(output_total, output.value, "Output amount overflow");
            if (owned.contains(output.address) && (include_watchonly || spendable.contains(output.address)))
                credit = checked_add_amount_or_throw(credit, output.value, "Credit overflow");
        }
        const Amount fee = debit > 0 ? (inputs_known && input_total >= output_total ? input_total - output_total : tx.fee) : 0;
        int64_t conf = confirmations(tx);
        json conflicts = json::array();
        if (conf == 0 && !tx.is_coinbase) {
            std::vector<const wallet::WalletTransaction*> ancestry{&tx};
            std::set<Hash256> visited;
            while (!ancestry.empty()) {
                const auto* current = ancestry.back(); ancestry.pop_back();
                if (!visited.insert(current->tx_hash).second) continue;
                for (const auto& [other, location] : current->conflicts) {
                    const auto position = chain.get_block_position(location.hash);
                    if (!position.in_active_chain || position.height > tip.height) continue;
                    const json id = hash256_to_hex_string(other);
                    if (std::find(conflicts.begin(), conflicts.end(), id) == conflicts.end()) conflicts.push_back(id);
                    conf = std::min(conf, -static_cast<int64_t>(tip.height - position.height + 1));
                }
                for (const auto& input : current->tx.inputs) {
                    const auto parent = index.find(input.prev_tx_hash);
                    if (parent != index.end() && confirmations(transactions[parent->second]) == 0)
                        ancestry.push_back(&transactions[parent->second]);
                }
            }
            std::set<std::pair<Hash256, uint32_t>> inputs;
            for (const auto& input : tx.tx.inputs) inputs.emplace(input.prev_tx_hash, input.vout);
            for (const auto& other : transactions) {
                if (other.tx_hash == tx.tx_hash) continue;
                const auto other_conf = confirmations(other);
                if (other_conf <= 0) continue;
                if (std::any_of(other.tx.inputs.begin(), other.tx.inputs.end(), [&](const auto& input) {
                    return inputs.contains({input.prev_tx_hash, input.vout});
                })) {
                    const auto id = hash256_to_hex_string(other.tx_hash);
                    if (std::find(conflicts.begin(), conflicts.end(), json(id)) == conflicts.end()) conflicts.push_back(id);
                    conf = std::min(conf, -other_conf);
                }
            }
        }
        Amount net = 0;
        if (!checked_sub_amount(credit, debit, net) || !checked_add_amount(net, fee, net))
            throw RPCException(RPC_WALLET_ERROR, "Wallet amount overflow");
        json result{{"txid", hash256_to_hex_string(tx.tx_hash)}, {"confirmations", conf},
            {"fromme", debit > 0},
            {"amount", static_cast<double>(net) / COIN}, {"amount_angles", net},
            {"time", tx.tx.timestamp}, {"timereceived", tx.received_at ? tx.received_at : tx.timestamp}, {"walletconflicts", conflicts},
            {"hex", bytes_to_hex_string(tx.tx.serialize())}, {"details", json::array()}};
        if (debit > 0) { result["fee"] = -static_cast<double>(fee) / COIN; result["fee_angles"] = -fee; }
        if (tx.is_coinbase) result["generated"] = true;
        if (conf > 0) {
            result["blockhash"] = hash256_to_hex_string(tx.block_hash);
            result["blockheight"] = tx.block_height; result["blockindex"] = tx.block_index;
            result["blocktime"] = tx.block_time;
        } else result["trusted"] = false;
        for (uint32_t vout = 0; vout < tx.tx.outputs.size(); ++vout) {
            const auto& output = tx.tx.outputs[vout];
            if (output.value == 0) continue;
            const bool mine = owned.contains(output.address);
            const bool watchonly = mine && !spendable.contains(output.address);
            const bool change = std::find(tx.change_outputs.begin(), tx.change_outputs.end(), vout) != tx.change_outputs.end() ||
                (tx.change_outputs.empty() && debit > 0 && input_addresses.contains(output.address));
            if (change && !include_change) continue;
            const auto add = [&](std::string category, Amount amount) {
                json detail{{"address", output.address.str()}, {"category", category}, {"vout", vout},
                    {"amount", static_cast<double>(amount) / COIN}, {"amount_angles", amount}, {"label", ""}};
                if (watchonly) detail["involvesWatchonly"] = true;
                if (category == "send") { detail["fee"] = -static_cast<double>(fee) / COIN; detail["fee_angles"] = -fee; }
                result["details"].push_back(std::move(detail));
            };
            if (debit > 0 && !change) add("send", -output.value);
            if (mine && (include_watchonly || !watchonly)) {
                const auto category = tx.is_coinbase ? (conf <= 0 ? "orphan" :
                    (conf < static_cast<int64_t>(consensus::params::COINBASE_MATURITY) ? "immature" : "generate")) : "receive";
                add(category, output.value);
            }
        }
        return result;
    }

    json entries(const wallet::WalletTransaction& tx, bool include_watchonly, bool include_change = false) const {
        auto summary = transaction(tx, include_watchonly, include_change);
        const auto details = summary["details"];
        summary.erase("details"); summary.erase("hex"); summary.erase("amount"); summary.erase("amount_angles");
        summary.erase("fee"); summary.erase("fee_angles");
        json result = json::array();
        for (const auto& detail : details) {
            auto entry = summary;
            entry.update(detail);
            result.push_back(std::move(entry));
        }
        return result;
    }
};
} // namespace

void RPCServer::synchronize_wallet() {
    auto& wallet = require_wallet();
    if (!chain_state_ || wallet.get_key_count() == 0) return;
    const auto pending = pending_mempool_transactions(mempool_.get());
    const auto scan = wallet.synchronize(chain_state_, pending);
    if (!scan.complete())
        throw RPCException(RPC_WALLET_ERROR, "Wallet history is incomplete: " + scan.error);
}

void RPCServer::register_wallet_exchange_methods() {
    register_method({.name = "estimatesmartfee", .method = [this](const json& params) -> json {
        require_min_params(params, 1, "conf_target is required");
        const auto target = optional_bounded_size_param(params, 0, 0, 1, 1008, "conf_target");
        const auto mode = optional_param<std::string>(params, 1, "conservative");
        if (mode != "economical" && mode != "conservative" && mode != "unset")
            throw RPCException(RPC_INVALID_PARAMETER, "estimate_mode must be economical or conservative");
        const auto estimate = require_mempool().fee_estimator().estimate(target, mode != "economical");
        json result{{"blocks", target}, {"fee_rate_unit", "GC_per_1000_serialized_bytes"}, {"observations", estimate.observations}};
        if (estimate.rate_per_kb) {
            result["feerate"] = static_cast<double>(*estimate.rate_per_kb) / COIN;
            result["feerate_angles_per_kb"] = *estimate.rate_per_kb;
        } else result["errors"] = json::array({"Insufficient observed transactions and blocks for this confirmation target"});
        return result;
    }, .category = "mempool", .help = "estimatesmartfee conf_target ( estimate_mode )\nObserved confirmation-target fee estimate in GC per 1000 serialized bytes (no Bitcoin witness discount). Returns errors and no feerate until enough observations exist. Modes: conservative, economical.",
        .required_role = RPCAuthRole::READONLY, .param_names = {"conf_target", "estimate_mode"}});

    register_method({.name = "listtransactions", .method = [this](const json& params) {
        const auto account = optional_param<std::string>(params, 0, "*");
        const auto count = optional_bounded_size_param(params, 1, 10, 0, 1000, "count");
        const auto skip = optional_bounded_size_param(params, 2, 0, 0, 100000, "skip");
        const bool watchonly = optional_param<bool>(params, 3, true);
        if (count == 0) return json::array();
        synchronize_wallet();
        HistoryView view(require_wallet(), require_chain_state(), require_blockchain());
        json entries = json::array();
        for (const auto& tx : view.transactions) for (auto& entry : view.entries(tx, watchonly)) {
            if (account == "*" || entry["address"] == account) entries.push_back(std::move(entry));
        }
        json result = json::array();
        const auto end = skip >= entries.size() ? 0 : entries.size() - skip;
        const auto begin = count >= end ? 0 : end - count;
        for (auto i = begin; i < end; ++i) result.push_back(std::move(entries[i]));
        view.validate(); return result;
    }, .category = "wallet", .help = "listtransactions ( account count skip include_watchonly )\nRecent wallet output entries, signed send amounts, fees and block metadata. count max 1000; skip max 100000. account accepts * or a wallet address.",
        .requires_wallet = true, .param_names = {"account", "count", "skip", "include_watchonly"}});

    register_method({.name = "gettransaction", .method = [this](const json& params) {
        const auto txid = required_hash(params, 0);
        const bool watchonly = optional_param<bool>(params, 1, false);
        const bool verbose = optional_param<bool>(params, 2, false);
        synchronize_wallet();
        HistoryView view(require_wallet(), require_chain_state(), require_blockchain());
        const auto found = view.index.find(txid);
        if (found == view.index.end()) throw RPCException(RPC_INVALID_ADDRESS_OR_KEY, "Transaction does not belong to this wallet");
        const auto& tx = view.transactions[found->second];
        auto result = view.transaction(tx, watchonly);
        if (verbose) result["decoded"] = transaction_to_verbose_json(tx.tx, txid, hash256_to_hex_string(txid), require_chain_state());
        view.validate(); return result;
    }, .category = "wallet", .help = "gettransaction txid ( include_watchonly verbose )\nDetailed wallet transaction, signed amounts, output details and active-chain confirmations.",
        .requires_wallet = true, .param_names = {"txid", "include_watchonly", "verbose"}});

    register_method({.name = "listsinceblock", .method = [this](const json& params) {
        const bool has_anchor = has_param(params, 0) && params[0] != "";
        const auto anchor = has_anchor ? required_hash(params, 0) : Hash256{};
        const auto target = optional_bounded_size_param(params, 1, 1, 1, 1000000, "target_confirmations");
        const bool watchonly = optional_param<bool>(params, 2, false);
        const bool include_removed = optional_param<bool>(params, 3, true);
        const bool include_change = optional_param<bool>(params, 4, false);
        synchronize_wallet();
        HistoryView view(require_wallet(), require_chain_state(), require_blockchain());
        const auto fork = has_anchor ? view.chain.get_active_fork_height(anchor) : std::optional<Height>{};
        if (has_anchor && !fork) throw RPCException(RPC_INVALID_ADDRESS_OR_KEY, "Block is unknown or its history has been pruned");
        json result{{"transactions", json::array()}, {"lastblock", ""}};
        if (include_removed) result["removed"] = json::array();
        for (const auto& tx : view.transactions) {
            if (!has_anchor || view.confirmations(tx) <= 0 || tx.block_height > *fork)
                for (auto& entry : view.entries(tx, watchonly, include_change)) result["transactions"].push_back(std::move(entry));
            if (include_removed && has_anchor) {
                for (const auto& location : tx.removed_blocks) {
                    if (location.height <= *fork || !view.chain.is_block_ancestor(location.hash, anchor)) continue;
                    auto removed = tx;
                    removed.block_hash = location.hash; removed.block_height = location.height;
                    removed.block_index = location.index; removed.block_time = location.time;
                    for (auto& entry : view.entries(removed, watchonly, include_change)) {
                        entry["blockhash"] = hash256_to_hex_string(location.hash);
                        entry["blockheight"] = location.height;
                        result["removed"].push_back(std::move(entry));
                    }
                }
            }
        }
        const auto height = target > view.tip.height + 1 ? 0 : view.tip.height + 1 - target;
        Hash256 last{};
        if (!view.chain.get_active_block_hash_by_height(height, last))
            throw RPCException(RPC_WALLET_ERROR, "Requested wallet cursor has been pruned");
        result["lastblock"] = hash256_to_hex_string(last);
        view.validate(); return result;
    }, .category = "wallet", .help = "listsinceblock ( blockhash target_confirmations include_watchonly include_removed include_change )\nWallet output entries since a chain cursor, with removed entries after a reorganization. target_confirmations affects lastblock only. No entries are silently truncated.",
        .requires_wallet = true, .param_names = {"blockhash", "target_confirmations", "include_watchonly", "include_removed", "include_change"}});

    register_method({.name = "encryptwallet", .method = [this](const json& params) -> json {
        auto& wallet = require_wallet();
        if (wallet.has_password_protection()) throw RPCException(RPC_WALLET_WRONG_ENC_STATE, "Wallet is already encrypted");
        auto password = required_param(params, 0, "Passphrase is required").get<std::string>();
        utils::ScopedContainerCleanser guard(password);
        if (password.empty()) throw RPCException(RPC_INVALID_PARAMETER, "Passphrase must not be empty");
        if (!wallet.enable_vault(password)) throw RPCException(RPC_WALLET_ENCRYPTION_FAILED, "Wallet encryption failed");
        return "Wallet encrypted and locked; make a new backup";
    }, .category = "wallet", .help = "encryptwallet passphrase\nEncrypt private keys, save durably and lock the wallet. Public history remains available.",
        .requires_wallet = true, .param_names = {"passphrase"}, .mutates = true, .side_effects = "updates local wallet state"});

    register_method({.name = "walletpassphrase", .method = [this](const json& params) -> json {
        require_min_params(params, 2, "passphrase and timeout are required");
        auto& wallet = require_wallet();
        if (!wallet.has_password_protection()) throw RPCException(RPC_WALLET_WRONG_ENC_STATE, "Wallet is not encrypted");
        auto password = required_param(params, 0, "Passphrase is required").get<std::string>();
        utils::ScopedContainerCleanser guard(password);
        const auto timeout = optional_bounded_size_param(params, 1, 0, 1, 86400, "timeout");
        if ((!wallet.is_vault() && !wallet.enable_vault(password)) || !wallet.unlock(password, timeout)) {
            if (wallet.verify_password(password))
                throw RPCException(RPC_WALLET_ERROR, "The private-key vault could not be opened or persisted");
            throw RPCException(RPC_WALLET_PASSPHRASE_INCORRECT, "Incorrect wallet passphrase");
        }
        return nullptr;
    }, .category = "wallet", .help = "walletpassphrase passphrase timeout\nUnlock private keys for 1..86400 seconds. Repeated calls replace the deadline.",
        .requires_wallet = true, .param_names = {"passphrase", "timeout"}, .mutates = true, .side_effects = "updates local wallet state"});

    register_method({.name = "walletlock", .method = [this](const json&) -> json {
        auto& wallet = require_wallet();
        if (!wallet.is_vault()) throw RPCException(RPC_WALLET_WRONG_ENC_STATE, "Use walletpassphrase or encryptwallet to enable the key vault first");
        wallet.lock(); return nullptr;
    }, .category = "wallet", .help = "walletlock\nClear decrypted wallet keys immediately.", .requires_wallet = true, .mutates = true, .side_effects = "updates local wallet state"});

    register_method({.name = "walletpassphrasechange", .method = [this](const json& params) -> json {
        auto& wallet = require_wallet();
        if (!wallet.has_password_protection()) throw RPCException(RPC_WALLET_WRONG_ENC_STATE, "Wallet is not encrypted");
        auto old_password = required_param(params, 0, "Old passphrase is required").get<std::string>();
        auto new_password = required_param(params, 1, "New passphrase is required").get<std::string>();
        utils::ScopedContainerCleanser old_guard(old_password), new_guard(new_password);
        if (new_password.empty()) throw RPCException(RPC_INVALID_PARAMETER, "New passphrase must not be empty");
        if (!wallet.verify_password(old_password)) throw RPCException(RPC_WALLET_PASSPHRASE_INCORRECT, "Incorrect wallet passphrase");
        if (!wallet.change_password(wallet.get_file_path(), old_password, new_password))
            throw RPCException(RPC_WALLET_ERROR, "Could not persist the new wallet passphrase");
        return nullptr;
    }, .category = "wallet", .help = "walletpassphrasechange oldpassphrase newpassphrase\nAtomically replace the private-key passphrase.",
        .requires_wallet = true, .param_names = {"oldpassphrase", "newpassphrase"}, .mutates = true, .side_effects = "updates local wallet state"});

    register_method({.name = "getwalletinfo", .method = [this](const json&) -> json {
        auto& wallet = require_wallet();
        std::string scan_error;
        if (!wallet.is_scanning()) {
            try { synchronize_wallet(); } catch (const RPCException& error) { scan_error = error.what(); }
        }
        Amount confirmed = 0, unconfirmed = 0, immature = 0;
        const auto pending = pending_mempool_transactions(mempool_.get());
        for (const auto& address : wallet.get_spendable_addresses()) {
            const auto funds = wallet::classify_address_funds(require_chain_state(), address, pending);
            if (!funds.valid) throw RPCException(RPC_WALLET_ERROR, "Invalid wallet balance");
            confirmed = checked_add_amount_or_throw(confirmed, funds.available, "Balance overflow");
            unconfirmed = checked_add_amount_or_throw(unconfirmed, funds.unconfirmed, "Balance overflow");
            immature = checked_add_amount_or_throw(immature, funds.immature, "Balance overflow");
        }
        const auto scan = wallet.get_last_scan_result();
        return json{{"walletname", "default"}, {"walletversion", wallet.is_vault() ? 1 : 13},
            {"walletformat", wallet.is_vault() ? "vault-v1" : "legacy-v13"}, {"encrypted", wallet.has_password_protection()},
            {"locked", wallet.is_locked()}, {"unlocked_until", wallet.unlocked_until()},
            {"balance", static_cast<double>(confirmed) / COIN}, {"unconfirmed_balance", static_cast<double>(unconfirmed) / COIN},
            {"immature_balance", static_cast<double>(immature) / COIN}, {"balance_angles", confirmed},
            {"txcount", wallet.get_transactions().size()}, {"private_keys_enabled", true},
            {"scanning", wallet.is_scanning()}, {"scan_complete", scan_error.empty() && scan.complete() &&
                wallet.get_last_scanned_block_hash() == require_chain_state().get_tip_hash()}, {"scan_error", scan_error},
            {"lastprocessedblock", {{"height", wallet.get_last_scanned_height()}, {"hash", hash256_to_hex_string(wallet.get_last_scanned_block_hash())}}}};
    }, .category = "wallet", .help = "getwalletinfo\nWallet balances, encryption/lock state and last processed block.", .requires_wallet = true});

    register_method({.name = "backupwallet", .method = [this](const json& params) -> json {
        synchronize_wallet();
        const auto path = required_param(params, 0, "Backup destination is required").get<std::string>();
        if (path.empty() || !require_wallet().backup(path))
            throw RPCException(RPC_WALLET_ERROR, "Backup failed; destination must be a new file and wallet must use the encrypted key vault");
        return nullptr;
    }, .category = "wallet", .help = "backupwallet destination\nWrite a consistent encrypted wallet backup to a new file, including while locked.",
        .requires_wallet = true, .param_names = {"destination"}, .mutates = true, .side_effects = "updates local wallet state"});

    register_method({.name = "restorewallet", .method = [this](const json& params) -> json {
        auto& wallet = require_wallet();
        if (wallet.get_key_count() != 0 || std::filesystem::exists(wallet.get_file_path()))
            throw RPCException(RPC_WALLET_ERROR, "Restore requires an empty wallet and an unused wallet path");
        const auto source = required_param(params, 0, "Backup file is required").get<std::string>();
        wallet::Wallet candidate;
        if (!candidate.load_from_file(source, "") || !candidate.is_vault() || !candidate.backup(wallet.get_file_path()))
            throw RPCException(RPC_WALLET_ERROR, "Could not restore encrypted wallet backup");
        if (!wallet.load_from_file(wallet.get_file_path(), ""))
            throw RPCException(RPC_WALLET_ERROR, "Restored wallet could not be opened");
        synchronize_wallet();
        return json{{"name", "default"}, {"locked", true}};
    }, .category = "wallet", .help = "restorewallet backupfile\nRestore into an empty single-wallet node. Existing wallets are never overwritten. The restored wallet starts locked.",
        .requires_wallet = true, .param_names = {"backupfile"}, .mutates = true, .side_effects = "updates local wallet state"});

    register_method({.name = "rescanblockchain", .method = [this](const json& params) -> json {
        const auto start = optional_u64_param(params, 0, 0, "Invalid start height");
        if (start > require_chain_state().get_height()) throw RPCException(RPC_INVALID_PARAMETER, "Start height exceeds the chain tip");
        const auto result = require_wallet().reconcile_blockchain(chain_state_, start);
        if (!result.complete()) throw RPCException(RPC_WALLET_ERROR, "Rescan incomplete: " + result.error);
        if (require_wallet().is_vault() && !require_wallet().save("")) throw RPCException(RPC_WALLET_ERROR, "Rescan could not be saved");
        return json{{"start_height", start}, {"stop_height", result.target_height}};
    }, .category = "wallet", .help = "rescanblockchain ( start_height )\nRescan available active-chain history. Missing/pruned blocks produce an explicit error.",
        .requires_wallet = true, .param_names = {"start_height"}, .mutates = true, .side_effects = "updates local wallet state"});
}
} // namespace geocoin::rpc
