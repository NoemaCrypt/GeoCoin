#include "chain_test_fixture.hpp"
#include "geocoin/node/chain_coordinator.hpp"
#include "geocoin/network/mempool.hpp"
#include "geocoin/network/fee_estimator.hpp"
#include "geocoin/rpc/rpc_server.hpp"
#include "geocoin/rpc/rpc_encoding.hpp"
#include "geocoin/wallet/wallet.hpp"
#include "geocoin/utils/bytes.hpp"
#include <fstream>
#include <iostream>
#include <thread>

namespace geocoin::rpc {
struct RPCServerTestAccess {
    static json call(RPCServer& server, const std::string& method, const json& params, RPCAuthRole role) {
        return server.handle_rpc_call({{"jsonrpc", "2.0"}, {"id", 1}, {"method", method}, {"params", params}}, role);
    }
};
}

namespace {
using namespace geocoin;
using namespace geocoin::test;
using json = rpc::json;
size_t checks = 0;
void check(bool value, const std::string& message) { ++checks; expect(value, message); }
std::string hex(const Hash256& value) { return utils::hash256_to_hex(value); }
json request(rpc::RPCServer& server, const std::string& method, const json& params = json::array(),
             rpc::RPCAuthRole role = rpc::RPCAuthRole::ADMIN) {
    return rpc::RPCServerTestAccess::call(server, method, params, role);
}
json result(rpc::RPCServer& server, const std::string& method, const json& params = json::array()) {
    const auto response = request(server, method, params);
    if (response.contains("error")) throw std::runtime_error(method + ": " + response["error"].dump());
    return response.at("result");
}
void error(rpc::RPCServer& server, const std::string& method, const json& params, int code) {
    const auto response = request(server, method, params);
    check(response.contains("error") && response["error"].value("code", 0) == code,
          method + " returns expected error: " + response.dump());
}

void test_vault(const std::filesystem::path& directory) {
    const auto path = (directory / "vault.dat").string();
    wallet::Wallet wallet;
    check(wallet.initialize(make_key(52)), "initialize key vault fixture");
    check(wallet.save_to_file(path, ""), "write initial private wallet file");
    check(wallet.enable_vault("correct horse"), "encrypt existing wallet into vault v1");
    check(wallet.is_locked() && wallet.has_password_protection(), "encryption starts locked");
    bytes private_key;
    check(!wallet.get_private_key(wallet.get_address(), private_key), "locked key accessor refuses secrets");
    check(wallet.export_private_key("correct horse").empty(), "password verification alone does not export a locked key");
    check(!wallet.unlock("wrong horse", 10) && wallet.is_locked(), "incorrect password does not unlock");
    check(!wallet.unlock("correct horse", 0) && !wallet.unlock("correct horse", 86401), "unlock timeout bounds");
    check(wallet.unlock("correct horse", 1), "unlock for one second");
    check(wallet.get_private_key(wallet.get_address(), private_key) && private_key == make_key(52), "unlocked key matches original");
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    check(wallet.is_locked() && wallet.unlocked_until() == 0 &&
        !wallet.get_private_key(wallet.get_address(), private_key), "timer clears access to decrypted keys");
    check(wallet.unlock("correct horse", 1) && wallet.unlock("correct horse", 10), "a repeated unlock replaces the deadline");
    const auto deadline = wallet.unlocked_until();
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    check(!wallet.is_locked() && wallet.unlocked_until() == deadline, "the old unlock deadline cannot close an extended session");
    check(!wallet.unlock("wrong horse", 1) && wallet.unlocked_until() == deadline, "wrong password does not change an existing session");
    wallet.lock();
    check(wallet.unlock("correct horse", 5), "unlock to generate deposit address");
    const auto next = wallet.create_new_key("second deposit");
    check(!next.empty() && wallet.save(""), "new independent key is durably sealed without keeping the password");
    wallet.lock();
    check(wallet.save(""), "locked public metadata can be saved");
    wallet::Wallet reopened;
    check(reopened.load_from_file(path, "") && reopened.is_locked() && reopened.get_key_count() == 2,
          "restart reads addresses without a password and keeps keys locked");
    check(reopened.unlock("correct horse", 2) && reopened.get_private_key(next, private_key), "restart recovers every encrypted key");
    reopened.lock();
    check(reopened.change_password(path, "correct horse", "new horse"), "change passphrase while locked");
    check(reopened.is_locked() && !reopened.verify_password("correct horse") && reopened.verify_password("new horse"),
          "passphrase change preserves lock state and retires old password");
    check(!std::filesystem::exists(path + ".bak"), "passphrase change removes backups under the old password");
    const auto backup = (directory / "vault-backup.dat").string();
    check(reopened.backup(backup), "consistent backup while locked");
    check(!reopened.backup(backup) && !reopened.backup(path), "backup never overwrites a file or the live wallet");
    wallet::Wallet restored;
    check(restored.load_from_file(backup, "") && restored.unlock("new horse", 1), "backup restores encrypted keys");
    const auto corrupted = (directory / "corrupt.dat").string();
    std::filesystem::copy_file(backup, corrupted);
    {
        std::fstream file(corrupted, std::ios::binary | std::ios::in | std::ios::out);
        file.seekp(20); file.put('!');
    }
    wallet::Wallet invalid;
    check(!invalid.load_from_file(corrupted, ""), "corrupted public wallet metadata is rejected");
    std::ifstream input(path, std::ios::binary);
    const std::string document((std::istreambuf_iterator<char>(input)), {});
    check(document.find(utils::bytes_to_hex(make_key(52))) == std::string::npos &&
          document.find("new horse") == std::string::npos, "vault file contains neither plaintext key nor password");
}

void test_fee_estimator(const std::filesystem::path& directory) {
    network::FeeEstimator estimator;
    const auto file = (directory / "fee-estimates.json").string();
    estimator.configure_storage(file, "test-genesis", 0, {});
    check(!estimator.estimate(2, true).rate_per_kb, "fresh estimator reports insufficient data");
    std::vector<Hash256> fast1, fast2, slow;
    for (uint8_t i = 1; i <= 60; ++i) {
        const auto txid = hash_from_seed(i);
        estimator.transaction_added(txid, i <= 40 ? 2000 : 1000, 1000, 0);
        if (i <= 20) fast1.push_back(txid); else if (i <= 40) fast2.push_back(txid); else slow.push_back(txid);
    }
    estimator.block_connected(hash_from_seed(91), 1, fast1);
    estimator.block_connected(hash_from_seed(92), 2, fast2);
    estimator.block_connected(hash_from_seed(93), 3, {});
    estimator.block_connected(hash_from_seed(94), 4, slow);
    check(estimator.estimate(2, true).rate_per_kb == 2000, "short target excludes slow lower-fee transactions");
    check(estimator.estimate(6, true).rate_per_kb == 1000, "longer target supports the observed lower fee");
    check(!estimator.estimate(0, true).rate_per_kb && !estimator.estimate(1009, true).rate_per_kb, "invalid fee targets have no estimate");
    network::FeeEstimator restored;
    restored.configure_storage(file, "test-genesis", 4, hash_from_seed(94));
    check(restored.estimate(2, true).rate_per_kb == 2000, "fee history survives restart at the same chain tip");
    network::FeeEstimator wrong_network;
    wrong_network.configure_storage(file, "other-genesis", 4, hash_from_seed(94));
    check(!wrong_network.estimate(2, true).rate_per_kb, "fee cache cannot cross network boundaries");
    for (uint8_t i = 100; i < 140; ++i) estimator.transaction_added(hash_from_seed(i), 2000, 1000, 4);
    estimator.block_connected(hash_from_seed(95), 5, {});
    estimator.block_connected(hash_from_seed(96), 6, {});
    check(!estimator.estimate(2, true).rate_per_kb, "waiting transactions prevent a falsely optimistic fee estimate");
    estimator.reorganize(0);
    check(!estimator.estimate(6, true).rate_per_kb, "reorg discards detached confirmation samples");
}

void test_exchange_rpc(const std::filesystem::path& directory) {
    TestChain fixture;
    core::Block funding;
    for (unsigned i = 0; i < 100; ++i) {
        auto block = fixture.make_block();
        fixture.append(block);
        if (i == 0) funding = block;
    }
    auto pool = std::make_shared<network::Mempool>(fixture.state);
    auto coordinator = std::make_shared<node::ChainCoordinator>(fixture.chain, pool);
    check(coordinator->start(), "start actual chain mutation service");
    auto wallet = std::make_shared<wallet::Wallet>();
    check(wallet->initialize(make_key(43)), "initialize exchange wallet");
    const auto wallet_path = (directory / "exchange.dat").string();
    check(wallet->save_to_file(wallet_path, "") && wallet->enable_vault("exchange pass"), "persist locked exchange wallet");
    check(wallet->unlock("exchange pass", 60), "unlock to issue second deposit address");
    const auto second_address = wallet->create_new_key("customer two");
    check(wallet->save("") && !second_address.empty(), "persist second deposit address");
    wallet->lock();
    rpc::RPCServer server;
    rpc::RPCServerContext context;
    context.wallet = wallet; context.blockchain = fixture.chain; context.chain_state = fixture.state;
    context.mempool = pool; context.chain_mutations = coordinator;
    check(server.set_context(context), "install exchange RPC context");
    check(result(server, "getwalletinfo")["locked"] == true, "wallet status is readable while locked");
    check(result(server, "estimatesmartfee", json::array({6})).contains("errors"), "fee RPC exposes insufficient observations");
    error(server, "estimatesmartfee", json::array({0}), rpc::RPC_INVALID_PARAMETER);
    error(server, "estimatesmartfee", json::array({6, "invalid"}), rpc::RPC_INVALID_PARAMETER);
    error(server, "gettransaction", json::array({"bad hash"}), rpc::RPC_INVALID_PARAMETER);
    error(server, "listsinceblock", json::array({hex(hash_from_seed(99))}), rpc::RPC_INVALID_ADDRESS_OR_KEY);
    const auto forbidden = request(server, "walletpassphrase", json::array({"exchange pass", 60}), rpc::RPCAuthRole::READONLY);
    check(forbidden.contains("error") && wallet->is_locked(), "readonly role cannot unlock the wallet");
    error(server, "walletpassphrase", json::array({"wrong", 60}), rpc::RPC_WALLET_PASSPHRASE_INCORRECT);
    error(server, "walletpassphrase", json::array({"exchange pass", 0}), rpc::RPC_INVALID_PARAMETER);

    core::Transaction deposit;
    deposit.version = 2; deposit.type = core::TransactionType::STANDARD;
    deposit.timestamp = fixture.chain->get_tip()->get_block_time() + 1;
    core::TxInput input; input.prev_tx_hash = funding.transactions.front().get_hash(); input.vout = 0;
    deposit.inputs.push_back(input);
    const Amount fee = 1000000;
    deposit.outputs.push_back({10 * COIN, wallet->get_address(), {}});
    deposit.outputs.push_back({2 * COIN, second_address, {}});
    deposit.outputs.push_back({funding.transactions.front().outputs[0].value - 12 * COIN - fee, address_from_seed(42), {}});
    deposit.sign_input(0, make_key(42));
    check(coordinator->admit_transaction(deposit).is_accepted(), "admit genuine signed multi-output deposit");
    const auto txid = hex(deposit.get_hash());
    const auto pending = result(server, "gettransaction", json::array({txid}));
    check(pending["confirmations"] == 0 && pending["details"].size() == 2 && pending["amount_angles"] == 12 * COIN,
          "locked wallet detects each pending deposit output");
    auto* fork = fixture.chain->get_tip();
    const auto cursor = hex(fork->get_block_hash());
    auto confirmed = fixture.make_block(nullptr, fee);
    confirmed.transactions.push_back(deposit); TestChain::seal(confirmed, fork);
    check(coordinator->process_block(confirmed).is_accepted(), "confirm deposit through production validation");
    const auto received = result(server, "gettransaction", json::array({txid, false, true}));
    check(received["confirmations"] == 1 && received["blockhash"] == hex(confirmed.get_hash()) &&
          received["blockindex"] == 1 && received.contains("decoded"), "gettransaction includes exact inclusion metadata");
    check(received["timereceived"] == pending["timereceived"], "confirming a deposit preserves its first observation time");
    {
        wallet::Wallet scanning;
        check(scanning.initialize(make_key(43)), "initialize concurrent history reader");
        std::atomic<bool> finished{false};
        wallet::WalletScanResult scan;
        std::exception_ptr failure;
        std::jthread worker([&] {
            try { scan = scanning.scan_blockchain(fixture.state, 0); }
            catch (...) { failure = std::current_exception(); }
            finished.store(true);
        });
        bool consistent = true;
        while (!finished.load()) {
            const auto snapshot = scanning.get_history_snapshot();
            for (const auto& tx : snapshot.transactions)
                if (tx.block_height > snapshot.scanned_height) consistent = false;
            std::this_thread::yield();
        }
        worker.join();
        if (failure) std::rethrow_exception(failure);
        const auto snapshot = scanning.get_history_snapshot();
        check(consistent && scan.complete() && snapshot.scanned_hash == confirmed.get_hash() &&
              snapshot.transactions.size() == 1, "concurrent readers receive history and its cursor as one consistent snapshot");
    }
    const auto changes = result(server, "listsinceblock", json::array({cursor}));
    check(changes["transactions"].size() == 2 && changes["lastblock"] == hex(confirmed.get_hash()),
          "listsinceblock exposes both deposit outputs and a continuation cursor");
    const auto repeated = result(server, "listsinceblock", json::array({cursor}));
    check(repeated == changes, "repeated polling has stable transaction/output identities");
    const auto delayed = result(server, "listsinceblock", json::array({cursor, 2}));
    check(delayed["transactions"].size() == 2 && delayed["lastblock"] == cursor,
          "target confirmations changes the cursor, never filters deposits");
    const auto newest_output = result(server, "listtransactions", json::array({"*", 1, 0}));
    const auto older_output = result(server, "listtransactions", json::array({"*", 1, 1}));
    check(newest_output.size() == 1 && older_output.size() == 1 && newest_output[0]["vout"] == 1 && older_output[0]["vout"] == 0,
          "listtransactions paginates output entries from the newest end");
    check(result(server, "listtransactions", json::array({"*", 0, 0})).empty(), "zero transaction page is empty");

    auto alternate = fixture.make_block(fork);
    auto* alternate_index = fixture.accept(alternate);
    auto replacement_tip = fixture.make_block(alternate_index);
    check(coordinator->process_block(replacement_tip).is_accepted(), "longer branch reorganizes the deposit block");
    const auto removed = result(server, "listsinceblock", json::array({hex(confirmed.get_hash())}));
    check(removed["removed"].size() == 2 && result(server, "gettransaction", json::array({txid}))["confirmations"] == 0,
          "reorg removes both deposit credits without losing transaction history");
    auto reconfirmed = fixture.make_block(nullptr, fee);
    reconfirmed.transactions.push_back(deposit); TestChain::seal(reconfirmed, fixture.chain->get_tip());
    check(coordinator->process_block(reconfirmed).is_accepted(), "reconfirm detached deposit");
    check(result(server, "gettransaction", json::array({txid}))["confirmations"] == 1, "deposit confirmations recover on new branch");

    auto reopened = std::make_shared<wallet::Wallet>();
    check(reopened->load_from_file(wallet_path, "") && reopened->is_locked(), "restart exchange wallet without a password");
    context.wallet = reopened;
    check(server.set_context(context), "replace stopped listener context for restart test");
    check(result(server, "gettransaction", json::array({txid}))["blockhash"] == hex(reconfirmed.get_hash()),
          "restart preserves the new deposit inclusion");
    check(result(server, "listsinceblock", json::array({hex(confirmed.get_hash())}))["removed"].size() == 2,
          "restart preserves detached-branch deposit records");
    const auto early_backup = (directory / "before-withdrawal.dat").string();
    result(server, "backupwallet", json::array({early_backup}));
    const auto destination = address_from_seed(44).str();
    error(server, "sendtoaddress", json::array({destination, 1.0}), rpc::RPC_WALLET_UNLOCK_NEEDED);
    result(server, "walletpassphrase", json::array({"exchange pass", 60}));
    const auto rpc_address = result(server, "getnewaddress", json::array({"RPC deposit"}));
    check(rpc_address.is_string() && rpc_address != second_address.str(), "unlocked RPC creates and persists a new deposit address");
    const auto withdrawal = result(server, "sendtoaddress", json::array({destination, 1.0})).get<std::string>();
    const auto sent = result(server, "gettransaction", json::array({withdrawal}));
    check(sent["amount"] == -1.0 && sent["fee"].get<double>() < 0 && sent["details"][0]["category"] == "send",
          "withdrawal reports negative sent amount and a separate negative fee");
    check(sent["details"].size() == 1, "change is excluded from deposit/withdrawal details");
    network::MempoolEntry withdrawal_entry;
    Hash256 withdrawal_hash{}; utils::hex_to_hash256_checked(withdrawal, withdrawal_hash);
    check(pool->get_entry(withdrawal_hash, withdrawal_entry), "lookup accepted withdrawal fee");
    auto withdrawal_block = fixture.make_block(nullptr, withdrawal_entry.fee);
    withdrawal_block.transactions.push_back(withdrawal_entry.tx); TestChain::seal(withdrawal_block, fixture.chain->get_tip());
    check(coordinator->process_block(withdrawal_block).is_accepted(), "confirm the withdrawal");
    auto cold_restored = std::make_shared<wallet::Wallet>();
    check(cold_restored->load_from_file(early_backup, ""), "open a backup predating the withdrawal");
    context.wallet = cold_restored; check(server.set_context(context), "install older backup for change reconstruction");
    const auto reconstructed = result(server, "gettransaction", json::array({withdrawal}));
    check(reconstructed["details"].size() == 1 && reconstructed["details"][0]["category"] == "send" && reconstructed["amount"] == -1.0,
          "restoring an older backup never credits a reconstructed withdrawal's change as a deposit");
    context.wallet = reopened; check(server.set_context(context), "restore live exchange context");
    result(server, "walletlock");
    const auto backup = (directory / "exchange-backup.dat").string();
    result(server, "backupwallet", json::array({backup}));
    wallet::Wallet backed_up;
    check(backed_up.load_from_file(backup, "") && backed_up.is_locked(), "RPC backup restores in locked state");
    wallet::WalletTransaction persisted;
    check(backed_up.get_transaction(withdrawal_hash, persisted) && persisted.broadcast,
          "acknowledged withdrawal and rebroadcast intent survive a backup");

    // An unrelated replacement pays no wallet address. Its conflicting input
    // must still invalidate the original deposit and its dependent withdrawal.
    auto conflict = deposit;
    conflict.outputs = {{funding.transactions.front().outputs[0].value - fee, address_from_seed(45), {}}};
    conflict.sign_input(0, make_key(42));
    auto conflicting_block = fixture.make_block(fork, fee);
    conflicting_block.transactions.push_back(conflict); TestChain::seal(conflicting_block, fork);
    auto* conflicting_index = fixture.accept(conflicting_block);
    for (unsigned i = 0; i < 3; ++i) conflicting_index = fixture.accept(fixture.make_block(conflicting_index));
    auto conflicting_tip = fixture.make_block(conflicting_index);
    check(coordinator->process_block(conflicting_tip).is_accepted(), "activate branch with external conflicting spend");
    const auto conflicted = result(server, "gettransaction", json::array({txid}));
    check(conflicted["confirmations"].get<int64_t>() < 0 &&
        std::find(conflicted["walletconflicts"].begin(), conflicted["walletconflicts"].end(), json(hex(conflict.get_hash()))) != conflicted["walletconflicts"].end(),
        "external double spend produces negative deposit confirmations and its txid");
    check(result(server, "gettransaction", json::array({withdrawal}))["confirmations"].get<int64_t>() < 0,
          "a withdrawal inherits a confirmed conflict in its deposit ancestry");

    const auto old_default = wallet::Wallet::get_default_wallet_path();
    struct DefaultPathGuard { std::string path; ~DefaultPathGuard() { wallet::Wallet::set_default_wallet_path(path); } } default_guard{old_default};
    const auto restored_path = (directory / "rpc-restored.dat").string();
    wallet::Wallet::set_default_wallet_path(restored_path);
    auto restored_wallet = std::make_shared<wallet::Wallet>();
    context.wallet = restored_wallet;
    check(server.set_context(context), "install an empty wallet for restore");
    const auto restored_result = result(server, "restorewallet", json::array({backup}));
    check(restored_result["locked"] == true && restored_wallet->is_locked() && std::filesystem::exists(restored_path),
          "restorewallet creates a new durable locked wallet");
    check(result(server, "gettransaction", json::array({txid}))["confirmations"].get<int64_t>() < 0,
          "restoring an older backup reconciles later chain reorganizations");
    error(server, "restorewallet", json::array({backup}), rpc::RPC_WALLET_ERROR);
    result(server, "rescanblockchain", json::array({0}));
    error(server, "rescanblockchain", json::array({999999}), rpc::RPC_INVALID_PARAMETER);

    wallet::Wallet legacy;
    check(legacy.initialize(make_key(52)), "initialize legacy-format history roundtrip");
    wallet::WalletTransaction historical;
    check(restored_wallet->get_transaction(deposit.get_hash(), historical), "read location/conflict history for roundtrip");
    legacy.add_transaction(historical);
    const auto legacy_path = (directory / "legacy-history.dat").string();
    check(legacy.save_to_file(legacy_path, "legacy pass"), "save history extension in existing wallet format");
    wallet::Wallet legacy_reopened;
    check(legacy_reopened.load_from_file(legacy_path, "legacy pass") && legacy_reopened.get_transaction(deposit.get_hash(), historical) &&
          historical.removed_blocks.size() >= 2 && !historical.conflicts.empty(),
          "existing wallet serialization retains detached blocks and external conflicts");

    auto plaintext = std::make_shared<wallet::Wallet>();
    const auto plain_path = (directory / "rpc-encrypt.dat").string();
    check(plaintext->initialize(make_key(54)) && plaintext->save_to_file(plain_path, ""), "prepare RPC encryption fixture");
    context.wallet = plaintext; check(server.set_context(context), "install plaintext wallet");
    result(server, "encryptwallet", json::array({"RPC pass"}));
    check(plaintext->is_locked() && plaintext->is_vault(), "encryptwallet seals keys and closes the wallet");
    error(server, "encryptwallet", json::array({"other pass"}), rpc::RPC_WALLET_WRONG_ENC_STATE);
    error(server, "walletpassphrasechange", json::array({"wrong pass", "new RPC pass"}), rpc::RPC_WALLET_PASSPHRASE_INCORRECT);
    result(server, "walletpassphrasechange", json::array({"RPC pass", "new RPC pass"}));
    result(server, "walletpassphrase", json::array({"new RPC pass", 1}));
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    check(result(server, "getwalletinfo")["locked"] == true, "RPC unlock expires without another signing request");
    error(server, "walletpassphrase", json::array({"RPC pass", 60}), rpc::RPC_WALLET_PASSPHRASE_INCORRECT);
    coordinator->stop();
}
} // namespace

int main() {
    const auto directory = geocoin::test::make_temp_dir("geocoin-exchange-wallet");
    try {
        geocoin::consensus::SelectParams(geocoin::consensus::Network::LOADNET);
        test_vault(directory);
        test_fee_estimator(directory);
        test_exchange_rpc(directory);
        std::filesystem::remove_all(directory);
        std::cout << "test_exchange_wallet: " << checks << " checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "test_exchange_wallet failed after " << checks << " checks: " << error.what() << '\n';
        std::filesystem::remove_all(directory);
        return 1;
    }
}
