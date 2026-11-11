#include "geocoin/consensus/consensus.hpp"
#include "geocoin/consensus/proof_of_work.hpp"
#include "geocoin/core/block.hpp"
#include "geocoin/crypto/addresses.hpp"
#include "geocoin/crypto/geo_area.hpp"
#include "geocoin/crypto/signatures.hpp"
#include "geocoin/state/state.hpp"
#include "geocoin/storage/block_storage.hpp"
#include "geocoin/wallet/multisig.hpp"
#include "geocoin/wallet/wallet_application_service.hpp"
#include "geocoin/wallet/wallet.hpp"
#include "wallet_state_fixture.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/stat.h>
#endif

namespace {

void expect(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

geocoin::bytes make_key(uint8_t seed)
{
    geocoin::bytes key(32);
    for (size_t i = 0; i < key.size(); ++i) {
        key[i] = static_cast<uint8_t>(seed + i);
    }
    return key;
}

geocoin::Hash256 hash_from_seed(uint8_t seed)
{
    geocoin::Hash256 hash{};
    for (size_t i = 0; i < hash.size(); ++i) {
        hash[i] = static_cast<uint8_t>(seed + i * 13);
    }
    return hash;
}

void append_u32_le(geocoin::bytes& out, uint32_t value)
{
    out.push_back(static_cast<geocoin::byte>(value & 0xFFu));
    out.push_back(static_cast<geocoin::byte>((value >> 8) & 0xFFu));
    out.push_back(static_cast<geocoin::byte>((value >> 16) & 0xFFu));
    out.push_back(static_cast<geocoin::byte>((value >> 24) & 0xFFu));
}

void append_u64_le(geocoin::bytes& out, uint64_t value)
{
    for (int i = 0; i < 8; ++i) {
        out.push_back(static_cast<geocoin::byte>((value >> (i * 8)) & 0xFFu));
    }
}

void append_i64_le(geocoin::bytes& out, int64_t value)
{
    append_u64_le(out, static_cast<uint64_t>(value));
}

void write_bytes(const std::filesystem::path& path, const geocoin::bytes& data)
{
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    expect(static_cast<bool>(file), "test fixture writes bytes");
}

geocoin::bytes make_wallet_file_body(uint32_t magic, uint32_t version, uint8_t encrypted_flag, const geocoin::bytes& payload)
{
    geocoin::bytes data;
    append_u32_le(data, magic);
    append_u32_le(data, version);
    data.push_back(encrypted_flag);
    append_u32_le(data, static_cast<uint32_t>(payload.size()));
    data.insert(data.end(), payload.begin(), payload.end());
    return data;
}

geocoin::bytes make_minimal_plain_wallet_payload()
{
    geocoin::bytes payload;
    payload.push_back(0); // no password verifier
    append_u32_le(payload, 0); // key_count
    append_u32_le(payload, 0); // tx_count
    append_u64_le(payload, 0); // last_scanned_height
    payload.insert(payload.end(), 32, 0); // scan block hash
    append_u32_le(payload, 0); // multisig_count
    append_u32_le(payload, 0); // primary address length
    return payload;
}

std::filesystem::path make_temp_dir(const std::string& name)
{
    const auto path = std::filesystem::temp_directory_path()
        / (name + "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::remove_all(path);
    std::filesystem::create_directories(path);
    return path;
}

std::shared_ptr<geocoin::state::ChainState> make_chain_state()
{
    auto chain_state = std::make_shared<geocoin::state::ChainState>(
        std::make_unique<geocoin::state::MemoryStorage>());
    chain_state->set_height(200);
    chain_state->set_tip_hash(hash_from_seed(0xfe));
    return chain_state;
}

class RecordingMutationService final : public geocoin::services::ChainMutationService {
public:
    geocoin::services::BlockSubmissionResult process_block(
        const geocoin::core::Block&
    ) override {
        return {};
    }

    geocoin::services::ChainTipActivationResult activate_tip(
        const geocoin::Hash256& tip_hash
    ) override {
        geocoin::services::ChainTipActivationResult result;
        result.requested_tip_hash = tip_hash;
        return result;
    }

    geocoin::services::TransactionSubmissionResult admit_transaction(
        const geocoin::core::Transaction& transaction
    ) override {
        ++submissions;
        last_txid = transaction.get_hash();
        geocoin::services::TransactionSubmissionResult result;
        result.txid = last_txid;
        result.outcome = accept
            ? geocoin::services::SubmissionOutcome::ACCEPTED
            : geocoin::services::SubmissionOutcome::REJECTED;
        if (!accept) {
            result.reject_reason = "test rejection";
        }
        return result;
    }

    bool accept = true;
    size_t submissions = 0;
    geocoin::Hash256 last_txid{};
};

void test_wallet_application_service_boundaries()
{
    auto mutations = std::make_shared<RecordingMutationService>();
    geocoin::wallet::WalletApplicationService submission_service(
        nullptr, nullptr, mutations);
    geocoin::core::Transaction transaction;
    transaction.timestamp = 1710000000;

    auto submitted = submission_service.submit(transaction);
    expect(submitted.succeeded() && mutations->submissions == 1,
           "wallet application service submits through the mutation port");
    expect(submitted.txid == transaction.get_hash(),
           "wallet application service returns submitted txid");

    mutations->accept = false;
    submitted = submission_service.submit(transaction);
    expect(submitted.error == geocoin::wallet::WalletUseCaseError::SUBMISSION_REJECTED &&
               submitted.detail == "test rejection",
           "wallet application service preserves mutation rejection details");

    geocoin::wallet::WalletApplicationService offline_service(nullptr, nullptr, nullptr);
    expect(offline_service.submit(transaction).error ==
               geocoin::wallet::WalletUseCaseError::SUBMISSION_UNAVAILABLE,
           "wallet application service rejects implicit offline commits");

    auto wallet = std::make_shared<geocoin::wallet::Wallet>();
    const auto first_key = make_key(0x71);
    const auto second_key = make_key(0x81);
    geocoin::Address first_address;
    geocoin::Address second_address;
    expect(geocoin::crypto::compute_address_from_private_key(first_key, first_address) &&
               geocoin::crypto::compute_address_from_private_key(second_key, second_address),
           "wallet service fixture derives multisig addresses");
    const auto canonical = geocoin::wallet::create_multisig_address(
        1, 2, {first_address, second_address});
    auto config = canonical.config;
    config.address = geocoin::Address(canonical.address);
    config.participants.emplace_back(first_address);
    config.participants.emplace_back(second_address);

    geocoin::wallet::WalletApplicationService metadata_service(
        wallet, nullptr, nullptr);
    std::string error;
    expect(metadata_service.remember_multisig_config(config, error),
           "wallet application service stores multisig metadata in wallet");
    expect(wallet->get_multisig_addresses().size() == 1,
           "multisig metadata does not require ChainState mutation");
    auto chain_state = std::make_shared<geocoin::state::ChainState>(std::make_unique<geocoin::state::MemoryStorage>());
    geocoin::wallet::WalletApplicationService geometry_service(wallet, chain_state, nullptr);
    geocoin::nft::NFTMetadata empty_geometry;
    empty_geometry.geometry.polygons.clear();
    expect(geometry_service.create_nft(empty_geometry, 0, geocoin::consensus::params::MIN_TX_FEE).error ==
               geocoin::wallet::WalletUseCaseError::INVALID_ARGUMENT,
           "wallet service rejects empty multipart geometry before accessing its first part");
}

void fund_address(
    const std::shared_ptr<geocoin::state::ChainState>& chain_state,
    geocoin::Height height,
    geocoin::Amount value,
    const geocoin::Address& address
)
{
    geocoin::core::Transaction coinbase = geocoin::core::Transaction::create_coinbase(
        height,
        address,
        value
    );
    expect(chain_state->apply_transaction(coinbase, height, true), "coinbase funding applies");
}

void finalize_block(geocoin::core::Block& block, geocoin::Height height, const geocoin::Hash256& prev_hash)
{
    block.header.version = 1;
    block.header.prev_block_hash = prev_hash;
    block.header.timestamp = 1000 + static_cast<uint32_t>(height);
    block.header.bits = geocoin::consensus::ProofOfWork::get_initial_bits();
    block.header.geo_tag = static_cast<uint64_t>(height);
    block.header.nonce = static_cast<uint64_t>(height);
    block.header.merkle_root = block.calculate_merkle_root();
    block.header.witness_commitment = block.calculate_witness_commitment();
}

geocoin::core::Transaction make_manual_spend(
    const geocoin::core::Transaction& prev_tx,
    const geocoin::Address& wallet_address,
    const geocoin::Address& recipient,
    geocoin::Amount amount,
    geocoin::Amount fee
)
{
    expect(!prev_tx.outputs.empty(), "manual spend source has output");
    expect(prev_tx.outputs[0].value > amount + fee, "manual spend source can fund payment");

    geocoin::core::Transaction tx;
    tx.version = 2;
    tx.type = geocoin::core::TransactionType::STANDARD;
    tx.timestamp = 2000;

    geocoin::core::TxInput input;
    input.prev_tx_hash = prev_tx.get_hash();
    input.vout = 0;
    tx.inputs.push_back(input);

    geocoin::core::TxOutput payment;
    payment.value = amount;
    payment.address = recipient;
    tx.outputs.push_back(payment);

    geocoin::core::TxOutput change;
    change.value = prev_tx.outputs[0].value - amount - fee;
    change.address = wallet_address;
    tx.outputs.push_back(change);
    return tx;
}

geocoin::nft::NFTMetadata make_test_metadata()
{
    geocoin::nft::NFTMetadata metadata;
    metadata.name = "Wallet NFT";
    metadata.description = "Wallet NFT builder coverage";
    metadata.image_uri = "ipfs://wallet-builder";
    metadata.content_hash = std::string(64, 'a');
    metadata.geometry.polygons.front().exterior = {
        {55750000000LL, 37610000000LL},
        {55750100000LL, 37610100000LL},
        {55749900000LL, 37610200000LL},
    };
    metadata.area_bits = 0;
    metadata.created_at = 123456;
    metadata.block_height = 0;
    metadata.location_name = "wallet-test";
    metadata.tags = {"wallet", "nft"};
    metadata.creator = "wallet-builder";
    return metadata;
}

void test_key_management_and_multisig()
{
    geocoin::wallet::Wallet wallet;
    expect(!wallet.initialize(geocoin::bytes{1, 2, 3}), "wallet rejects invalid private key size");
    expect(wallet.initialize(make_key(1)), "wallet initializes with valid key");
    const geocoin::Address primary = wallet.get_address();
    expect(!primary.empty(), "wallet exposes primary address");
    expect(wallet.has_key(primary), "wallet has primary key");

    geocoin::wallet::KeyInfo info;
    expect(wallet.get_key_info(primary, info), "wallet returns key info");
    expect(info.address == primary, "key info address matches primary");

    geocoin::bytes private_key;
    expect(wallet.get_private_key(primary, private_key), "wallet returns primary private key");
    expect(private_key == make_key(1), "primary private key matches seed");

    const geocoin::Address generated = wallet.create_new_key("generated");
    expect(wallet.has_key(generated), "create_new_key stores spendable key");
    expect(wallet.get_key_count() == 2, "wallet tracks generated key");
    expect(wallet.get_address() == primary, "get_address stays on the initialized key after create_new_key");

    expect(!wallet.import_private_key(geocoin::bytes{4, 5}, "bad"), "import rejects bad key size");
    expect(wallet.import_private_key(make_key(33), "imported"), "import accepts valid key");
    expect(!wallet.import_private_key(make_key(33), "duplicate"), "import rejects duplicate private key");
    expect(wallet.get_key_count() == 3, "wallet tracks imported key");
    expect(wallet.get_address() == primary, "get_address stays on the initialized key after import");
    expect(wallet.get_spendable_addresses().size() == 3, "spendable addresses include all keys");
    expect(wallet.get_watch_only_addresses().empty(), "fresh wallet has no watch-only addresses");

    const std::vector<geocoin::Address> participants{primary, generated};
    auto multisig = geocoin::wallet::create_multisig_address(1, 2, participants);
    multisig.config.address = geocoin::Address(multisig.address);
    expect(wallet.add_multisig_address(multisig.config), "wallet accepts GEOM multisig watch-only address");
    expect(wallet.get_all_addresses().size() == 4, "all addresses include spendable keys and multisig watch-only");
    expect(wallet.get_watch_only_addresses().size() == 1, "watch-only list includes multisig");
    expect(wallet.get_multisig_addresses().size() == 1, "multisig config list includes entry");
    expect(wallet.remove_multisig_address(multisig.address), "wallet removes multisig watch-only address");
    expect(!wallet.remove_multisig_address(multisig.address), "removing missing multisig returns false");
    geocoin::wallet::MultisigConfig bad_config;
    bad_config.address = primary;
    expect(!wallet.add_multisig_address(bad_config), "wallet rejects non-GEOM multisig config");
    for (int seed = 2; seed < 40; ++seed) {
        (void)wallet.import_private_key(make_key(static_cast<uint8_t>(seed)), "lex-extra");
    }
    expect(wallet.get_address() == primary,
           "get_address stays on the initialized key even if imported addresses sort first");
}

void test_password_save_load_and_history()
{
    const auto dir = make_temp_dir("geocoin-wallet-lifecycle");
    std::error_code ec;
    try {
        geocoin::wallet::Wallet wallet;
        expect(wallet.initialize(make_key(10)), "wallet initializes for save/load");
        const geocoin::Address primary = wallet.get_address();
        const geocoin::Address participant_two = wallet.create_new_key("participant-two");
        expect(wallet.export_private_key("").size() == 32, "unprotected export works without password");

        geocoin::wallet::Wallet failed_save_wallet;
        expect(failed_save_wallet.initialize(make_key(9)),
               "wallet initializes for failed encrypted save check");
        expect(!failed_save_wallet.save_to_file(
                   (dir / "missing-parent" / "wallet.dat").string(),
                   "new-secret"
               ),
               "encrypted save reports an unwritable destination");
        expect(!failed_save_wallet.has_password_protection(),
               "failed encrypted save does not mutate the in-memory verifier state");

        const std::vector<geocoin::Address> participants{primary, participant_two};
        auto multisig = geocoin::wallet::create_multisig_address(2, 2, participants);
        multisig.config.address = geocoin::Address(multisig.address);
        expect(wallet.add_multisig_address(multisig.config),
               "wallet stores multisig config before save");

        wallet.remember_password("secret");
        expect(wallet.has_password_protection(), "wallet remembers password verifier");
        expect(wallet.verify_password("secret"), "wallet verifies correct password");
        expect(!wallet.verify_password("wrong"), "wallet rejects wrong password");
        expect(wallet.export_private_key("secret").size() == 32, "protected export accepts correct password");
        expect(wallet.export_private_key("wrong").empty(), "protected export rejects wrong password");

        const auto wallet_path = dir / "wallet.dat";
        const auto plaintext_path = dir / "plain.dat";
        expect(wallet.save_to_file(wallet_path.string(), "secret"), "protected wallet saves with password");
        expect(!wallet.save_to_file(plaintext_path.string(), ""), "protected wallet refuses empty save password");

        geocoin::wallet::Wallet loaded;
        expect(!loaded.load_from_file(wallet_path.string(), "wrong"), "load rejects wrong password");
        expect(loaded.load_from_file(wallet_path.string(), "secret"), "load accepts correct password");
        expect(loaded.has_key(primary), "loaded wallet contains primary key");
        expect(loaded.verify_password("secret"), "loaded wallet keeps password verifier");
        const auto loaded_multisig = loaded.get_multisig_addresses();
        expect(loaded_multisig.size() == 1, "loaded wallet restores multisig configs");
        expect(loaded_multisig[0].address.str() == multisig.address, "loaded multisig address matches");
        expect(loaded_multisig[0].participants.size() == 2, "loaded multisig participants roundtrip");

        geocoin::wallet::WalletTransaction wtx;
        wtx.tx = geocoin::core::Transaction::create_coinbase(1, primary, 10 * geocoin::COIN);
        wtx.tx_hash = wtx.tx.get_hash();
        wtx.block_height = 1;
        wtx.timestamp = 123;
        wtx.amount = 10 * geocoin::COIN;
        wtx.is_coinbase = true;
        loaded.add_transaction(wtx);
        expect(loaded.get_transactions(geocoin::Address(""), 10).size() == 1,
               "wallet history stores transaction");
        expect(loaded.get_transactions(primary, 10).size() == 1,
               "wallet history filters by output address");
        wtx.amount = 11 * geocoin::COIN;
        loaded.add_transaction(wtx);
        expect(loaded.get_transactions(geocoin::Address(""), 10).size() == 1,
               "adding same tx updates indexed history");

        const std::string old_default = geocoin::wallet::Wallet::get_default_wallet_path();
        geocoin::wallet::Wallet::set_default_wallet_path(wallet_path.string());
        expect(geocoin::wallet::Wallet::get_default_wallet_path() == wallet_path.string(),
               "default wallet path can be overridden");
        geocoin::wallet::Wallet::set_default_wallet_path(old_default);

        std::filesystem::remove_all(dir, ec);
    } catch (...) {
        std::filesystem::remove_all(dir, ec);
        throw;
    }
}

void test_plaintext_history_load_and_backup_fallback()
{
    const auto dir = make_temp_dir("geocoin-wallet-plain-history");
    std::error_code ec;
    try {
        geocoin::wallet::Wallet wallet;
        expect(wallet.initialize(make_key(120)), "plain wallet initializes");
        const geocoin::Address primary = wallet.get_address();

        geocoin::wallet::WalletTransaction wtx;
        wtx.tx = geocoin::core::Transaction::create_coinbase(7, primary, 3 * geocoin::COIN);
        wtx.tx_hash = wtx.tx.get_hash();
        wtx.block_height = 7;
        wtx.timestamp = 7007;
        wtx.amount = 3 * geocoin::COIN;
        wtx.fee = 12;
        wtx.is_coinbase = true;
        wtx.memo = "plain-history";
        wallet.add_transaction(wtx);

        const auto plain_path = dir / "wallet.dat";
        expect(wallet.save_to_file(plain_path.string(), ""), "plaintext wallet saves");

        geocoin::wallet::Wallet loaded;
        expect(loaded.load_from_file(plain_path.string(), ""), "plaintext wallet loads");
        expect(loaded.has_key(primary), "plaintext load restores key");
        const auto loaded_history = loaded.get_transactions(geocoin::Address(""), 10);
        expect(loaded_history.size() == 1, "plaintext load restores transaction history");
        expect(loaded_history[0].tx_hash == wtx.tx_hash, "loaded history txid matches");
        expect(loaded_history[0].block_height == 7, "loaded history height matches");
        expect(loaded_history[0].timestamp == 7007, "loaded history timestamp matches");
        expect(loaded_history[0].amount == 3 * geocoin::COIN, "loaded history amount matches");
        expect(loaded_history[0].fee == 12, "loaded history fee matches");
        expect(loaded_history[0].is_coinbase, "loaded history coinbase flag matches");
        expect(loaded_history[0].memo == "plain-history", "loaded history memo matches");

        loaded.clear();
        expect(loaded.get_key_count() == 0, "clear removes loaded keys");
        expect(!loaded.has_password_protection(), "clear removes password state");
        expect(loaded.get_transactions(geocoin::Address(""), 10).empty(), "clear removes loaded history");

        const auto backup_path = dir / "wallet.dat.bak";
        std::filesystem::copy_file(plain_path, backup_path, std::filesystem::copy_options::overwrite_existing, ec);
        expect(!ec, "wallet backup fixture copies");
        {
            std::ofstream bad(plain_path, std::ios::binary | std::ios::trunc);
            bad << "bad-wallet";
            expect(static_cast<bool>(bad), "wallet primary fixture corrupts");
        }

        geocoin::wallet::Wallet recovered;
        expect(recovered.load_from_file(plain_path.string(), ""), "wallet load falls back to backup");
        expect(recovered.has_key(primary), "backup fallback restores key");
        expect(recovered.get_transactions(geocoin::Address(""), 10).size() == 1,
               "backup fallback restores transaction history");

        std::filesystem::remove_all(dir, ec);
    } catch (...) {
        std::filesystem::remove_all(dir, ec);
        throw;
    }
}

void test_old_wallet_format_is_rejected()
{
    const auto dir = make_temp_dir("geocoin-wallet-legacy-password");
    std::error_code ec;
    try {
        const auto wallet_path = dir / "legacy.dat";
        const geocoin::bytes private_key = make_key(130);
        const std::string password = "legacy-pass";
        const geocoin::bytes password_bytes(password.begin(), password.end());
        const geocoin::Hash256 legacy_hash = geocoin::crypto::sha256(password_bytes);

        geocoin::bytes payload;
        payload.push_back(1); // legacy SHA256 password verifier
        payload.insert(payload.end(), legacy_hash.begin(), legacy_hash.end());
        append_u32_le(payload, 1); // key_count
        payload.insert(payload.end(), private_key.begin(), private_key.end());
        const std::string label = "legacy-key";
        append_u32_le(payload, static_cast<uint32_t>(label.size()));
        payload.insert(payload.end(), label.begin(), label.end());
        append_u64_le(payload, 424242);
        append_u32_le(payload, 0); // tx_count
        append_u64_le(payload, 77); // last_scanned_height
        append_u32_le(payload, 0); // multisig_count

        geocoin::bytes file_body;
        append_u32_le(file_body, 0x47454F43); // "GEOC"
        append_u32_le(file_body, 11);
        file_body.push_back(0); // plaintext payload
        append_u32_le(file_body, static_cast<uint32_t>(payload.size()));
        file_body.insert(file_body.end(), payload.begin(), payload.end());

        {
            std::ofstream file(wallet_path, std::ios::binary | std::ios::trunc);
            file.write(reinterpret_cast<const char*>(file_body.data()), static_cast<std::streamsize>(file_body.size()));
            expect(static_cast<bool>(file), "legacy wallet fixture writes");
        }

        geocoin::wallet::Wallet wallet;
        expect(!wallet.load_from_file(wallet_path.string(), ""), "old wallet format is rejected without conversion");
        std::ifstream preserved(wallet_path, std::ios::binary);
        const geocoin::bytes after((std::istreambuf_iterator<char>(preserved)), std::istreambuf_iterator<char>());
        expect(after == file_body, "unsupported wallet remains unchanged");

        std::filesystem::remove_all(dir, ec);
    } catch (...) {
        std::filesystem::remove_all(dir, ec);
        throw;
    }
}

void test_nft_wallet_builders_and_guards()
{
    auto chain_state = make_chain_state();
    chain_state->set_activation_manifest(geocoin::utils::ActivationManifest::fully_enabled_for_tests("degree"));
    geocoin::wallet::Wallet wallet;
    geocoin::wallet::Wallet recipient_wallet;
    expect(wallet.initialize(make_key(140)), "nft wallet initializes");
    expect(recipient_wallet.initialize(make_key(141)), "nft recipient initializes");

    const geocoin::Address owner = wallet.get_address();
    const geocoin::Address recipient = recipient_wallet.get_address();
    fund_address(chain_state, 1, 4 * geocoin::COIN, owner);

    const auto metadata = make_test_metadata();
    const geocoin::Amount fee = geocoin::consensus::params::MIN_TX_FEE;
    geocoin::core::Transaction tx;

    expect(!wallet.create_nft_create(metadata, 250, fee, nullptr, tx),
           "nft create rejects null chainstate");
    expect(!wallet.create_nft_create(metadata, 250, 0, chain_state, tx),
           "nft create rejects zero fee");
    expect(!wallet.create_nft_create(
               metadata,
               250,
               geocoin::consensus::params::MIN_TX_FEE - 1,
               chain_state,
               tx
           ),
           "nft create rejects fee below relay floor");
    expect(wallet.create_nft_create(metadata, 250, fee, chain_state, tx),
           "nft create builder succeeds");
    expect(tx.type == geocoin::core::TransactionType::NFT_CREATE, "nft create tx type");
    expect(tx.nft_create.has_value(), "nft create payload present");
    expect(tx.nft_create->nft_id == metadata.calculate_hash(), "nft create id is metadata hash");
    expect(tx.nft_create->creator_address == owner, "nft create owner is the wallet primary address");
    expect(tx.nft_create->royalty_percent == 250, "nft create royalty preserved");
    expect(!tx.nft_create->signature.empty(), "nft create payload is signed");
    expect(!tx.witnesses.empty(), "nft create transaction is signed");
    expect(chain_state->validate_transaction(tx), "nft create builder output validates");
    const geocoin::Height nft_height = chain_state->get_height() + 1;
    expect(chain_state->apply_transaction(tx, nft_height, false), "nft create builder output applies");
    chain_state->set_height(nft_height);

    const geocoin::Hash256 nft_id = tx.nft_create->nft_id;
    geocoin::core::Transaction transfer_tx;
    expect(!wallet.create_nft_transfer(nft_id, recipient, fee, nullptr, transfer_tx),
           "nft transfer rejects null chainstate");
    expect(!wallet.create_nft_transfer(nft_id, geocoin::Address(""), fee, chain_state, transfer_tx),
           "nft transfer rejects empty recipient");
    expect(!wallet.create_nft_transfer(nft_id, recipient, 0, chain_state, transfer_tx),
           "nft transfer rejects zero fee");
    expect(!wallet.create_nft_transfer(
               nft_id,
               recipient,
               geocoin::consensus::params::MIN_TX_FEE - 1,
               chain_state,
               transfer_tx
           ),
           "nft transfer rejects fee below relay floor");
    expect(wallet.create_nft_transfer(nft_id, recipient, fee, chain_state, transfer_tx),
           "nft transfer builder succeeds");
    expect(transfer_tx.type == geocoin::core::TransactionType::NFT_TRANSFER, "nft transfer tx type");
    expect(transfer_tx.nft_transfer.has_value(), "nft transfer payload present");
    expect(transfer_tx.nft_transfer->from == owner, "nft transfer owner comes from selected utxo");
    expect(transfer_tx.nft_transfer->to == recipient, "nft transfer recipient preserved");
    expect(!transfer_tx.nft_transfer->signature.empty(), "nft transfer payload is signed");
    expect(chain_state->validate_transaction(transfer_tx), "nft transfer builder output validates");

    auto mismatched_transfer_fee = transfer_tx;
    ++mismatched_transfer_fee.nft_transfer->fee;
    mismatched_transfer_fee.nft_transfer->signature = geocoin::crypto::sign_recoverable_message(
        geocoin::crypto::sha256(mismatched_transfer_fee.nft_transfer->serialize_for_signing()),
        make_key(140)
    );
    mismatched_transfer_fee.sign(make_key(140));
    expect(mismatched_transfer_fee.verify(), "nft transfer fee mismatch fixture has valid signatures");
    expect(!chain_state->validate_transaction(mismatched_transfer_fee),
           "nft transfer rejects payload fee that differs from UTXO fee");

    geocoin::core::Transaction burn_tx;
    expect(!wallet.create_nft_burn(nft_id, 0, chain_state, burn_tx),
           "nft burn rejects zero fee");
    expect(!wallet.create_nft_burn(
               nft_id,
               geocoin::consensus::params::MIN_TX_FEE - 1,
               chain_state,
               burn_tx
           ),
           "nft burn rejects fee below relay floor");
    expect(!wallet.create_nft_burn(nft_id, fee, nullptr, burn_tx),
           "nft burn rejects null chainstate");
    expect(wallet.create_nft_burn(nft_id, fee, chain_state, burn_tx),
           "nft burn builder succeeds");
    expect(burn_tx.type == geocoin::core::TransactionType::NFT_BURN, "nft burn tx type");
    expect(burn_tx.nft_burn.has_value(), "nft burn payload present");
    expect(burn_tx.nft_burn->owner == owner, "nft burn owner comes from selected utxo");
    expect(!burn_tx.nft_burn->signature.empty(), "nft burn payload is signed");
    expect(chain_state->validate_transaction(burn_tx), "nft burn builder output validates");

    auto mismatched_burn_fee = burn_tx;
    ++mismatched_burn_fee.nft_burn->fee;
    mismatched_burn_fee.nft_burn->signature = geocoin::crypto::sign_recoverable_message(
        geocoin::crypto::sha256(mismatched_burn_fee.nft_burn->serialize_for_signing()),
        make_key(140)
    );
    mismatched_burn_fee.sign(make_key(140));
    expect(mismatched_burn_fee.verify(), "nft burn fee mismatch fixture has valid signatures");
    expect(!chain_state->validate_transaction(mismatched_burn_fee),
           "nft burn rejects payload fee that differs from UTXO fee");

    const auto history = wallet.get_transactions(geocoin::Address(""), 10);
    expect(history.size() >= 3, "nft builders append wallet history");

    geocoin::wallet::Wallet empty_wallet;
    expect(empty_wallet.initialize(make_key(142)), "empty nft wallet initializes");
    geocoin::core::Transaction empty_tx;
    expect(!empty_wallet.create_nft_create(metadata, 0, fee, chain_state, empty_tx),
           "nft create rejects missing spendable utxo");
}

void test_nft_wallet_transfer_and_burn_use_current_owner_address()
{
    geocoin::bytes key_a = make_key(150);
    geocoin::bytes key_b = make_key(151);
    geocoin::Address addr_a;
    geocoin::Address addr_b;
    expect(geocoin::crypto::compute_address_from_private_key(key_a, addr_a), "test address A derives");
    expect(geocoin::crypto::compute_address_from_private_key(key_b, addr_b), "test address B derives");

    geocoin::bytes non_owner_key = key_a;
    geocoin::bytes owner_key = key_b;
    geocoin::Address non_owner = addr_a;
    geocoin::Address owner = addr_b;
    if (owner < non_owner) {
        std::swap(non_owner_key, owner_key);
        std::swap(non_owner, owner);
    }

    auto chain_state = make_chain_state();
    chain_state->set_activation_manifest(geocoin::utils::ActivationManifest::fully_enabled_for_tests("degree"));
    fund_address(chain_state, 1, 4 * geocoin::COIN, non_owner);
    fund_address(chain_state, 2, 4 * geocoin::COIN, owner);

    geocoin::wallet::Wallet owner_only_wallet;
    expect(owner_only_wallet.initialize(owner_key), "owner-only NFT wallet initializes");

    geocoin::wallet::Wallet combined_wallet;
    expect(combined_wallet.initialize(non_owner_key), "combined NFT wallet initializes with non-owner key");
    expect(combined_wallet.import_private_key(owner_key, "nft-owner"),
           "combined NFT wallet imports owner key");

    geocoin::wallet::Wallet recipient_wallet;
    expect(recipient_wallet.initialize(make_key(152)), "NFT recipient wallet initializes");
    const geocoin::Address recipient = recipient_wallet.get_address();
    const geocoin::Amount fee = geocoin::consensus::params::MIN_TX_FEE;

    auto metadata = make_test_metadata();
    metadata.name = "Owner-selected NFT";
    geocoin::core::Transaction create_tx;
    expect(owner_only_wallet.create_nft_create(metadata, 0, fee, chain_state, create_tx),
           "owner-only wallet creates NFT");
    expect(create_tx.nft_create.has_value(), "owner-only NFT create has payload");
    expect(create_tx.nft_create->creator_address == owner, "test NFT is created by owner address");
    expect(chain_state->apply_transaction(create_tx, chain_state->get_height() + 1, false),
           "owner-only NFT create applies");
    chain_state->set_height(chain_state->get_height() + 1);
    const geocoin::Hash256 nft_id = create_tx.nft_create->nft_id;

    geocoin::core::Transaction transfer_tx;
    expect(combined_wallet.create_nft_transfer(nft_id, recipient, fee, chain_state, transfer_tx),
           "combined wallet builds NFT transfer from current owner");
    expect(transfer_tx.nft_transfer.has_value(), "owner-selected NFT transfer has payload");
    expect(transfer_tx.nft_transfer->from == owner,
           "NFT transfer uses current owner address instead of earlier funded wallet address");
    expect(chain_state->validate_transaction(transfer_tx),
           "owner-selected NFT transfer validates");

    geocoin::core::Transaction burn_tx;
    expect(combined_wallet.create_nft_burn(nft_id, fee, chain_state, burn_tx),
           "combined wallet builds NFT burn from current owner");
    expect(burn_tx.nft_burn.has_value(), "owner-selected NFT burn has payload");
    expect(burn_tx.nft_burn->owner == owner,
           "NFT burn uses current owner address instead of earlier funded wallet address");
    expect(chain_state->validate_transaction(burn_tx),
           "owner-selected NFT burn validates");

    geocoin::core::Transaction combined_create;
    auto combined_metadata = make_test_metadata();
    combined_metadata.name = "Primary-key NFT";
    expect(combined_wallet.create_nft_create(combined_metadata, 0, fee, chain_state, combined_create),
           "combined wallet creates NFT from primary address even when another key has UTXOs");
    expect(combined_create.nft_create.has_value(), "combined create has payload");
    expect(combined_create.nft_create->creator_address == non_owner,
           "nft create spends the wallet primary address, not an imported funded key");
}

void test_utxo_backed_transaction_paths()
{
    auto chain_state = make_chain_state();
    geocoin::wallet::Wallet wallet;
    geocoin::wallet::Wallet recipient_wallet;
    expect(wallet.initialize(make_key(50)), "sender wallet initializes");
    expect(recipient_wallet.initialize(make_key(80)), "recipient wallet initializes");

    const geocoin::Address sender = wallet.get_address();
    const geocoin::Address recipient = recipient_wallet.get_address();
    fund_address(chain_state, 1, 5 * geocoin::COIN, sender);
    fund_address(chain_state, 2, 2 * geocoin::COIN, sender);

    const geocoin::Amount funded_balance = wallet.get_balance(sender, chain_state);
    expect(funded_balance > 2 * geocoin::COIN, "wallet balance reads chainstate UTXOs");
    expect(wallet.get_total_balance(chain_state) == funded_balance,
           "wallet total balance sums spendable keys");
    expect(wallet.estimate_required_fee(1 * geocoin::COIN, nullptr) == geocoin::consensus::params::MIN_TX_FEE,
           "fee estimate without chainstate returns minimum");
    const geocoin::Amount estimated_fee = static_cast<geocoin::Amount>(
        (static_cast<uint64_t>(geocoin::consensus::params::TX_BASE_SIZE +
                               geocoin::consensus::params::TX_INPUT_SIZE +
                               2 * geocoin::consensus::params::TX_OUTPUT_SIZE) *
             static_cast<uint64_t>(geocoin::consensus::params::FEE_RATE_PER_KB) +
         999u) /
        1000u
    );
    expect(wallet.estimate_required_fee(1 * geocoin::COIN, chain_state) == estimated_fee,
           "fee estimate uses the Falcon-aware byte rate");
    expect(estimated_fee < geocoin::COIN / 1000,
           "typical base payment fee stays below 0.001 GC");

    auto dust_state = make_chain_state();
    geocoin::wallet::Wallet dust_wallet;
    expect(dust_wallet.initialize(make_key(83)), "dust-change wallet initializes");
    const geocoin::Amount dust_remainder =
        static_cast<geocoin::Amount>(geocoin::consensus::params::DUST_LIMIT) - 1;
    geocoin::core::Transaction dust_funding;
    dust_funding.version = 2;
    dust_funding.type = geocoin::core::TransactionType::STANDARD;
    dust_funding.timestamp = 83;
    dust_funding.outputs.push_back({
        geocoin::COIN + estimated_fee + dust_remainder,
        dust_wallet.get_address(),
        {}
    });
    geocoin::state::ChainStateTestAccess::seed_funding(*dust_state, dust_funding, 1);
    geocoin::core::Transaction dust_tx;
    expect(dust_wallet.create_transaction(
               recipient,
               geocoin::COIN,
               estimated_fee,
               dust_state,
               dust_tx
           ),
           "wallet builds payment with sub-dust remainder");
    expect(dust_tx.outputs.size() == 1,
           "sub-dust remainder is absorbed into the fee instead of creating dust");
    expect(dust_state->calculate_transaction_fee(dust_tx) == estimated_fee + dust_remainder,
           "absorbed sub-dust remainder is accounted as miner fee");

    geocoin::core::Transaction tx;
    expect(!wallet.create_transaction(recipient, 1 * geocoin::COIN, geocoin::consensus::params::MIN_TX_FEE, nullptr, tx),
           "create_transaction rejects null chainstate");
    expect(wallet.create_transaction(recipient, 2 * geocoin::COIN, geocoin::consensus::params::MIN_TX_FEE, chain_state, tx),
           "create_transaction builds signed standard transaction");
    expect(!tx.inputs.empty(), "created transaction has inputs");
    expect(tx.outputs.size() >= 1, "created transaction has outputs");
    expect(wallet.get_transactions(geocoin::Address(""), 10).size() == 1,
           "created transaction is added to wallet history");

    geocoin::wallet::KeyInfo used_info;
    expect(wallet.get_key_info(sender, used_info), "used key info is available");
    expect(used_info.used, "create_transaction marks source address used");

    geocoin::core::Transaction change_tx;
    expect(wallet.create_transaction_with_change(
               recipient,
               1 * geocoin::COIN,
               geocoin::consensus::params::MIN_TX_FEE,
               recipient,
               chain_state,
               change_tx
           ),
           "create_transaction_with_change accepts explicit change address");

    geocoin::core::Transaction move_tx;
    expect(!wallet.move_funds(sender, recipient, geocoin::consensus::params::MIN_TX_FEE, nullptr, move_tx),
           "move_funds rejects null chainstate");
    expect(!wallet.move_funds(recipient, sender, geocoin::consensus::params::MIN_TX_FEE, chain_state, move_tx),
           "move_funds rejects source address not in wallet");
    expect(wallet.move_funds(sender, recipient, geocoin::consensus::params::MIN_TX_FEE, chain_state, move_tx),
           "move_funds builds all-funds transaction from owned address");
    expect(move_tx.outputs.size() == 1, "move_funds creates a single output");

    geocoin::wallet::Wallet unfunded_wallet;
    expect(unfunded_wallet.initialize(make_key(82)), "unfunded wallet initializes");
    expect(!unfunded_wallet.move_funds(unfunded_wallet.get_address(), recipient, geocoin::consensus::params::MIN_TX_FEE, chain_state, move_tx),
           "move_funds rejects owned address without UTXO");

    auto immature_state = make_chain_state();
    immature_state->set_height(2);
    fund_address(immature_state, 2, 3 * geocoin::COIN, sender);
    geocoin::core::Transaction immature_tx;
    expect(!wallet.create_transaction(
               recipient,
               1 * geocoin::COIN,
               geocoin::consensus::params::MIN_TX_FEE,
               immature_state,
               immature_tx
           ),
           "create_transaction skips immature coinbase UTXO");
    expect(!wallet.move_funds(sender, recipient, geocoin::consensus::params::MIN_TX_FEE, immature_state, immature_tx),
           "move_funds skips immature coinbase UTXO");

    auto exact_fee_state = make_chain_state();
    fund_address(exact_fee_state, 1, geocoin::consensus::params::MIN_TX_FEE, sender);
    expect(!wallet.move_funds(sender, recipient, geocoin::consensus::params::MIN_TX_FEE, exact_fee_state, move_tx),
           "move_funds rejects UTXO set that only covers fee");

    auto split_state = make_chain_state();
    const geocoin::Address second_sender = wallet.create_new_key("split-sender");
    fund_address(split_state, 1, 1 * geocoin::COIN, sender);
    fund_address(split_state, 2, 1 * geocoin::COIN, second_sender);
    geocoin::core::Transaction split_tx;
    expect(!wallet.create_transaction(
               recipient,
               150 * geocoin::COIN / 100,
               geocoin::consensus::params::MIN_TX_FEE,
               split_state,
               split_tx
           ),
           "create_transaction refuses to combine UTXO from different wallet addresses");
}

void test_recent_history_discard_and_fee_floor()
{
    using geocoin::consensus::params::FEE_RATE_PER_KB;
    using geocoin::consensus::params::TX_BASE_SIZE;
    using geocoin::consensus::params::TX_INPUT_SIZE;
    using geocoin::consensus::params::TX_OUTPUT_SIZE;
    const auto size_fee = [](size_t inputs, size_t outputs) {
        const uint64_t size = TX_BASE_SIZE + inputs * TX_INPUT_SIZE + outputs * TX_OUTPUT_SIZE;
        return static_cast<geocoin::Amount>(
            (size * static_cast<uint64_t>(FEE_RATE_PER_KB) + 999u) / 1000u);
    };

    auto chain_state = make_chain_state();
    geocoin::wallet::Wallet wallet;
    geocoin::wallet::Wallet recipient_wallet;
    expect(wallet.initialize(make_key(91)), "fee-floor wallet initializes");
    expect(recipient_wallet.initialize(make_key(92)), "fee-floor recipient initializes");
    const geocoin::Address sender = wallet.get_address();
    fund_address(chain_state, 1, geocoin::COIN, sender);
    fund_address(chain_state, 2, geocoin::COIN / 2, sender);
    // Coinbase funding pays the foundation share too; read the actual values.
    auto funded = chain_state->get_utxos_for_address(sender);
    expect(funded.size() == 2, "fee-floor sender has two UTXOs");
    std::sort(funded.begin(), funded.end(), [](const auto& a, const auto& b) { return a.value > b.value; });
    expect(funded[1].value > 10'000, "the smaller UTXO can fund change above dust");

    // The larger input alone covers the amount at the minimum fee, so the
    // plain estimate describes a one-input payment.
    const geocoin::Amount amount = funded[0].value - 4000;
    expect(wallet.estimate_required_fee(amount, chain_state) == size_fee(1, 1),
           "minimum-fee estimate selects one input");
    // Regression: a larger requested fee needs the second input, and the
    // estimate used to describe the one-input transaction regardless, so the
    // built payment paid less than its size required.
    expect(wallet.estimate_required_fee(amount, chain_state, {}, 5000) == size_fee(2, 2),
           "a requested fee that needs more inputs raises the size estimate");
    expect(wallet.estimate_required_fee(amount, chain_state, {}, 50'000) == 50'000,
           "a requested fee above the size requirement is kept");

    geocoin::core::Transaction sent;
    expect(wallet.create_transaction(recipient_wallet.get_address(), geocoin::COIN / 4,
                                     size_fee(1, 2), chain_state, sent),
           "wallet builds a payment for the history checks");
    const geocoin::Hash256 sent_hash = sent.get_hash();
    const auto newest = wallet.get_recent_transactions(geocoin::Address(""), 1, 0);
    expect(newest.size() == 1 && newest.front().tx_hash == sent_hash,
           "the newest entry is the payment just built");

    // A rejected submission must not leave a permanently unconfirmed send.
    expect(wallet.discard_unconfirmed_transaction(sent_hash), "unconfirmed entry is discarded");
    expect(wallet.get_transactions(geocoin::Address(""), 0).empty(), "discarded entry leaves history");
    expect(!wallet.discard_unconfirmed_transaction(sent_hash), "a missing entry is not discarded twice");

    // Synthetic history: a receipt to `watched`, an unrelated entry, then a
    // spend of that receipt that sends nothing back to `watched`.
    const geocoin::Address watched = sender;
    const geocoin::Address other = recipient_wallet.get_address();
    const auto record = [&](const geocoin::core::Transaction& tx, geocoin::Height height) {
        geocoin::wallet::WalletTransaction wtx;
        wtx.tx = tx;
        wtx.tx_hash = tx.get_hash();
        wtx.block_height = height;
        wallet.add_transaction(wtx);
        return wtx.tx_hash;
    };
    geocoin::core::Transaction receipt;
    receipt.timestamp = 1;
    receipt.outputs.push_back({geocoin::COIN, watched, {}});
    const auto receipt_hash = record(receipt, 10);
    geocoin::core::Transaction unrelated;
    unrelated.timestamp = 2;
    unrelated.outputs.push_back({geocoin::COIN, other, {}});
    const auto unrelated_hash = record(unrelated, 11);
    geocoin::core::Transaction spend;
    spend.timestamp = 3;
    spend.inputs.push_back({receipt_hash, 0, {}, 0xffffffff, {}, {}});
    spend.outputs.push_back({geocoin::COIN / 2, other, {}});
    const auto spend_hash = record(spend, 12);

    // Regression: listtransactions paged from the oldest entry, so new
    // activity was invisible without a large skip.
    const auto latest_two = wallet.get_recent_transactions(geocoin::Address(""), 2, 0);
    expect(latest_two.size() == 2 && latest_two[0].tx_hash == unrelated_hash &&
               latest_two[1].tx_hash == spend_hash,
           "recent page holds the newest entries, oldest first");
    const auto skipped = wallet.get_recent_transactions(geocoin::Address(""), 2, 1);
    expect(skipped.size() == 2 && skipped[0].tx_hash == receipt_hash &&
               skipped[1].tx_hash == unrelated_hash,
           "skip counts from the newest entry");
    const auto for_watched = wallet.get_recent_transactions(watched, 10, 0);
    expect(for_watched.size() == 2 && for_watched[0].tx_hash == receipt_hash &&
               for_watched[1].tx_hash == spend_hash,
           "the address filter includes spends of the address's outputs");

    expect(!wallet.discard_unconfirmed_transaction(spend_hash),
           "confirmed history entries are never discarded");
}

void test_monetary_overflow_guards()
{
    using geocoin::Amount;
    using geocoin::core::Transaction;
    using geocoin::core::TransactionType;

    auto chain_state = make_chain_state();
    geocoin::wallet::Wallet wallet;
    geocoin::wallet::Wallet recipient_wallet;
    expect(wallet.initialize(make_key(171)), "overflow wallet initializes");
    expect(recipient_wallet.initialize(make_key(172)), "overflow recipient initializes");
    const auto sender = wallet.get_address();
    const auto recipient = recipient_wallet.get_address();

    auto add_extreme_utxo = [&](Amount value, uint64_t timestamp) {
        Transaction funding;
        funding.version = 2;
        funding.type = TransactionType::STANDARD;
        funding.timestamp = timestamp;
        funding.outputs.push_back({value, sender, {}});
        geocoin::state::ChainStateTestAccess::seed_funding(*chain_state, funding, timestamp);
    };
    add_extreme_utxo((std::numeric_limits<Amount>::max)() - 5, 1);
    add_extreme_utxo(10, 2);

    expect(chain_state->get_balance(sender) == 0, "overflowing address balance fails closed");
    expect(chain_state->get_total_supply() == 0, "overflowing total supply fails closed");
    expect(wallet.get_total_balance(chain_state) == 0, "overflowing wallet balance fails closed");
    expect(wallet.estimate_required_fee((std::numeric_limits<Amount>::max)(), chain_state) == 0,
           "fee estimation rejects amount plus fee overflow");

    Transaction tx;
    expect(!wallet.create_transaction(
               recipient,
               (std::numeric_limits<Amount>::max)(),
               1,
               chain_state,
               tx
           ),
           "wallet rejects amount plus fee overflow");
    expect(!wallet.create_transaction(
               recipient,
               (std::numeric_limits<Amount>::max)() - 1,
               1,
               chain_state,
               tx
           ),
           "wallet rejects overflowing selected UTXO total");
    expect(!wallet.move_funds(sender, recipient, 1, chain_state, tx),
           "move_funds rejects overflowing UTXO total");

    geocoin::wallet::WalletTransaction previous;
    previous.tx.version = 2;
    previous.tx.type = TransactionType::STANDARD;
    previous.tx.timestamp = 500;
    previous.tx.outputs.push_back({(std::numeric_limits<Amount>::max)() - 5, sender, {}});
    previous.tx.outputs.push_back({10, sender, {}});
    previous.tx_hash = previous.tx.get_hash();
    wallet.add_transaction(previous);
    expect(wallet.get_credit(previous) == 0, "wallet credit overflow fails closed");

    geocoin::wallet::WalletTransaction spending;
    spending.tx.version = 2;
    spending.tx.type = TransactionType::STANDARD;
    spending.tx.inputs.push_back({previous.tx_hash, 0, {}, 0xffffffff, {}, {}});
    spending.tx.inputs.push_back({previous.tx_hash, 1, {}, 0xffffffff, {}, {}});
    expect(wallet.get_debit(spending, nullptr) == 0, "wallet debit overflow fails closed");
}

void test_wallet_edge_guards_and_accounting_caches()
{
    geocoin::wallet::Wallet empty_wallet;
    expect(empty_wallet.get_address().empty(), "empty wallet has no primary address");
    expect(empty_wallet.export_private_key("").empty(), "empty wallet exports no key");
    expect(empty_wallet.verify_password("unused"), "unprotected wallet accepts any password check");
    expect(empty_wallet.get_balance(geocoin::Address("GEOmissing"), nullptr) == 0,
           "wallet balance with null chainstate is zero");
    expect(empty_wallet.get_total_balance(nullptr) == 0,
           "wallet total balance with null chainstate is zero");
    geocoin::wallet::KeyInfo missing_info;
    expect(!empty_wallet.get_key_info(geocoin::Address("GEOmissing"), missing_info),
           "missing key info is rejected");
    geocoin::bytes missing_key;
    expect(!empty_wallet.get_private_key(geocoin::Address("GEOmissing"), missing_key),
           "missing private key is rejected");

    empty_wallet.remember_password("cache-pass");
    expect(empty_wallet.has_password_protection(), "remember_password enables verifier");
    empty_wallet.clear_password_cache();
    expect(!empty_wallet.has_password_protection(), "clear_password_cache removes verifier");
    expect(empty_wallet.verify_password("wrong-after-clear"),
           "password check succeeds after verifier clear");

    auto chain_state = make_chain_state();
    geocoin::wallet::Wallet wallet;
    geocoin::wallet::Wallet recipient_wallet;
    expect(wallet.initialize(make_key(150)), "accounting wallet initializes");
    expect(recipient_wallet.initialize(make_key(151)), "accounting recipient initializes");
    const geocoin::Address sender = wallet.get_address();
    const geocoin::Address recipient = recipient_wallet.get_address();

    geocoin::core::Transaction coinbase = geocoin::core::Transaction::create_coinbase(
        1,
        sender,
        4 * geocoin::COIN
    );
    expect(chain_state->apply_transaction(coinbase, 1, true), "accounting coinbase applies");

    geocoin::wallet::WalletTransaction coinbase_wtx;
    coinbase_wtx.tx = coinbase;
    coinbase_wtx.tx_hash = coinbase.get_hash();
    coinbase_wtx.is_coinbase = true;
    expect(wallet.get_debit(coinbase_wtx, chain_state) == 0,
           "coinbase wallet transaction has zero debit");

    geocoin::wallet::WalletTransaction spend_wtx;
    spend_wtx.tx = make_manual_spend(
        coinbase,
        sender,
        recipient,
        1 * geocoin::COIN,
        geocoin::consensus::params::MIN_TX_FEE
    );
    spend_wtx.tx_hash = spend_wtx.tx.get_hash();
    spend_wtx.is_coinbase = false;
    const int64_t debit = wallet.get_debit(spend_wtx, chain_state);
    expect(debit == static_cast<int64_t>(coinbase.outputs[0].value),
           "wallet debit can resolve previous UTXO from chainstate");
    expect(wallet.get_debit(spend_wtx, nullptr) == debit,
           "wallet debit cache is reused");

    geocoin::wallet::WalletTransaction credit_wtx;
    credit_wtx.tx = geocoin::core::Transaction::create_coinbase(2, sender, 2 * geocoin::COIN);
    credit_wtx.tx_hash = credit_wtx.tx.get_hash();
    const int64_t credit = wallet.get_credit(credit_wtx);
    expect(credit == static_cast<int64_t>(credit_wtx.tx.outputs[0].value),
           "wallet credit counts owned outputs");
    expect(wallet.get_credit(credit_wtx) == credit,
           "wallet credit cache is reused");
}

void test_scan_and_sync_transaction_paths()
{
    const auto dir = make_temp_dir("geocoin-wallet-scan");
    std::error_code ec;
    try {
        auto storage = std::make_shared<geocoin::storage::BlockStorage>(dir.string());
        expect(storage->is_open(), "wallet scan block storage opens");

        auto chain_state = std::make_shared<geocoin::state::ChainState>(
            std::make_unique<geocoin::state::MemoryStorage>());
        chain_state->set_block_storage(storage);

        geocoin::wallet::Wallet wallet;
        geocoin::wallet::Wallet recipient_wallet;
        expect(wallet.initialize(make_key(91)), "scan wallet initializes");
        expect(recipient_wallet.initialize(make_key(92)), "scan recipient initializes");
        const geocoin::Address wallet_address = wallet.get_address();
        const geocoin::Address recipient = recipient_wallet.get_address();

        geocoin::core::Transaction coinbase = geocoin::core::Transaction::create_coinbase(
            1,
            wallet_address,
            12 * geocoin::COIN
        );
        expect(chain_state->apply_transaction(coinbase, 1, true), "scan coinbase applies to chainstate");

        geocoin::core::Block block1;
        block1.transactions.push_back(coinbase);
        finalize_block(block1, 1, {});
        expect(storage->save_block(block1, 1, false), "scan block1 persists");
        expect(storage->set_active_block_and_tip(1, block1.get_hash()), "scan block1 active index persists");

        const geocoin::Amount fee = geocoin::consensus::params::MIN_TX_FEE;
        geocoin::core::Transaction spend = make_manual_spend(
            coinbase,
            wallet_address,
            recipient,
            3 * geocoin::COIN,
            fee
        );

        geocoin::core::Block block2;
        block2.transactions.push_back(spend);
        finalize_block(block2, 2, block1.get_hash());
        expect(storage->save_block(block2, 2, false), "scan block2 persists");
        expect(storage->set_active_block_and_tip(2, block2.get_hash()), "scan block2 active index persists");
        chain_state->set_height(2);

        geocoin::wallet::Wallet scanner;
        expect(scanner.initialize(make_key(91)), "scanner wallet initializes with same key");
        scanner.scan_blockchain(nullptr, 0);
        expect(scanner.get_last_scanned_height() == 0, "scan with null chainstate is a no-op");
        scanner.scan_blockchain(chain_state, 1);
        expect(scanner.get_last_scanned_height() == 2, "scanner records last scanned height");

        auto scanned = scanner.get_transactions(geocoin::Address(""), 10);
        expect(scanned.size() == 2, "scan discovers incoming coinbase and outgoing spend");
        bool saw_coinbase = false;
        bool saw_spend = false;
        for (const auto& wtx : scanned) {
            if (wtx.tx_hash == coinbase.get_hash()) {
                saw_coinbase = true;
                expect(wtx.amount > 0 && wtx.is_coinbase, "scanned coinbase is positive credit");
            }
            if (wtx.tx_hash == spend.get_hash()) {
                saw_spend = true;
                expect(wtx.amount < 0 && !wtx.is_coinbase, "scanned spend is negative net amount");
            }
        }
        expect(saw_coinbase && saw_spend, "scan found both expected txids");

        scanner.scan_blockchain(chain_state, 99);
        expect(scanner.get_last_scanned_height() == 2,
               "scan from beyond current height clamps last scanned to current height");

        geocoin::wallet::Wallet sync_wallet;
        expect(sync_wallet.initialize(make_key(91)), "sync wallet initializes with same key");
        sync_wallet.sync_transaction(spend, 2, 2002, nullptr);
        expect(sync_wallet.get_transactions(geocoin::Address(""), 10).empty(),
               "sync_transaction with null chainstate is a no-op");

        sync_wallet.sync_transaction(coinbase, 1, 1001, chain_state);
        expect(sync_wallet.get_transactions(geocoin::Address(""), 10).size() == 1,
               "sync_transaction adds owned incoming tx");
        sync_wallet.sync_transaction(coinbase, 2, 2001, chain_state);
        auto synced = sync_wallet.get_transactions(geocoin::Address(""), 10);
        expect(synced.size() == 1 && synced[0].block_height == 2 && synced[0].timestamp == 2001,
               "sync_transaction updates existing transaction confirmation data");

        sync_wallet.sync_transaction(spend, 3, 3001, chain_state);
        synced = sync_wallet.get_transactions(geocoin::Address(""), 10);
        expect(synced.size() == 2, "sync_transaction adds outgoing spend using wallet tx index debit path");

        geocoin::core::Block disconnected;
        disconnected.transactions = {spend};
        sync_wallet.sync_disconnected_block(disconnected, 3, 2);
        synced = sync_wallet.get_transactions(geocoin::Address(""), 10);
        const auto disconnected_spend = std::find_if(
            synced.begin(),
            synced.end(),
            [&](const auto& wallet_tx) { return wallet_tx.tx_hash == spend.get_hash(); }
        );
        expect(disconnected_spend != synced.end() && disconnected_spend->block_height == 0,
               "disconnected block downgrades wallet transaction to unconfirmed");
        expect(sync_wallet.get_last_scanned_height() == 0,
               "transaction events do not advance the contiguous history scan cursor");

        // Simulate a dropped reorg notification: the wallet still believes the
        // transaction belongs to orphan height 3 while the active index stores
        // it at height 2. Reconciliation must demote and rediscover it.
        sync_wallet.sync_transaction(spend, 3, 3002, chain_state);
        sync_wallet.reconcile_blockchain(chain_state, 2);
        synced = sync_wallet.get_transactions(geocoin::Address(""), 10);
        const auto reconciled_spend = std::find_if(
            synced.begin(),
            synced.end(),
            [&](const auto& wallet_tx) { return wallet_tx.tx_hash == spend.get_hash(); }
        );
        expect(reconciled_spend != synced.end() && reconciled_spend->block_height == 2,
               "stream-gap reconciliation rediscovers transaction on the active chain");
        expect(sync_wallet.get_last_scanned_height() == 2,
               "stream-gap reconciliation restores the active scan cursor");

        geocoin::core::Transaction unrelated = geocoin::core::Transaction::create_coinbase(
            4,
            recipient,
            1 * geocoin::COIN
        );
        sync_wallet.sync_transaction(unrelated, 4, 4001, chain_state);
        expect(sync_wallet.get_transactions(geocoin::Address(""), 10).size() == 2,
               "sync_transaction ignores unrelated transaction");

        storage->close();
        std::filesystem::remove_all(dir, ec);
    } catch (...) {
        std::filesystem::remove_all(dir, ec);
        throw;
    }
}

void test_wallet_file_guards_and_participant_roundtrip()
{
    const auto dir = make_temp_dir("geocoin-wallet-file-guards");
    std::error_code ec;
    try {
        geocoin::wallet::Wallet missing_file;
        expect(!missing_file.load_from_file((dir / "missing.dat").string(), ""),
               "missing wallet file is rejected");

        const auto empty_file = dir / "empty.dat";
        write_bytes(empty_file, {});
        geocoin::wallet::Wallet empty_file_wallet;
        expect(!empty_file_wallet.load_from_file(empty_file.string(), ""),
               "wallet load rejects empty file before magic");

        geocoin::bytes magic_only_bytes;
        append_u32_le(magic_only_bytes, 0x47454F43);
        const auto magic_only = dir / "magic-only.dat";
        write_bytes(magic_only, magic_only_bytes);
        geocoin::wallet::Wallet magic_only_wallet;
        expect(!magic_only_wallet.load_from_file(magic_only.string(), ""),
               "wallet load rejects file truncated before version");

        geocoin::bytes header_without_payload;
        append_u32_le(header_without_payload, 0x47454F43);
        append_u32_le(header_without_payload, 11);
        header_without_payload.push_back(0);
        append_u32_le(header_without_payload, 4);
        const auto missing_payload = dir / "missing-payload.dat";
        write_bytes(missing_payload, header_without_payload);
        geocoin::wallet::Wallet missing_payload_wallet;
        expect(!missing_payload_wallet.load_from_file(missing_payload.string(), ""),
               "wallet load rejects file truncated before payload");

        const geocoin::bytes minimal_payload = make_minimal_plain_wallet_payload();
        const auto bad_magic = dir / "bad-magic.dat";
        write_bytes(bad_magic, make_wallet_file_body(0x12345678, 13, 0, minimal_payload));
        geocoin::wallet::Wallet bad_magic_wallet;
        expect(!bad_magic_wallet.load_from_file(bad_magic.string(), ""),
               "wallet load rejects bad magic");

        const auto bad_version = dir / "bad-version.dat";
        write_bytes(bad_version, make_wallet_file_body(0x47454F43, 10, 0, minimal_payload));
        geocoin::wallet::Wallet bad_version_wallet;
        expect(!bad_version_wallet.load_from_file(bad_version.string(), ""),
               "wallet load rejects unsupported version");

        const auto bad_flag = dir / "bad-flag.dat";
        write_bytes(bad_flag, make_wallet_file_body(0x47454F43, 13, 2, minimal_payload));
        geocoin::wallet::Wallet bad_flag_wallet;
        expect(!bad_flag_wallet.load_from_file(bad_flag.string(), ""),
               "wallet load rejects invalid encrypted flag");

        const auto truncated_payload = dir / "truncated.dat";
        geocoin::bytes truncated_header;
        append_u32_le(truncated_header, 0x47454F43);
        append_u32_le(truncated_header, 11);
        truncated_header.push_back(0);
        append_u32_le(truncated_header, 8);
        truncated_header.push_back(0xaa);
        write_bytes(truncated_payload, truncated_header);
        geocoin::wallet::Wallet truncated_wallet;
        expect(!truncated_wallet.load_from_file(truncated_payload.string(), ""),
               "wallet load rejects truncated payload");

        geocoin::bytes oversized_header;
        append_u32_le(oversized_header, 0x47454F43);
        append_u32_le(oversized_header, 11);
        oversized_header.push_back(0);
        append_u32_le(oversized_header, 0xffffffffU);
        const auto oversized_payload = dir / "oversized-payload.dat";
        write_bytes(oversized_payload, oversized_header);
        geocoin::wallet::Wallet oversized_wallet;
        expect(!oversized_wallet.load_from_file(oversized_payload.string(), ""),
               "wallet load rejects oversized declared payload before allocation");

        const auto empty_payload = dir / "empty-payload.dat";
        write_bytes(empty_payload, make_wallet_file_body(0x47454F43, 13, 0, {}));
        geocoin::wallet::Wallet empty_payload_wallet;
        expect(!empty_payload_wallet.load_from_file(empty_payload.string(), ""),
               "wallet load rejects empty wallet payload");

        const auto truncated_legacy_verifier = dir / "truncated-legacy-verifier.dat";
        write_bytes(truncated_legacy_verifier, make_wallet_file_body(0x47454F43, 13, 0, geocoin::bytes{1}));
        geocoin::wallet::Wallet truncated_legacy_wallet;
        expect(!truncated_legacy_wallet.load_from_file(truncated_legacy_verifier.string(), ""),
               "wallet load rejects truncated legacy password verifier");

        geocoin::bytes zero_iteration_pbkdf2;
        zero_iteration_pbkdf2.push_back(2);
        zero_iteration_pbkdf2.insert(zero_iteration_pbkdf2.end(), 32, 0x11);
        zero_iteration_pbkdf2.insert(zero_iteration_pbkdf2.end(), 16, 0x22);
        append_u32_le(zero_iteration_pbkdf2, 0);
        const auto zero_iterations = dir / "zero-iterations.dat";
        write_bytes(zero_iterations, make_wallet_file_body(0x47454F43, 13, 0, zero_iteration_pbkdf2));
        geocoin::wallet::Wallet zero_iterations_wallet;
        expect(!zero_iterations_wallet.load_from_file(zero_iterations.string(), ""),
               "wallet load rejects PBKDF2 verifier with zero iterations");

        geocoin::bytes excessive_iteration_pbkdf2;
        excessive_iteration_pbkdf2.push_back(2);
        excessive_iteration_pbkdf2.insert(excessive_iteration_pbkdf2.end(), 32, 0x11);
        excessive_iteration_pbkdf2.insert(excessive_iteration_pbkdf2.end(), 16, 0x22);
        append_u32_le(excessive_iteration_pbkdf2, 0xffffffffU);
        const auto excessive_iterations = dir / "excessive-iterations.dat";
        write_bytes(
            excessive_iterations,
            make_wallet_file_body(0x47454F43, 13, 0, excessive_iteration_pbkdf2)
        );
        geocoin::wallet::Wallet excessive_iterations_wallet;
        expect(!excessive_iterations_wallet.load_from_file(excessive_iterations.string(), ""),
               "wallet load rejects unsupported PBKDF2 iteration counts before KDF work");

        geocoin::bytes missing_key_count;
        missing_key_count.push_back(0);
        const auto missing_key_count_path = dir / "missing-key-count.dat";
        write_bytes(missing_key_count_path, make_wallet_file_body(0x47454F43, 13, 0, missing_key_count));
        geocoin::wallet::Wallet missing_key_count_wallet;
        expect(!missing_key_count_wallet.load_from_file(missing_key_count_path.string(), ""),
               "wallet load rejects payload missing key count");

        geocoin::bytes missing_tx_count;
        missing_tx_count.push_back(0);
        append_u32_le(missing_tx_count, 0);
        const auto missing_tx_count_path = dir / "missing-tx-count.dat";
        write_bytes(missing_tx_count_path, make_wallet_file_body(0x47454F43, 13, 0, missing_tx_count));
        geocoin::wallet::Wallet missing_tx_count_wallet;
        expect(!missing_tx_count_wallet.load_from_file(missing_tx_count_path.string(), ""),
               "wallet load rejects payload missing transaction count");

        geocoin::bytes unknown_password_mode = minimal_payload;
        unknown_password_mode[0] = 9;
        const auto bad_password_mode = dir / "bad-password-mode.dat";
        write_bytes(bad_password_mode, make_wallet_file_body(0x47454F43, 13, 0, unknown_password_mode));
        geocoin::wallet::Wallet bad_mode_wallet;
        expect(!bad_mode_wallet.load_from_file(bad_password_mode.string(), ""),
               "wallet load rejects unknown password verifier mode");

        geocoin::bytes extra_payload = minimal_payload;
        extra_payload.push_back(0xff);
        const auto extra_bytes = dir / "extra-bytes.dat";
        write_bytes(extra_bytes, make_wallet_file_body(0x47454F43, 13, 0, extra_payload));
        geocoin::wallet::Wallet extra_wallet;
        expect(!extra_wallet.load_from_file(extra_bytes.string(), ""),
               "wallet load rejects unread trailing bytes");

        geocoin::wallet::Wallet preserved_wallet;
        expect(preserved_wallet.initialize(make_key(159)),
               "wallet used for transactional load check initializes");
        const geocoin::Address preserved_address = preserved_wallet.get_address();
        expect(!preserved_wallet.load_from_file(extra_bytes.string(), ""),
               "malformed replacement wallet is rejected");
        expect(preserved_wallet.get_key_count() == 1 &&
                   preserved_wallet.get_address() == preserved_address,
               "failed wallet load preserves the previously active wallet state");

        geocoin::wallet::Wallet participants_wallet;
        expect(participants_wallet.initialize(make_key(160)), "participants wallet initializes");
        const geocoin::Address first = participants_wallet.get_address();
        const geocoin::Address second = participants_wallet.create_new_key("participant-second");
        auto canonical_config = geocoin::wallet::create_multisig_address(2, 2, {first, second});
        geocoin::wallet::MultisigConfig config = canonical_config.config;
        config.address = geocoin::Address(canonical_config.address);
        config.participants.emplace_back(first);
        config.participants.emplace_back(second);
        expect(participants_wallet.add_multisig_address(config),
               "participants multisig config is accepted");
        const auto participants_path = dir / "participants.dat";
        expect(participants_wallet.save_to_file(participants_path.string(), ""),
               "participants wallet saves");
#if defined(__unix__) || defined(__APPLE__)
        struct stat participants_stat {};
        expect(::stat(participants_path.c_str(), &participants_stat) == 0,
               "saved wallet permissions can be inspected");
        expect((participants_stat.st_mode & 0777) == 0600,
               "saved wallet is owner-readable and owner-writable only");
#endif

        geocoin::wallet::Wallet loaded_participants;
        expect(loaded_participants.load_from_file(participants_path.string(), ""),
               "participants wallet loads");
        const auto loaded_configs = loaded_participants.get_multisig_addresses();
        expect(loaded_configs.size() == 1 && loaded_configs[0].participants.size() == 2,
               "participants multisig config roundtrips");

        geocoin::wallet::Wallet migration_wallet;
        expect(migration_wallet.initialize(make_key(165)), "plaintext migration wallet initializes");
        const auto migration_path = dir / "plaintext-to-encrypted.dat";
        expect(migration_wallet.save_to_file(migration_path.string(), ""),
               "plaintext migration wallet saves");
        expect(migration_wallet.save_to_file(migration_path.string(), "migration-pass"),
               "plaintext migration wallet encrypts in place");
        expect(!std::filesystem::exists(migration_path.string() + ".bak"),
               "successful first encryption removes plaintext backup");

        geocoin::wallet::Wallet blocked_migration_wallet;
        expect(blocked_migration_wallet.initialize(make_key(169)),
               "blocked migration wallet initializes");
        const auto blocked_migration_path = dir / "blocked-migration.dat";
        expect(blocked_migration_wallet.save_to_file(blocked_migration_path.string(), ""),
               "blocked migration starts from a plaintext wallet");
        const auto obstructing_backup = std::filesystem::path(
            blocked_migration_path.string() + ".bak"
        );
        std::filesystem::create_directories(obstructing_backup, ec);
        expect(!ec, "obstructing backup directory is created");
        {
            std::ofstream marker(obstructing_backup / "keep", std::ios::binary);
            marker << "occupied";
        }
        expect(!blocked_migration_wallet.save_to_file(
                   blocked_migration_path.string(),
                   "blocked-pass"
               ),
               "first encryption fails before commit when stale backup cannot be removed");
        expect(!blocked_migration_wallet.has_password_protection(),
               "failed backup cleanup does not commit a new password verifier");
        geocoin::wallet::Wallet still_plaintext;
        expect(still_plaintext.load_from_file(blocked_migration_path.string(), ""),
               "failed backup cleanup preserves the original wallet file");
        std::filesystem::remove_all(obstructing_backup, ec);
        ec.clear();

#if defined(__unix__) || defined(__APPLE__)
        const auto symlink_path = dir / "wallet-symlink.dat";
        std::filesystem::create_symlink(participants_path.filename(), symlink_path, ec);
        expect(!ec, "wallet symlink fixture is created");
        geocoin::wallet::Wallet symlink_wallet;
        expect(!symlink_wallet.load_from_file(symlink_path.string(), ""),
               "wallet load rejects symbolic links");
#endif

        geocoin::wallet::Wallet encrypted_wallet;
        expect(encrypted_wallet.initialize(make_key(170)), "encrypted wallet initializes");
        const auto encrypted_path = dir / "encrypted.dat";
        expect(encrypted_wallet.save_to_file(encrypted_path.string(), "fresh-pass"),
               "wallet can initialize password verifier during encrypted save");
        expect(encrypted_wallet.has_password_protection(),
               "encrypted save records password protection");
        geocoin::wallet::Wallet loaded_encrypted;
        expect(loaded_encrypted.load_from_file(encrypted_path.string(), "fresh-pass"),
               "encrypted wallet loads before tamper");

        geocoin::wallet::Wallet rekey_wallet;
        expect(rekey_wallet.initialize(make_key(171)), "rekey wallet initializes");
        const auto rekey_path = dir / "rekey.dat";
        expect(rekey_wallet.save_to_file(rekey_path.string(), "old-pass"),
               "rekey wallet saves with old password");
        expect(rekey_wallet.save_to_file(rekey_path.string(), "old-pass"),
               "rekey wallet refresh creates an encrypted recovery backup");
        expect(std::filesystem::exists(rekey_path.string() + ".bak"),
               "pre-rekey backup exists and is decryptable with the old password");
        expect(!rekey_wallet.change_password(
                   (dir / "missing-rekey-parent" / "wallet.dat").string(),
                   "old-pass",
                   "new-pass"
               ),
               "failed atomic rekey reports an unwritable destination");
        expect(rekey_wallet.verify_password("old-pass") &&
                   !rekey_wallet.verify_password("new-pass"),
               "failed atomic rekey preserves the old in-memory verifier");
        expect(rekey_wallet.change_password(rekey_path.string(), "old-pass", "new-pass"),
               "rekey wallet commits with new password");
        expect(!std::filesystem::exists(rekey_path.string() + ".bak"),
               "rekey removes backup decryptable with old password");
        geocoin::wallet::Wallet rekey_old_password;
        expect(!rekey_old_password.load_from_file(rekey_path.string(), "old-pass"),
               "rekeyed wallet rejects old password");
        geocoin::wallet::Wallet rekey_new_password;
        expect(rekey_new_password.load_from_file(rekey_path.string(), "new-pass"),
               "rekeyed wallet accepts new password");

        const auto missing_hmac_path = dir / "missing-hmac.dat";
        std::filesystem::copy_file(
            encrypted_path,
            missing_hmac_path,
            std::filesystem::copy_options::overwrite_existing,
            ec
        );
        expect(!ec, "wallet missing-HMAC fixture copies");
        const auto encrypted_size = std::filesystem::file_size(missing_hmac_path, ec);
        expect(!ec && encrypted_size > 0, "wallet missing-HMAC fixture reads size");
        std::filesystem::resize_file(missing_hmac_path, encrypted_size - 1, ec);
        expect(!ec, "wallet missing-HMAC fixture truncates");
        geocoin::wallet::Wallet missing_hmac_wallet;
        expect(!missing_hmac_wallet.load_from_file(missing_hmac_path.string(), "fresh-pass"),
               "wallet load rejects encrypted file with truncated HMAC");

        {
            std::fstream file(encrypted_path, std::ios::in | std::ios::out | std::ios::binary);
            file.seekg(-1, std::ios::end);
            char last = 0;
            file.get(last);
            last ^= 0x01;
            file.seekp(-1, std::ios::end);
            file.put(last);
            expect(static_cast<bool>(file), "wallet hmac tamper writes");
        }
        geocoin::wallet::Wallet tampered_encrypted;
        expect(!tampered_encrypted.load_from_file(encrypted_path.string(), "fresh-pass"),
               "wallet load rejects tampered encrypted HMAC");

        std::filesystem::remove_all(dir, ec);
    } catch (...) {
        std::filesystem::remove_all(dir, ec);
        throw;
    }
}

} // namespace

int main()
{
    try {
        test_wallet_application_service_boundaries();
        test_key_management_and_multisig();
        test_password_save_load_and_history();
        test_plaintext_history_load_and_backup_fallback();
        test_old_wallet_format_is_rejected();
        test_nft_wallet_builders_and_guards();
        test_nft_wallet_transfer_and_burn_use_current_owner_address();
        test_utxo_backed_transaction_paths();
        test_recent_history_discard_and_fee_floor();
        test_monetary_overflow_guards();
        test_wallet_edge_guards_and_accounting_caches();
        test_scan_and_sync_transaction_paths();
        test_wallet_file_guards_and_participant_roundtrip();
        std::cout << "test_wallet_lifecycle_units: ok\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "test_wallet_lifecycle_units failed: " << e.what() << "\n";
        return 1;
    }
}
