#include "geocoin/consensus/consensus.hpp"
#include "geocoin/core/transaction.hpp"
#include "geocoin/crypto/geo_area.hpp"
#include "geocoin/crypto/signatures.hpp"
#include "geocoin/nft/nft_types.hpp"
#include "geocoin/state/state.hpp"
#include "geocoin/state/storage.hpp"
#include "geocoin/utils/activation_manifest.hpp"
#include "geocoin/wallet/wallet.hpp"

#include <chrono>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace {
void expect(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

geocoin::bytes make_key(uint8_t seed) {
    geocoin::bytes key(32);
    for (size_t i = 0; i < key.size(); ++i) {
        key[i] = static_cast<uint8_t>(seed + i);
    }
    return key;
}

geocoin::Hash256 hash_from_tag(const std::string& tag) {
    geocoin::Hash256 out{};
    for (size_t i = 0; i < out.size(); ++i) {
        const uint8_t c = static_cast<uint8_t>(tag[i % tag.size()]);
        out[i] = static_cast<uint8_t>(c + static_cast<uint8_t>(i * 13));
    }
    return out;
}
}

int main() {
    using namespace geocoin;
    using namespace geocoin::core;

    try {
        auto chain_state = std::make_shared<state::ChainState>(std::make_unique<state::MemoryStorage>());
        chain_state->set_activation_manifest(utils::ActivationManifest::fully_enabled_for_tests("degree"));

        wallet::Wallet miner_wallet;
        wallet::Wallet recipient_wallet;
        const bytes miner_key = make_key(1);
        const bytes recipient_key = make_key(41);
        expect(miner_wallet.initialize(miner_key), "failed to initialize miner wallet");
        expect(recipient_wallet.initialize(recipient_key), "failed to initialize recipient wallet");

        const Address miner = miner_wallet.get_address();
        const Address recipient = recipient_wallet.get_address();
        const Amount fee = consensus::params::MIN_TX_FEE;
        const Amount reward = 50 * COIN;

        Transaction coinbase = Transaction::create_coinbase(1, miner, reward);
        const Amount miner_reward = coinbase.outputs.empty() ? 0 : coinbase.outputs.front().value;
        expect(chain_state->apply_transaction(coinbase, 1, true), "failed to apply coinbase");
        chain_state->set_height(120);
        expect(chain_state->get_balance(miner) == miner_reward, "unexpected miner balance after coinbase");

        Transaction payment;
        expect(
            miner_wallet.create_transaction(recipient, 10 * COIN, fee, chain_state, payment),
            "failed to create standard payment"
        );
        expect(chain_state->validate_transaction(payment), "standard payment should validate");
        expect(chain_state->apply_transaction(payment, 120, false), "failed to apply standard payment");
        chain_state->set_height(121);

        expect(
            chain_state->get_balance(miner) == miner_reward - (10 * COIN) - fee,
            "unexpected miner balance after standard payment"
        );
        expect(chain_state->get_balance(recipient) == 10 * COIN, "unexpected recipient balance after payment");

        bytes miner_privkey;
        expect(miner_wallet.get_private_key(miner, miner_privkey), "failed to get miner private key");
        bytes recipient_privkey;
        expect(recipient_wallet.get_private_key(recipient, recipient_privkey), "failed to get recipient private key");

        auto make_card_tx = [&](const Address& signer, const bytes& signer_priv) -> Transaction {
            const auto utxos = chain_state->get_utxos_for_address(signer);
            expect(!utxos.empty(), "no UTXO for card tx");
            const auto& utxo = utxos.front();
            expect(utxo.value > fee, "card tx utxo is too small");

            Transaction tx;
            tx.version = 2;
            tx.timestamp = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch()
                ).count()
            );
            TxInput in;
            in.prev_tx_hash = utxo.tx_hash;
            in.vout = utxo.vout;
            tx.inputs.push_back(in);
            tx.outputs.push_back(TxOutput{utxo.value - fee, signer, {}});
            tx.sign_input(0, signer_priv);
            return tx;
        };

        const Hash256 program_id = hash_from_tag("card-program-001");
        const Hash256 scope_id = hash_from_tag("card-scope-001");
        const Hash256 applet_pkg_hash = hash_from_tag("card-applet-pkg-v1");
        const Hash256 uid_hash = hash_from_tag("card-uid-001");
        const Hash256 pubkey_hash = hash_from_tag("card-pubkey-001");

        Transaction card_program = make_card_tx(miner, miner_privkey);
        card_program.type = TransactionType::CARD_PROGRAM_DEFINE;
        card_program.card_program_id = program_id;
        card_program.card_issuer = miner;
        card_program.card_chip_vendor = "NXP";
        card_program.card_chip_os = "JCOP4";
        card_program.card_applet_aid = "A000000001";
        card_program.card_applet_version = 1;
        card_program.card_applet_package_hash = applet_pkg_hash;
        card_program.card_policy_id = 7;
        card_program.card_scope_id = scope_id;
        card_program.sign_input(0, miner_privkey);
        expect(chain_state->validate_transaction(card_program), "CARD_PROGRAM_DEFINE should validate");
        expect(chain_state->apply_transaction(card_program, 121, false), "failed to apply CARD_PROGRAM_DEFINE");
        chain_state->set_height(122);

        Transaction card_issue = make_card_tx(miner, miner_privkey);
        card_issue.type = TransactionType::CARD_ISSUE;
        card_issue.card_program_id = program_id;
        card_issue.card_issuer = miner;
        card_issue.card_policy_id = 7;
        card_issue.card_scope_id = scope_id;
        card_issue.card_uid_hash = uid_hash;
        card_issue.card_pubkey_hash = pubkey_hash;
        card_issue.card_valid_from = 1710000000;
        card_issue.card_valid_to = 0;
        card_issue.card_status = 0;
        card_issue.card_metadata_hash = hash_from_tag("card-meta-initial");
        card_issue.sign_input(0, miner_privkey);
        expect(chain_state->validate_transaction(card_issue), "CARD_ISSUE should validate");
        expect(chain_state->apply_transaction(card_issue, 122, false), "failed to apply CARD_ISSUE");
        chain_state->set_height(123);

        Transaction bad_update = make_card_tx(recipient, recipient_privkey);
        bad_update.type = TransactionType::CARD_UPDATE_STATUS;
        bad_update.card_program_id = program_id;
        bad_update.card_issuer = recipient; // неверный эмитент
        bad_update.card_uid_hash = uid_hash;
        bad_update.card_status = 1;
        bad_update.card_metadata_hash = hash_from_tag("card-meta-bad-update");
        bad_update.sign_input(0, recipient_privkey);
        expect(!chain_state->validate_transaction(bad_update), "CARD_UPDATE_STATUS with wrong issuer must fail");

        Transaction suspend = make_card_tx(miner, miner_privkey);
        suspend.type = TransactionType::CARD_UPDATE_STATUS;
        suspend.card_program_id = program_id;
        suspend.card_issuer = miner;
        suspend.card_uid_hash = uid_hash;
        suspend.card_status = 1;
        suspend.card_metadata_hash = hash_from_tag("card-meta-suspend");
        suspend.sign_input(0, miner_privkey);
        expect(chain_state->validate_transaction(suspend), "CARD_UPDATE_STATUS suspend should validate");
        expect(chain_state->apply_transaction(suspend, 123, false), "failed to apply CARD_UPDATE_STATUS suspend");
        chain_state->set_height(124);

        Transaction revoke = make_card_tx(miner, miner_privkey);
        revoke.type = TransactionType::CARD_UPDATE_STATUS;
        revoke.card_program_id = program_id;
        revoke.card_issuer = miner;
        revoke.card_uid_hash = uid_hash;
        revoke.card_status = 2;
        revoke.card_metadata_hash = hash_from_tag("card-meta-revoke");
        revoke.sign_input(0, miner_privkey);
        expect(chain_state->validate_transaction(revoke), "CARD_UPDATE_STATUS revoke should validate");
        expect(chain_state->apply_transaction(revoke, 124, false), "failed to apply CARD_UPDATE_STATUS revoke");
        chain_state->set_height(125);

        Transaction reopen = make_card_tx(miner, miner_privkey);
        reopen.type = TransactionType::CARD_UPDATE_STATUS;
        reopen.card_program_id = program_id;
        reopen.card_issuer = miner;
        reopen.card_uid_hash = uid_hash;
        reopen.card_status = 0;
        reopen.card_metadata_hash = hash_from_tag("card-meta-reopen");
        reopen.sign_input(0, miner_privkey);
        expect(!chain_state->validate_transaction(reopen), "revoked card must be immutable");

        // В MemoryStorage нет BlockStorage-индекса блоков, поэтому get_transaction()
        // по txid здесь не используется: проверяем интеграцию по балансам/UTXO-пути.

        nft::NFTMetadata metadata;
        metadata.name = "Test NFT";
        metadata.description = "Integration smoke asset";
        metadata.image_uri = "ipfs://geocoin-test";
        metadata.content_hash = std::string(64, 'a');
        metadata.geometry.polygons.front().exterior = {
            {56000000000LL, 37000000000LL},
            {56000000000LL, 37000100000LL},
            {56000100000LL, 37000000000LL},
        };
        metadata.created_at = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()
            ).count()
        );
        metadata.block_height = 125;
        metadata.creator = miner.str();
        metadata.location_name = "Integration plot";
        metadata.tags = {"integration", "smoke"};

        Transaction nft_create;
        expect(
            miner_wallet.create_nft_create(metadata, 250, fee, chain_state, nft_create),
            "failed to create NFT_CREATE transaction"
        );
        expect(chain_state->validate_transaction(nft_create), "NFT_CREATE should validate when enabled");
        expect(chain_state->apply_transaction(nft_create, 125, false), "failed to apply NFT_CREATE");
        chain_state->set_height(126);

        const Hash256 nft_id = nft_create.nft_create->nft_id;
        expect(chain_state->nft_exists(nft_id), "NFT should exist after create");
        expect(chain_state->get_nft_owner(nft_id) == miner, "NFT owner should be miner after create");

        Transaction nft_transfer;
        expect(
            miner_wallet.create_nft_transfer(nft_id, recipient, fee, chain_state, nft_transfer),
            "failed to create NFT_TRANSFER transaction"
        );
        Transaction transfer_fee_mismatch = nft_transfer;
        ++transfer_fee_mismatch.nft_transfer->fee;
        transfer_fee_mismatch.nft_transfer->signature = crypto::sign_recoverable_message(
            crypto::sha256(transfer_fee_mismatch.nft_transfer->serialize_for_signing()),
            miner_key
        );
        transfer_fee_mismatch.sign(miner_key);
        expect(!chain_state->validate_transaction(transfer_fee_mismatch),
               "NFT_TRANSFER must bind the declared fee to the actual UTXO fee");
        expect(chain_state->get_nft_owner(nft_id) == miner,
               "rejected NFT_TRANSFER fee mismatch must not change ownership");
        expect(chain_state->validate_transaction(nft_transfer), "NFT_TRANSFER should validate");
        expect(chain_state->apply_transaction(nft_transfer, 126, false), "failed to apply NFT_TRANSFER");
        chain_state->set_height(127);
        expect(chain_state->get_nft_owner(nft_id) == recipient, "NFT owner should be recipient after transfer");

        Transaction nft_burn;
        expect(
            recipient_wallet.create_nft_burn(nft_id, fee, chain_state, nft_burn),
            "failed to create NFT_BURN transaction"
        );
        Transaction burn_fee_mismatch = nft_burn;
        ++burn_fee_mismatch.nft_burn->fee;
        burn_fee_mismatch.nft_burn->signature = crypto::sign_recoverable_message(
            crypto::sha256(burn_fee_mismatch.nft_burn->serialize_for_signing()),
            recipient_key
        );
        burn_fee_mismatch.sign(recipient_key);
        expect(!chain_state->validate_transaction(burn_fee_mismatch),
               "NFT_BURN must bind the declared fee to the actual UTXO fee");
        expect(chain_state->nft_exists(nft_id),
               "rejected NFT_BURN fee mismatch must leave the NFT intact");
        expect(chain_state->validate_transaction(nft_burn), "NFT_BURN should validate");
        expect(chain_state->apply_transaction(nft_burn, 127, false), "failed to apply NFT_BURN");
        chain_state->set_height(128);
        expect(!chain_state->nft_exists(nft_id), "NFT should be removed after burn");

        chain_state->set_activation_manifest(utils::ActivationManifest::production_defaults("degree"));
        Transaction gated_nft;
        expect(
            miner_wallet.create_nft_create(metadata, 250, fee, chain_state, gated_nft),
            "failed to create gated NFT_CREATE transaction"
        );
        expect(!chain_state->validate_transaction(gated_nft), "production manifest should reject NFT tx");

        std::cout << "test_wallet_chainstate_integration: ok\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "test_wallet_chainstate_integration failed: " << e.what() << "\n";
        return 1;
    }
}
