#include "geocoin/consensus/consensus.hpp"
#include "geocoin/core/transaction.hpp"
#include "geocoin/crypto/addresses.hpp"
#include "geocoin/network/mempool.hpp"
#include "geocoin/nft/nft_types.hpp"
#include "geocoin/rpc/rpc_encoding.hpp"
#include "geocoin/rpc/rpc_server.hpp"
#include "geocoin/state/state.hpp"
#include "geocoin/utils/bytes.hpp"
#include "geocoin/state/storage.hpp"
#include "geocoin/types.hpp"
#include "geocoin/utils/activation_manifest.hpp"
#include "geocoin/wallet/multisig.hpp"
#include "rpc_test_utils.hpp"
#include "test_chain_mutation_service.hpp"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using geocoin::test::json;
using geocoin::test::expect_true;
using geocoin::test::find_free_port;
using geocoin::test::send_rpc;

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
    expect_true(geocoin::crypto::compute_address_from_private_key(make_key(seed), address), "address derives");
    return address;
}

geocoin::Hash256 hash_from_seed(uint8_t seed)
{
    geocoin::Hash256 hash{};
    for (size_t i = 0; i < hash.size(); ++i) {
        hash[i] = static_cast<uint8_t>(seed + i * 17);
    }
    return hash;
}

std::string hex_hash(uint8_t seed)
{
    return geocoin::rpc::hash256_to_hex_string(hash_from_seed(seed));
}

std::string key_hex(uint8_t seed)
{
    return geocoin::rpc::bytes_to_hex_string(make_key(seed));
}

void append_u32_le(geocoin::bytes& out, uint32_t value)
{
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<geocoin::byte>((value >> (i * 8)) & 0xFFu));
    }
}

void append_u64_le(geocoin::bytes& out, uint64_t value)
{
    for (int i = 0; i < 8; ++i) {
        out.push_back(static_cast<geocoin::byte>((value >> (i * 8)) & 0xFFu));
    }
}

void append_hash(geocoin::bytes& out, const geocoin::Hash256& hash)
{
    out.insert(out.end(), hash.begin(), hash.end());
}

void append_string(geocoin::bytes& out, const std::string& value)
{
    append_u32_le(out, static_cast<uint32_t>(value.size()));
    out.insert(out.end(), value.begin(), value.end());
}

geocoin::bytes serialize_sidechain_deposit_for_test(const geocoin::state::SidechainDeposit& deposit)
{
    geocoin::bytes out;
    append_string(out, deposit.owner.str());
    append_u64_le(out, static_cast<uint64_t>(deposit.amount));
    append_u64_le(out, static_cast<uint64_t>(deposit.unlock_height));
    append_hash(out, deposit.checkpoint);
    append_u64_le(out, static_cast<uint64_t>(deposit.last_checkpoint_height));
    append_u64_le(out, static_cast<uint64_t>(deposit.last_sidechain_checkpoint_height));
    return out;
}

geocoin::bytes serialize_sidechain_registry_for_test(const geocoin::state::SidechainRegistryEntry& entry)
{
    geocoin::bytes out;
    append_string(out, entry.owner.str());
    append_u64_le(out, static_cast<uint64_t>(entry.issuance_limit));
    append_u64_le(out, static_cast<uint64_t>(entry.total_issued));
    append_u32_le(out, entry.multiplier);
    append_u32_le(out, entry.fee_bps);
    append_u32_le(out, entry.max_multiplier);
    out.push_back(entry.checkpoint_policy_id);
    out.push_back(entry.checkpoint_version);
    append_string(out, entry.token_symbol);
    out.push_back(entry.committee_m);
    out.push_back(entry.committee_n);
    append_u32_le(out, static_cast<uint32_t>(entry.committee_members.size()));
    for (const auto& member : entry.committee_members) {
        append_string(out, member.str());
    }
    append_hash(out, entry.committee_id);
    out.insert(out.end(), {0xA3, 1, static_cast<uint8_t>(entry.kind), static_cast<uint8_t>(entry.status)});
    append_u64_le(out, entry.decommission_height);
    append_string(out, entry.accounting_namespace);
    append_hash(out, entry.accounting_schema_hash);
    return out;
}

json call_ok(uint16_t port, const std::string& method, const json& object_param)
{
    const auto response = send_rpc(port, "admin", "admin-pass", method, json::array({object_param}));
    expect_true(response.status == 200, "RPC handler should return JSON-RPC HTTP 200");
    if (response.body.contains("error")) {
        throw std::runtime_error(method + " unexpectedly failed: " + response.body["error"].dump());
    }
    return response.body["result"];
}

json call_ok_params(uint16_t port, const std::string& method, const json& params)
{
    const auto response = send_rpc(port, "admin", "admin-pass", method, params);
    expect_true(response.status == 200, "RPC handler should return JSON-RPC HTTP 200");
    if (response.body.contains("error")) {
        throw std::runtime_error(method + " unexpectedly failed: " + response.body["error"].dump());
    }
    return response.body["result"];
}

void call_error(uint16_t port, const std::string& method, const json& object_param, int expected_code)
{
    const auto response = send_rpc(port, "admin", "admin-pass", method, json::array({object_param}));
    expect_true(response.status == 200, "RPC handler error should return JSON-RPC HTTP 200");
    expect_true(response.body.contains("error"), "RPC handler should return JSON-RPC error");
    expect_true(response.body["error"]["code"].get<int>() == expected_code, "RPC error code should match");
}

void test_raw_transaction_builder_guards(uint16_t port)
{
    const json inputs = json::array({json{
        {"txid", std::string(64, '0')},
        {"vout", 0},
    }});
    const json outputs = json::array({json{
        {"address", address_from_key(99).str()},
        {"value_angles", geocoin::consensus::params::DUST_LIMIT},
    }});

    const auto overflow = send_rpc(
        port,
        "admin",
        "admin-pass",
        "createrawtransaction",
        json::array({inputs, outputs, uint64_t{1} + std::numeric_limits<uint32_t>::max()})
    );
    expect_true(overflow.status == 200 && overflow.body.contains("error"),
                "raw builder rejects locktime above uint32");
    expect_true(overflow.body["error"]["code"].get<int>() == geocoin::rpc::RPC_INVALID_PARAMETER,
                "raw builder locktime overflow uses invalid-parameter error");

    const auto invalid_address = send_rpc(
        port,
        "admin",
        "admin-pass",
        "createrawtransaction",
        json::array({inputs, json::array({json{
            {"address", "GEO-invalid-address"},
            {"value_angles", geocoin::consensus::params::DUST_LIMIT},
        }})})
    );
    expect_true(invalid_address.status == 200 && invalid_address.body.contains("error"),
                "raw builder rejects non-canonical destination address");
    expect_true(invalid_address.body["error"]["code"].get<int>() ==
                    geocoin::rpc::RPC_INVALID_ADDRESS_OR_KEY,
                "raw builder invalid address uses address error");
}

void test_raw_transaction_finality_defaults()
{
    auto state = std::make_shared<geocoin::state::ChainState>(
        std::make_unique<geocoin::state::MemoryStorage>());
    geocoin::rpc::RPCServer server;
    expect_true(server.set_context(geocoin::rpc::RPCServerContext{.chain_state = state}),
                "install state for verbose raw decoding");
    const uint16_t port = find_free_port();
    expect_true(server.start("127.0.0.1", port, "admin", "admin-pass"), "start finality RPC server");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const json inputs = json::array({json{{"txid", std::string(64, '0')}, {"vout", 0}}});
    const json outputs = json::array({json{{"address", address_from_key(99).str()},
        {"value_angles", geocoin::consensus::params::DUST_LIMIT}}});
    for (uint32_t lock_time : {uint32_t{0}, uint32_t{7}, uint32_t{500000000}, uint32_t{0xFFFFFFFFu}}) {
        const auto raw = call_ok_params(port, "createrawtransaction", json::array({inputs, outputs, lock_time}));
        const auto decoded = call_ok_params(port, "decoderawtransaction", json::array({raw}));
        expect_true(decoded["lock_time"] == lock_time, "RPC round trip preserves lock_time");
        expect_true(decoded["inputs"][0]["sequence"] == (lock_time == 0 ? 0xFFFFFFFFu : 0xFFFFFFFEu),
                    "RPC automatically enables a supplied locktime");
    }
    auto explicit_inputs = inputs;
    explicit_inputs[0]["sequence"] = 0xFFFFFFFFu;
    auto raw = call_ok_params(port, "createrawtransaction", json::array({explicit_inputs, outputs, 7}));
    auto decoded = call_ok_params(port, "decoderawtransaction", json::array({raw}));
    expect_true(decoded["lock_time"] == 7 && decoded["inputs"][0]["sequence"] == 0xFFFFFFFFu,
                "explicit FINAL sequence is preserved even with locktime");
    explicit_inputs.push_back(json{{"txid", std::string(64, '1')}, {"vout", 1}});
    raw = call_ok_params(port, "createrawtransaction", json::array({explicit_inputs, outputs, 7}));
    decoded = call_ok_params(port, "decoderawtransaction", json::array({raw}));
    expect_true(decoded["inputs"][0]["sequence"] == 0xFFFFFFFFu &&
                    decoded["inputs"][1]["sequence"] == 0xFFFFFFFEu,
                "each omitted input sequence enables the lock without changing explicit values");

    server.stop();
}

void call_error_params(uint16_t port, const std::string& method, const json& params, int expected_code)
{
    const auto response = send_rpc(port, "admin", "admin-pass", method, params);
    expect_true(response.status == 200, "RPC handler error should return JSON-RPC HTTP 200");
    expect_true(response.body.contains("error"), "RPC handler should return JSON-RPC error");
    expect_true(response.body["error"]["code"].get<int>() == expected_code, "RPC error code should match");
}

void test_nft_payload_handlers(uint16_t port)
{
    const auto creator = address_from_key(1).str();
    const json metadata = {
        {"name", "parcel"},
        {"description", "desc"},
        {"image_uri", "ipfs://image"},
        {"content_hash", std::string(64, 'a')},
        {"geometry", {{"parts", json::array({json{{"exterior", json::array({
            json{{"lat_ndeg", 1000000}, {"lon_ndeg", 2000000}},
            json{{"lat_ndeg", 1000000}, {"lon_ndeg", 4000000}},
            json{{"lat_ndeg", 5000000}, {"lon_ndeg", 2000000}}
        })}, {"holes", json::array()}}})}}},
        {"area_bits", 123},
        {"created_at", 456},
        {"block_height", 7},
        {"block_hash", hex_hash(2)},
        {"location_name", "loc"},
        {"tags", json::array({"land", "test"})},
        {"creator", creator},
        {"cadastral", json{
            {"cadastral_number", "77:01"},
            {"owner_name", "owner"},
            {"land_category", "cat"},
            {"permitted_use", "use"},
            {"area_sqcm_official", 100},
            {"ownership_document_hash", hex_hash(3)},
            {"technical_plan_hash", hex_hash(4)},
            {"photo_hashes", json::array({hex_hash(5)})},
            {"market_value_angles", 1000},
            {"cadastral_value_angles", 900},
            {"last_valuation_date", 800},
            {"restrictions", json::array({"none"})},
            {"has_buildings", true},
            {"building_count", 2},
        }},
    };

    json create_result = call_ok(port, "createnftcreatepayload", json{
        {"metadata", metadata},
        {"creator_address", creator},
        {"royalty_percent", 500},
    });
    expect_true(create_result["creator_address"] == creator, "NFT create payload returns creator");
    expect_true(create_result["payload_to_sign_hex"].get<std::string>().size() > 10,
                "NFT create payload returns signing payload");

    call_error(port, "createnftcreatepayload", json{{"creator_address", creator}}, geocoin::rpc::RPC_INVALID_PARAMETER);
    call_error(port, "createnftcreatepayload", json{{"metadata", metadata}}, geocoin::rpc::RPC_INVALID_PARAMETER);
    json bad_meta = metadata;
    bad_meta["area_bits"] = "bad";
    call_error(port, "createnftcreatepayload", json{{"metadata", bad_meta}, {"creator_address", creator}},
               geocoin::rpc::RPC_INVALID_PARAMETER);
    json oversized_meta = metadata;
    oversized_meta["name"] = std::string(geocoin::consensus::params::MAX_NFT_NAME + 1, 'x');
    call_error(
        port,
        "createnftcreatepayload",
        json{{"metadata", oversized_meta}, {"creator_address", creator}},
        geocoin::rpc::RPC_INVALID_PARAMETER
    );

    json transfer = call_ok(port, "createnfttransferpayload", json{
        {"nft_id", hex_hash(6)},
        {"from", creator},
        {"to", address_from_key(2).str()},
        {"fee", 12},
    });
    expect_true(transfer["nft_id"] == hex_hash(6), "NFT transfer payload returns nft id");
    call_error(port, "createnfttransferpayload", json{{"nft_id", "bad"}}, geocoin::rpc::RPC_INVALID_PARAMETER);

    json burn = call_ok(port, "createnftburnpayload", json{
        {"nft_id", hex_hash(7)},
        {"owner", creator},
        {"fee", 13},
    });
    expect_true(burn["owner"] == creator, "NFT burn payload returns owner");
    call_error(port, "createnftburnpayload", json{{"nft_id", "bad"}}, geocoin::rpc::RPC_INVALID_PARAMETER);

    geocoin::nft::NFTMetadata create_metadata;
    create_metadata.name = "rpc-finalize";
    create_metadata.description = "RPC finalize NFT coverage";
    create_metadata.image_uri = "ipfs://rpc-finalize";
    create_metadata.content_hash = std::string(64, 'b');
    create_metadata.geometry.polygons.front().exterior = {{10000000, 10000000}, {10000000, 20000000}, {20000000, 10000000}};
    create_metadata.created_at = 1700000000;
    create_metadata.block_height = 1;
    create_metadata.block_hash = hash_from_seed(8);
    create_metadata.creator = creator;

    geocoin::nft::NFTCreationTransaction create_payload;
    create_payload.metadata = create_metadata;
    create_payload.nft_id = create_metadata.calculate_hash();
    create_payload.creator_address = geocoin::Address(creator);
    create_payload.royalty_percent = 100;
    geocoin::core::Transaction create_tx;
    create_tx.type = geocoin::core::TransactionType::NFT_CREATE;
    create_tx.nft_create = create_payload;

    const std::string signature_hex(20, 'a');
    const json finalized_create = call_ok_params(
        port,
        "finalizenftcreate",
        json::array({geocoin::rpc::bytes_to_hex_string(create_tx.serialize()), signature_hex})
    );
    const auto decoded_create = geocoin::test::decode_raw_tx_or_throw(finalized_create["tx_hex"]);
    expect_true(decoded_create.type == geocoin::core::TransactionType::NFT_CREATE &&
                    decoded_create.nft_create.has_value() &&
                    decoded_create.nft_create->signature.size() == 10,
                "finalizenftcreate attaches signature through RPC");

    geocoin::nft::NFTTransferTransaction transfer_payload;
    transfer_payload.nft_id = hash_from_seed(9);
    transfer_payload.from = geocoin::Address(creator);
    transfer_payload.to = address_from_key(2);
    transfer_payload.fee = 12;
    geocoin::core::Transaction transfer_tx;
    transfer_tx.type = geocoin::core::TransactionType::NFT_TRANSFER;
    transfer_tx.nft_transfer = transfer_payload;
    const json finalized_transfer = call_ok_params(
        port,
        "finalizenfttransfer",
        json::array({geocoin::rpc::bytes_to_hex_string(transfer_tx.serialize()), signature_hex})
    );
    const auto decoded_transfer = geocoin::test::decode_raw_tx_or_throw(finalized_transfer["tx_hex"]);
    expect_true(decoded_transfer.type == geocoin::core::TransactionType::NFT_TRANSFER &&
                    decoded_transfer.nft_transfer.has_value() &&
                    decoded_transfer.nft_transfer->signature.size() == 10,
                "finalizenfttransfer attaches signature through RPC");

    geocoin::nft::NFTBurnTransaction burn_payload;
    burn_payload.nft_id = hash_from_seed(10);
    burn_payload.owner = geocoin::Address(creator);
    burn_payload.fee = 13;
    geocoin::core::Transaction burn_tx;
    burn_tx.type = geocoin::core::TransactionType::NFT_BURN;
    burn_tx.nft_burn = burn_payload;
    const json finalized_burn = call_ok_params(
        port,
        "finalizenftburn",
        json::array({geocoin::rpc::bytes_to_hex_string(burn_tx.serialize()), signature_hex})
    );
    const auto decoded_burn = geocoin::test::decode_raw_tx_or_throw(finalized_burn["tx_hex"]);
    expect_true(decoded_burn.type == geocoin::core::TransactionType::NFT_BURN &&
                    decoded_burn.nft_burn.has_value() &&
                    decoded_burn.nft_burn->signature.size() == 10,
                "finalizenftburn attaches signature through RPC");
    call_error_params(
        port,
        "finalizenftcreate",
        json::array({geocoin::rpc::bytes_to_hex_string(transfer_tx.serialize()), signature_hex}),
        geocoin::rpc::RPC_INVALID_PARAMETER
    );
    call_error_params(
        port,
        "finalizenftburn",
        json::array({geocoin::rpc::bytes_to_hex_string(burn_tx.serialize()), "aa"}),
        geocoin::rpc::RPC_INVALID_PARAMETER
    );
}

void test_checkpoint_proof_handler(uint16_t port)
{
    json proof = call_ok(port, "createcheckpointproofv2", json{
        {"sidechain_id", hex_hash(20)},
        {"state_root", hex_hash(21)},
        {"checkpoint_height", 1},
        {"total_supply", 1000},
        {"leaf_index", 0},
        {"merkle_path", json::array()},
        {"verify", false},
    });
    expect_true(proof["sidechain_id"] == hex_hash(20), "checkpoint proof returns sidechain id");
    expect_true(proof["merkle_depth"] == 0, "checkpoint proof returns merkle depth");

    json verified_single_leaf = call_ok(port, "createcheckpointproofv2", json{
        {"sidechain_id", hex_hash(20)},
        {"state_root", proof["leaf_hash"]},
        {"checkpoint_height", 1},
        {"total_supply", 1000},
        {"leaf_index", 0},
        {"merkle_path", json::array()},
        {"verify", true},
    });
    expect_true(verified_single_leaf["merkle_depth"] == 0,
                "checkpoint proof verifies a single-leaf merkle path");
    expect_true(verified_single_leaf["state_root"] == proof["leaf_hash"],
                "single-leaf checkpoint proof uses the supply leaf as state_root");

    json accounting_proof = call_ok(port, "createaccountingcheckpointproofv1", json{
        {"sidechain_id", hex_hash(24)},
        {"state_root", hex_hash(25)},
        {"checkpoint_height", 9},
        {"schema_hash", hex_hash(26)},
    });
    expect_true(accounting_proof["sidechain_kind"] == "accounting",
                "accounting checkpoint proof reports kind");
    expect_true(accounting_proof["checkpoint_version"] == 1,
                "accounting checkpoint proof reports version 1");
    expect_true(accounting_proof["proof_size"] == 80,
                "accounting checkpoint proof has canonical fixed size");
    call_error(port, "createaccountingcheckpointproofv1", json{
        {"sidechain_id", hex_hash(24)},
        {"state_root", hex_hash(25)},
        {"checkpoint_height", 9},
        {"schema_hash", std::string(64, '0')},
    }, geocoin::rpc::RPC_INVALID_PARAMETER);

    call_error(port, "createcheckpointproofv2", json::object(), geocoin::rpc::RPC_INVALID_PARAMETER);
    call_error(port, "createcheckpointproofv2", json{
        {"sidechain_id", "bad"},
        {"state_root", hex_hash(21)},
        {"checkpoint_height", 1},
        {"total_supply", 1000},
        {"leaf_index", 0},
        {"merkle_path", json::array()},
    }, geocoin::rpc::RPC_INVALID_PARAMETER);
    call_error(port, "createcheckpointproofv2", json{
        {"sidechain_id", hex_hash(20)},
        {"genesis_hash", hex_hash(22)},
        {"state_root", hex_hash(21)},
        {"checkpoint_height", 1},
        {"total_supply", 1000},
        {"leaf_index", 0},
        {"merkle_path", json::array()},
    }, geocoin::rpc::RPC_INVALID_PARAMETER);
    call_error(port, "createcheckpointproofv2", json{
        {"sidechain_id", hex_hash(20)},
        {"state_root", hex_hash(21)},
        {"checkpoint_height", 0},
        {"total_supply", 1000},
        {"leaf_index", 0},
        {"merkle_path", json::array()},
    }, geocoin::rpc::RPC_INVALID_PARAMETER);
    call_error(port, "createcheckpointproofv2", json{
        {"sidechain_id", hex_hash(20)},
        {"state_root", hex_hash(21)},
        {"checkpoint_height", 1},
        {"total_supply", 1000},
        {"leaf_index", 1},
        {"merkle_path", json::array()},
    }, geocoin::rpc::RPC_INVALID_PARAMETER);
    call_error(port, "createcheckpointproofv2", json{
        {"sidechain_id", hex_hash(20)},
        {"state_root", hex_hash(21)},
        {"checkpoint_height", 1},
        {"total_supply", 1000},
        {"leaf_index", 0},
        {"merkle_path", json::array({hex_hash(23)})},
        {"verify", true},
    }, geocoin::rpc::RPC_INVALID_PARAMETER);
}

void test_sidechain_issue_handler(uint16_t port)
{
    const auto owner = address_from_key(30).str();
    const auto member1 = address_from_key(31).str();
    const auto member2 = address_from_key(32).str();
    const uint32_t min_multiplier = geocoin::consensus::params::MIN_SIDECHAIN_MULTIPLIER;

    json issue = call_ok(port, "createsidechainissuepayload", json{
        {"genesis_hash", hex_hash(40)},
        {"owner_address", owner},
        {"token_symbol", "TOK"},
        {"multiplier", min_multiplier},
        {"fee_bps", 100},
        {"max_multiplier", min_multiplier},
        {"committee_m", 1},
        {"committee_members", json::array({member1, member2})},
        {"checkpoint_policy_id", 2},
        {"checkpoint_version", 2},
        {"validate", false},
    });
    expect_true(issue["token_symbol"] == "TOK", "sidechain issue payload returns token symbol");
    expect_true(issue["committee_n"] == 2, "sidechain issue payload returns committee n");

    json accounting = call_ok(port, "createaccountingsidechainpayload", json{
        {"genesis_hash", hex_hash(41)},
        {"owner_address", owner},
        {"namespace", "land-registry"},
        {"schema_hash", hex_hash(42)},
        {"committee_m", 1},
        {"committee_members", json::array({member1, member2})},
        {"validate", false},
    });
    expect_true(accounting["sidechain_kind"] == "accounting",
                "accounting registration reports kind");
    expect_true(accounting["checkpoint_policy_id"] == 3 &&
                    accounting["checkpoint_version"] == 1,
                "accounting registration fixes checkpoint policy 3/1");
    const auto accounting_tx = geocoin::test::decode_raw_tx_or_throw(
        accounting["tx_hex"].get<std::string>()
    );
    expect_true(accounting_tx.type ==
                    geocoin::core::TransactionType::REGISTER_ACCOUNTING_SIDECHAIN &&
                    accounting_tx.sidechain_namespace == "land-registry",
                "accounting registration roundtrips through RPC");

    call_error(port, "createsidechainissuepayload", json::object(), geocoin::rpc::RPC_INVALID_PARAMETER);
    call_error(port, "createsidechainissuepayload", json{
        {"genesis_hash", "bad"},
        {"owner_address", owner},
        {"token_symbol", "TOK"},
        {"multiplier", min_multiplier},
        {"committee_m", 1},
        {"committee_members", json::array({member1})},
        {"validate", false},
    }, geocoin::rpc::RPC_INVALID_PARAMETER);
    call_error(port, "createsidechainissuepayload", json{
        {"genesis_hash", hex_hash(40)},
        {"owner_address", owner},
        {"token_symbol", ""},
        {"multiplier", min_multiplier},
        {"committee_m", 1},
        {"committee_members", json::array({member1})},
        {"validate", false},
    }, geocoin::rpc::RPC_INVALID_PARAMETER);
    call_error(port, "createsidechainissuepayload", json{
        {"genesis_hash", hex_hash(40)},
        {"owner_address", owner},
        {"token_symbol", "TO\nK"},
        {"multiplier", min_multiplier},
        {"committee_m", 1},
        {"committee_members", json::array({member1})},
        {"validate", false},
    }, geocoin::rpc::RPC_INVALID_PARAMETER);
    call_error(port, "createsidechainissuepayload", json{
        {"genesis_hash", hex_hash(40)},
        {"owner_address", owner},
        {"token_symbol", "TOK"},
        {"multiplier", min_multiplier > 0 ? min_multiplier - 1 : 0},
        {"committee_m", 1},
        {"committee_members", json::array({member1})},
        {"validate", false},
    }, geocoin::rpc::RPC_INVALID_PARAMETER);
}

json sidechain_base(const std::string& owner, uint8_t seed)
{
    return json{
        {"sidechain_id", hex_hash(seed)},
        {"owner_address", owner},
        {"validate", false},
    };
}

json tx_input(uint8_t seed, uint32_t vout = 0)
{
    return json{
        {"txid", hex_hash(seed)},
        {"vout", vout},
        {"sequence", 0xFFFFFFFEu},
        {"script_sig_hex", "abcd"},
    };
}

json tx_output(const std::string& address, uint64_t value)
{
    return json{
        {"address", address},
        {"value", value},
    };
}

void test_sidechain_deposit_handlers(uint16_t port)
{
    const auto owner = address_from_key(80).str();
    const auto recipient = address_from_key(81).str();
    const std::string owner_sig_hex(20, 'b');

    json lock = sidechain_base(owner, 82);
    lock["amount"] = 1000;
    lock["unlock_height"] = 10;
    lock["inputs"] = json::array({tx_input(83, 1)});
    lock["outputs"] = json::array({tx_output(recipient, 900)});
    lock["checkpoint"] = hex_hash(84);
    json lock_out = call_ok(port, "createsidechainlockpayload", lock);
    expect_true(lock_out["amount"] == 1000, "lock payload returns amount");
    expect_true(lock_out["unlock_height"] == 10, "lock payload returns unlock height");
    expect_true(call_ok_params(port, "finalizesidechainlock",
                               json::array({lock_out["tx_hex"], owner_sig_hex}))["tx_hex"].get<std::string>().size() >=
                    lock_out["tx_hex"].get<std::string>().size(),
                "finalize lock returns tx hex");

    json topup = sidechain_base(owner, 82);
    topup["amount"] = 250;
    topup["inputs"] = json::array({tx_input(84, 2)});
    topup["outputs"] = json::array({tx_output(recipient, 50)});
    json topup_out = call_ok(port, "createsidechaintopuppayload", topup);
    expect_true(topup_out["amount"] == 250, "top-up payload returns amount");
    const auto topup_tx = geocoin::test::decode_raw_tx_or_throw(
        topup_out["tx_hex"].get<std::string>()
    );
    expect_true(topup_tx.type ==
                    geocoin::core::TransactionType::TOP_UP_SIDECHAIN_DEPOSIT,
                "top-up payload uses dedicated transaction type");

    json lock_missing = sidechain_base(owner, 82);
    call_error(port, "createsidechainlockpayload", lock_missing, geocoin::rpc::RPC_INVALID_PARAMETER);
    json lock_empty_inputs = lock;
    lock_empty_inputs["inputs"] = json::array();
    call_error(port, "createsidechainlockpayload", lock_empty_inputs, geocoin::rpc::RPC_INVALID_PARAMETER);
    json lock_bad_input = lock;
    lock_bad_input["inputs"] = json::array({json{{"txid", "bad"}, {"vout", 0}}});
    call_error(port, "createsidechainlockpayload", lock_bad_input, geocoin::rpc::RPC_INVALID_PARAMETER);
    json lock_bad_output = lock;
    lock_bad_output["outputs"] = json::array({json{{"address", recipient}}});
    call_error(port, "createsidechainlockpayload", lock_bad_output, geocoin::rpc::RPC_INVALID_PARAMETER);

    json spend = sidechain_base(owner, 85);
    spend["amount"] = 700;
    spend["outputs"] = json::array({tx_output(recipient, 700)});
    json spend_out = call_ok(port, "createsidechainspendpayload", spend);
    expect_true(spend_out["amount"] == 700, "spend payload returns amount");
    expect_true(call_ok_params(port, "finalizesidechainspend",
                               json::array({spend_out["tx_hex"], owner_sig_hex}))["tx_hex"].get<std::string>().size() >=
                    spend_out["tx_hex"].get<std::string>().size(),
                "finalize spend returns tx hex");
    json spend_mismatch = spend;
    spend_mismatch["outputs"] = json::array({tx_output(recipient, 699)});
    call_error(port, "createsidechainspendpayload", spend_mismatch, geocoin::rpc::RPC_INVALID_PARAMETER);
    json spend_empty_outputs = spend;
    spend_empty_outputs["outputs"] = json::array();
    call_error(port, "createsidechainspendpayload", spend_empty_outputs, geocoin::rpc::RPC_INVALID_PARAMETER);

    json unlock = sidechain_base(owner, 86);
    unlock["outputs"] = json::array({tx_output(recipient, 300)});
    json unlock_out = call_ok(port, "createsidechainunlockpayload", unlock);
    expect_true(unlock_out["sidechain_id"] == hex_hash(86), "unlock payload returns sidechain id");
    expect_true(call_ok_params(port, "finalizesidechainunlock",
                               json::array({unlock_out["tx_hex"], owner_sig_hex}))["tx_hex"].get<std::string>().size() >=
                    unlock_out["tx_hex"].get<std::string>().size(),
                "finalize unlock returns tx hex");
    json unlock_missing_outputs = sidechain_base(owner, 86);
    call_error(port, "createsidechainunlockpayload", unlock_missing_outputs, geocoin::rpc::RPC_INVALID_PARAMETER);
    json unlock_empty_outputs = unlock;
    unlock_empty_outputs["outputs"] = json::array();
    call_error(port, "createsidechainunlockpayload", unlock_empty_outputs, geocoin::rpc::RPC_INVALID_PARAMETER);
}

void test_sidechain_checkpoint_and_slash_handlers(uint16_t port)
{
    const auto owner = address_from_key(90).str();
    const auto recipient = address_from_key(91).str();

    json checkpoint = sidechain_base(owner, 92);
    checkpoint["checkpoint"] = hex_hash(93);
    checkpoint["checkpoint_height"] = 5;
    checkpoint["committee_id"] = hex_hash(94);
    checkpoint["fee"] = 1000;
    json checkpoint_out = call_ok(port, "createsidechaincheckpointpayload", checkpoint);
    expect_true(checkpoint_out["checkpoint"] == hex_hash(93), "single checkpoint returns checkpoint");
    expect_true(checkpoint_out["committee_id"] == hex_hash(94), "single checkpoint returns committee id");

    json checkpoint_weight = sidechain_base(owner, 95);
    checkpoint_weight["checkpoint"] = hex_hash(96);
    checkpoint_weight["checkpoint_height"] = 6;
    checkpoint_weight["committee_id"] = hex_hash(97);
    checkpoint_weight["total_weight_bytes"] = 2048;
    expect_true(call_ok(port, "createsidechaincheckpointpayload", checkpoint_weight)["fee"].get<uint64_t>() > 0,
                "single checkpoint can compute fee from weight");

    json checkpoint_missing = sidechain_base(owner, 92);
    call_error(port, "createsidechaincheckpointpayload", checkpoint_missing, geocoin::rpc::RPC_INVALID_PARAMETER);
    json checkpoint_bad_hex = checkpoint;
    checkpoint_bad_hex["checkpoint"] = "bad";
    call_error(port, "createsidechaincheckpointpayload", checkpoint_bad_hex, geocoin::rpc::RPC_INVALID_PARAMETER);
    json checkpoint_no_committee = checkpoint;
    checkpoint_no_committee.erase("committee_id");
    call_error(port, "createsidechaincheckpointpayload", checkpoint_no_committee, geocoin::rpc::RPC_INVALID_PARAMETER);

    json batch = {
        {"validate", false},
        {"items", json::array({
            json{
                {"sidechain_id", hex_hash(100)},
                {"checkpoint", hex_hash(101)},
                {"checkpoint_height", 7},
                {"fee", 1000},
                {"committee_id", hex_hash(102)},
                {"version", 1},
                {"proof_hex", "abcd"},
            },
            json{
                {"genesis_hash", hex_hash(103)},
                {"checkpoint", hex_hash(104)},
                {"checkpoint_height", 8},
                {"fee", 2000},
                {"committee_id", hex_hash(105)},
                {"version", 2},
                {"proof_hex", "abcd"},
            },
        })},
    };
    json batch_out = call_ok(port, "createbatchsidechaincheckpointpayload", batch);
    expect_true(batch_out["payloads"].size() == 2, "batch checkpoint returns per-item payloads");
    json finalized_batch = call_ok_params(
        port,
        "finalizebatchsidechaincheckpoint",
        json::array({
            batch_out["tx_hex"],
            json::array({json::array({"abcd"}), json::array({"0102", "0304"})}),
        })
    );
    expect_true(finalized_batch["tx_hex"].get<std::string>().size() >=
                    batch_out["tx_hex"].get<std::string>().size(),
                "finalize batch checkpoint returns tx hex");

    call_error(port, "createbatchsidechaincheckpointpayload", json::object(), geocoin::rpc::RPC_INVALID_PARAMETER);
    call_error(port, "createbatchsidechaincheckpointpayload", json{{"items", 1}},
               geocoin::rpc::RPC_INVALID_PARAMETER);
    json batch_bad_item = batch;
    batch_bad_item["items"] = json::array({1});
    call_error(port, "createbatchsidechaincheckpointpayload", batch_bad_item, geocoin::rpc::RPC_INVALID_PARAMETER);
    json batch_missing_id = batch;
    batch_missing_id["items"][0].erase("sidechain_id");
    call_error(port, "createbatchsidechaincheckpointpayload", batch_missing_id, geocoin::rpc::RPC_INVALID_PARAMETER);
    json batch_bad_hex = batch;
    batch_bad_hex["items"][0]["sidechain_id"] = "bad";
    call_error(port, "createbatchsidechaincheckpointpayload", batch_bad_hex, geocoin::rpc::RPC_INVALID_PARAMETER);
    json batch_bad_genesis = batch;
    batch_bad_genesis["items"][0].erase("sidechain_id");
    batch_bad_genesis["items"][0]["genesis_hash"] = "bad";
    call_error(port, "createbatchsidechaincheckpointpayload", batch_bad_genesis, geocoin::rpc::RPC_INVALID_PARAMETER);
    json batch_mismatched_genesis = batch;
    batch_mismatched_genesis["items"][0]["genesis_hash"] = hex_hash(106);
    call_error(port, "createbatchsidechaincheckpointpayload", batch_mismatched_genesis,
               geocoin::rpc::RPC_INVALID_PARAMETER);
    json batch_missing_checkpoint = batch;
    batch_missing_checkpoint["items"][0].erase("checkpoint");
    call_error(port, "createbatchsidechaincheckpointpayload", batch_missing_checkpoint,
               geocoin::rpc::RPC_INVALID_PARAMETER);
    json batch_bad_checkpoint = batch;
    batch_bad_checkpoint["items"][0]["checkpoint"] = "bad";
    call_error(port, "createbatchsidechaincheckpointpayload", batch_bad_checkpoint,
               geocoin::rpc::RPC_INVALID_PARAMETER);
    json batch_missing_height = batch;
    batch_missing_height["items"][0].erase("checkpoint_height");
    call_error(port, "createbatchsidechaincheckpointpayload", batch_missing_height,
               geocoin::rpc::RPC_INVALID_PARAMETER);
    json batch_bad_height = batch;
    batch_bad_height["items"][0]["checkpoint_height"] = 0;
    call_error(port, "createbatchsidechaincheckpointpayload", batch_bad_height,
               geocoin::rpc::RPC_INVALID_PARAMETER);
    json batch_bad_committee = batch;
    batch_bad_committee["items"][0]["committee_id"] = "bad";
    call_error(port, "createbatchsidechaincheckpointpayload", batch_bad_committee,
               geocoin::rpc::RPC_INVALID_PARAMETER);
    json batch_bad_proof = batch;
    batch_bad_proof["items"][1]["proof_hex"] = "abcx";
    call_error(port, "createbatchsidechaincheckpointpayload", batch_bad_proof,
               geocoin::rpc::RPC_INVALID_PARAMETER);
    json batch_missing_fee = batch;
    batch_missing_fee["items"][0].erase("fee");
    call_error(port, "createbatchsidechaincheckpointpayload", batch_missing_fee, geocoin::rpc::RPC_INVALID_PARAMETER);
    call_error_params(port, "finalizebatchsidechaincheckpoint", json::array({checkpoint_out["tx_hex"], json::array()}),
                      geocoin::rpc::RPC_INVALID_PARAMETER);

    json slash = sidechain_base(owner, 110);
    slash["amount"] = 500;
    slash["outputs"] = json::array({tx_output(recipient, 400)});
    slash["fraud_ref_a"] = json{{"txid", hex_hash(111)}, {"index", 1}};
    slash["fraud_ref_b"] = json{{"txid", hex_hash(112)}, {"index", 2}};
    slash["committee_id"] = hex_hash(113);
    slash["checkpoint"] = hex_hash(114);
    json slash_out = call_ok(port, "createsidechainslashpayload", slash);
    expect_true(slash_out["amount"] == 500, "slash payload returns amount");
    expect_true(slash_out["fraud_checkpoint_index_b"] == 2, "slash payload returns fraud index");
    json finalized_slash = call_ok_params(port, "finalizesidechainslash",
                                          json::array({slash_out["tx_hex"], json::array({"abcd", "0102"})}));
    expect_true(finalized_slash["tx_hex"].get<std::string>().size() >=
                    slash_out["tx_hex"].get<std::string>().size(),
                "finalize slash returns tx hex");

    json slash_legacy = sidechain_base(owner, 116);
    slash_legacy["amount"] = 300;
    slash_legacy["fraud_checkpoint_a"] = hex_hash(117);
    slash_legacy["fraud_checkpoint_b"] = hex_hash(118);
    slash_legacy["fraud_checkpoint_index_a"] = 3;
    slash_legacy["fraud_checkpoint_index_b"] = 4;
    slash_legacy["committee_id"] = hex_hash(119);
    json slash_legacy_out = call_ok(port, "createsidechainslashpayload", slash_legacy);
    expect_true(slash_legacy_out["fraud_checkpoint_index_a"] == 3,
                "legacy slash payload accepts explicit indexes");

    json slash_missing = sidechain_base(owner, 110);
    call_error(port, "createsidechainslashpayload", slash_missing, geocoin::rpc::RPC_INVALID_PARAMETER);
    json slash_missing_pair = slash;
    slash_missing_pair.erase("fraud_ref_b");
    call_error(port, "createsidechainslashpayload", slash_missing_pair, geocoin::rpc::RPC_INVALID_PARAMETER);
    json slash_missing_txid = slash;
    slash_missing_txid["fraud_ref_a"] = json{{"index", 1}};
    call_error(port, "createsidechainslashpayload", slash_missing_txid, geocoin::rpc::RPC_INVALID_PARAMETER);
    json slash_bad_ref = slash;
    slash_bad_ref["fraud_ref_a"] = json{{"txid", "bad"}, {"index", 1}};
    call_error(port, "createsidechainslashpayload", slash_bad_ref, geocoin::rpc::RPC_INVALID_PARAMETER);
    json slash_bad_ref_index = slash;
    slash_bad_ref_index["fraud_ref_a"] = json{{"txid", hex_hash(111)}, {"index", 70000}};
    call_error(port, "createsidechainslashpayload", slash_bad_ref_index, geocoin::rpc::RPC_INVALID_PARAMETER);
    json slash_missing_index = slash;
    slash_missing_index["fraud_ref_a"] = json{{"txid", hex_hash(111)}};
    call_error(port, "createsidechainslashpayload", slash_missing_index, geocoin::rpc::RPC_INVALID_PARAMETER);
    json slash_legacy_mix = slash;
    slash_legacy_mix["fraud_checkpoint_a"] = hex_hash(115);
    call_error(port, "createsidechainslashpayload", slash_legacy_mix, geocoin::rpc::RPC_INVALID_PARAMETER);
    json slash_legacy_bad_a = slash_legacy;
    slash_legacy_bad_a["fraud_checkpoint_a"] = "bad";
    call_error(port, "createsidechainslashpayload", slash_legacy_bad_a, geocoin::rpc::RPC_INVALID_PARAMETER);
    json slash_legacy_bad_b = slash_legacy;
    slash_legacy_bad_b["fraud_checkpoint_b"] = "bad";
    call_error(port, "createsidechainslashpayload", slash_legacy_bad_b, geocoin::rpc::RPC_INVALID_PARAMETER);
    json slash_legacy_bad_index_a = slash_legacy;
    slash_legacy_bad_index_a["fraud_checkpoint_index_a"] = 70000;
    call_error(port, "createsidechainslashpayload", slash_legacy_bad_index_a,
               geocoin::rpc::RPC_INVALID_PARAMETER);
    json slash_legacy_bad_index_b = slash_legacy;
    slash_legacy_bad_index_b["fraud_checkpoint_index_b"] = 70000;
    call_error(port, "createsidechainslashpayload", slash_legacy_bad_index_b,
               geocoin::rpc::RPC_INVALID_PARAMETER);
    json slash_bad_outputs_type = slash;
    slash_bad_outputs_type["outputs"] = 1;
    call_error(port, "createsidechainslashpayload", slash_bad_outputs_type,
               geocoin::rpc::RPC_INVALID_PARAMETER);
    json slash_outputs_too_high = slash;
    slash_outputs_too_high["outputs"] = json::array({tx_output(recipient, 501)});
    call_error(port, "createsidechainslashpayload", slash_outputs_too_high, geocoin::rpc::RPC_INVALID_PARAMETER);
    call_error_params(port, "finalizesidechainslash", json::array({batch_out["tx_hex"], json::array({"abcd"})}),
                      geocoin::rpc::RPC_INVALID_PARAMETER);
}

void test_sidechain_validate_handlers()
{
    auto storage = std::make_unique<geocoin::state::MemoryStorage>();
    auto* storage_ptr = storage.get();
    expect_true(storage_ptr->put_metadata("chainstate_height", geocoin::utils::encode_u64_le(100)), "sidechain snapshot height");
    expect_true(storage_ptr->put_metadata("chainstate_utxo_count", geocoin::bytes(8, 0)), "empty snapshot UTXO count");
    expect_true(storage_ptr->put_metadata("idx_epoch", geocoin::bytes(32, '1')), "sidechain snapshot epoch");
    const geocoin::Address owner = address_from_key(150);
    const std::string owner_str = owner.str();

    auto make_registry = [&](const geocoin::Hash256& id,
                             uint8_t policy_id,
                             uint8_t version,
                             geocoin::Amount total_issued) {
        geocoin::state::SidechainRegistryEntry entry;
        entry.owner = owner;
        entry.issuance_limit = 1000000;
        entry.total_issued = total_issued;
        entry.multiplier = geocoin::consensus::params::MIN_SIDECHAIN_MULTIPLIER;
        entry.fee_bps = 25;
        entry.max_multiplier = geocoin::consensus::params::MIN_SIDECHAIN_MULTIPLIER;
        entry.token_symbol = "VAL";
        entry.committee_m = 1;
        entry.committee_n = 1;
        entry.committee_members = {owner};
        entry.committee_id = geocoin::consensus::compute_sidechain_committee_id(1, 1, entry.committee_members);
        entry.checkpoint_policy_id = policy_id;
        entry.checkpoint_version = version;
        expect_true(storage_ptr->put_metadata(
                        "sc:reg:" + geocoin::rpc::hash256_to_hex_string(id),
                        serialize_sidechain_registry_for_test(entry)
                    ),
                    "test sidechain registry stored");
        return entry;
    };

    auto make_deposit = [&](const geocoin::Hash256& id,
                            geocoin::Amount amount,
                            geocoin::Height unlock_height,
                            geocoin::Height last_checkpoint_height,
                            geocoin::Height last_sidechain_checkpoint_height,
                            const geocoin::Hash256& checkpoint) {
        geocoin::state::SidechainDeposit deposit;
        deposit.owner = owner;
        deposit.amount = amount;
        deposit.unlock_height = unlock_height == 0 ? 1 : unlock_height;
        deposit.checkpoint = checkpoint;
        deposit.last_checkpoint_height = last_checkpoint_height == 0 && last_sidechain_checkpoint_height > 0 ? 1 : last_checkpoint_height;
        deposit.last_sidechain_checkpoint_height = last_sidechain_checkpoint_height;
        expect_true(storage_ptr->put_metadata(
                        "scd:" + geocoin::rpc::hash256_to_hex_string(id),
                        serialize_sidechain_deposit_for_test(deposit)
                    ),
                    "test sidechain deposit stored");
        return deposit;
    };

    const geocoin::Hash256 valid_id = hash_from_seed(151);
    const geocoin::Hash256 registry_only_id = hash_from_seed(152);
    const geocoin::Hash256 unsupported_id = hash_from_seed(153);
    const geocoin::Hash256 frequent_id = hash_from_seed(154);
    const geocoin::Hash256 deposit_only_id = hash_from_seed(180);
    const geocoin::Hash256 locked_deposit_id = hash_from_seed(181);
    const geocoin::Hash256 low_issued_id = hash_from_seed(182);
    const geocoin::Hash256 small_deposit_id = hash_from_seed(183);
    const geocoin::Hash256 zero_issued_id = hash_from_seed(207);
    const auto valid_registry = make_registry(valid_id, 2, 2, 10000);
    const auto registry_only = make_registry(registry_only_id, 2, 2, 10000);
    (void)registry_only;
    const auto unsupported_registry = make_registry(unsupported_id, 9, 2, 10000);
    (void)unsupported_registry;
    const auto frequent_registry = make_registry(frequent_id, 2, 2, 10000);
    (void)frequent_registry;
    const auto low_issued_registry = make_registry(low_issued_id, 2, 2, 50);
    const auto small_deposit_registry = make_registry(small_deposit_id, 2, 2, 10000);
    const auto zero_issued_registry = make_registry(zero_issued_id, 2, 2, 0);
    const auto valid_deposit = make_deposit(valid_id, 100000, 0, 0, 1, hash_from_seed(155));
    make_deposit(unsupported_id, 100000, 0, 0, 1, hash_from_seed(156));
    make_deposit(frequent_id, 100000, 0, 100, 1, hash_from_seed(157));
    const auto spend_deposit = make_deposit(deposit_only_id, 5000, 0, 0, 0, hash_from_seed(184));
    const auto locked_deposit = make_deposit(locked_deposit_id, 5000, 200, 0, 0, hash_from_seed(185));
    make_deposit(low_issued_id, 100000, 0, 0, 0, hash_from_seed(186));
    make_deposit(small_deposit_id, 100, 0, 0, 0, hash_from_seed(187));
    make_deposit(zero_issued_id, 100000, 0, 0, 0, hash_from_seed(208));

    geocoin::core::Transaction fraud_ref_a;
    fraud_ref_a.type = geocoin::core::TransactionType::SIDECHAIN_CHECKPOINT;
    fraud_ref_a.sidechain_id = valid_id;
    fraud_ref_a.sidechain_checkpoint = hash_from_seed(210);
    fraud_ref_a.sidechain_checkpoint_height = 33;
    const geocoin::Hash256 fraud_ref_a_txid = fraud_ref_a.get_hash();
    expect_true(storage_ptr->put_transaction(fraud_ref_a_txid, fraud_ref_a),
                "test fraud checkpoint A stored");

    geocoin::core::Transaction fraud_ref_b = fraud_ref_a;
    fraud_ref_b.sidechain_checkpoint = hash_from_seed(211);
    const geocoin::Hash256 fraud_ref_b_txid = fraud_ref_b.get_hash();
    expect_true(storage_ptr->put_transaction(fraud_ref_b_txid, fraud_ref_b),
                "test fraud checkpoint B stored");

    auto state = std::make_shared<geocoin::state::ChainState>(std::move(storage));
    state->set_height(100);

    geocoin::rpc::RPCServer server;
    expect_true(server.set_context(geocoin::rpc::RPCServerContext{
        .chain_state = state,
    }), "ChainState RPC context installs atomically");
    const uint16_t port = find_free_port();
    expect_true(server.start("127.0.0.1", port, "admin", "admin-pass"),
                "failed to start ChainState-backed RPC server");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    const auto id_hex = [](const geocoin::Hash256& id) {
        return geocoin::rpc::hash256_to_hex_string(id);
    };
    const geocoin::Amount min_fee =
        geocoin::consensus::calculate_sidechain_checkpoint_min_fee(state->get_height() + 1, 0);
    auto base_request = [&](const geocoin::Hash256& id, const std::string& request_owner) {
        return json{
            {"sidechain_id", id_hex(id)},
            {"owner_address", request_owner},
            {"validate", true},
        };
    };
    auto checkpoint_item = [&](const geocoin::Hash256& id,
                               const geocoin::Hash256& checkpoint,
                               geocoin::Height checkpoint_height,
                               geocoin::Amount fee,
                               const geocoin::Hash256& committee_id,
                               uint8_t version,
                               const std::string& proof_hex) {
        json item = {
            {"sidechain_id", id_hex(id)},
            {"checkpoint", id_hex(checkpoint)},
            {"checkpoint_height", checkpoint_height},
            {"fee", fee},
            {"committee_id", id_hex(committee_id)},
            {"version", version},
        };
        if (!proof_hex.empty()) {
            item["proof_hex"] = proof_hex;
        }
        return item;
    };
    auto batch_request = [&](const json& item) {
        return json{{"validate", true}, {"items", json::array({item})}};
    };

    const json valid_item = checkpoint_item(valid_id, hash_from_seed(160), 2, min_fee,
                                            valid_registry.committee_id, 2, "abcd");
    json valid_batch = call_ok(port, "createbatchsidechaincheckpointpayload", batch_request(valid_item));
    expect_true(valid_batch["payloads"].size() == 1, "validated batch checkpoint succeeds");

    call_error(port, "createbatchsidechaincheckpointpayload",
               batch_request(checkpoint_item(hash_from_seed(161), hash_from_seed(162), 2, min_fee,
                                             valid_registry.committee_id, 2, "abcd")),
               geocoin::rpc::RPC_INVALID_PARAMETER);
    call_error(port, "createbatchsidechaincheckpointpayload",
               batch_request(checkpoint_item(registry_only_id, hash_from_seed(163), 2, min_fee,
                                             valid_registry.committee_id, 2, "abcd")),
               geocoin::rpc::RPC_DATABASE_ERROR);
    call_error(port, "createbatchsidechaincheckpointpayload",
               batch_request(checkpoint_item(unsupported_id, hash_from_seed(164), 2, min_fee,
                                             valid_registry.committee_id, 2, "abcd")),
               geocoin::rpc::RPC_DATABASE_ERROR);
    call_error(port, "createbatchsidechaincheckpointpayload",
               batch_request(checkpoint_item(valid_id, hash_from_seed(165), 2, min_fee,
                                             valid_registry.committee_id, 1, "")),
               geocoin::rpc::RPC_INVALID_PARAMETER);
    call_error(port, "createbatchsidechaincheckpointpayload",
               batch_request(checkpoint_item(valid_id, hash_from_seed(166), 2, min_fee,
                                             valid_registry.committee_id, 2, "")),
               geocoin::rpc::RPC_INVALID_PARAMETER);
    call_error(port, "createbatchsidechaincheckpointpayload",
               batch_request(checkpoint_item(
                   valid_id,
                   hash_from_seed(167),
                   2,
                   min_fee,
                   valid_registry.committee_id,
                   2,
                   std::string((geocoin::consensus::params::MAX_BATCH_CHECKPOINT_PROOF + 1) * 2, 'a')
               )),
               geocoin::rpc::RPC_INVALID_PARAMETER);
    call_error(port, "createbatchsidechaincheckpointpayload",
               batch_request(checkpoint_item(valid_id, hash_from_seed(168), 1, min_fee,
                                             valid_registry.committee_id, 2, "abcd")),
               geocoin::rpc::RPC_INVALID_PARAMETER);
    call_error(port, "createbatchsidechaincheckpointpayload",
               batch_request(checkpoint_item(valid_id, hash_from_seed(169), 2, valid_deposit.amount + 1,
                                             valid_registry.committee_id, 2, "abcd")),
               geocoin::rpc::RPC_INVALID_PARAMETER);
    call_error(port, "createbatchsidechaincheckpointpayload",
               batch_request(checkpoint_item(valid_id, valid_deposit.checkpoint, 2, min_fee,
                                             valid_registry.committee_id, 2, "abcd")),
               geocoin::rpc::RPC_INVALID_PARAMETER);
    call_error(port, "createbatchsidechaincheckpointpayload",
               batch_request(checkpoint_item(frequent_id, hash_from_seed(170), 2, min_fee,
                                             valid_registry.committee_id, 2, "abcd")),
               geocoin::rpc::RPC_INVALID_PARAMETER);
    call_error(port, "createbatchsidechaincheckpointpayload",
               batch_request(checkpoint_item(valid_id, hash_from_seed(171), 2, min_fee - 1,
                                             valid_registry.committee_id, 2, "abcd")),
               geocoin::rpc::RPC_INVALID_PARAMETER);
    call_error(port, "createbatchsidechaincheckpointpayload",
               batch_request(checkpoint_item(valid_id, hash_from_seed(172), 2, min_fee,
                                             hash_from_seed(173), 2, "abcd")),
               geocoin::rpc::RPC_INVALID_PARAMETER);

    json single_checkpoint = base_request(valid_id, owner_str);
    single_checkpoint["checkpoint"] = id_hex(hash_from_seed(188));
    single_checkpoint["checkpoint_height"] = 2;
    single_checkpoint["fee"] = min_fee;
    json single_out = call_ok(port, "createsidechaincheckpointpayload", single_checkpoint);
    expect_true(single_out["committee_id"] == id_hex(valid_registry.committee_id),
                "validated single checkpoint fills active committee id");
    json single_missing_deposit = single_checkpoint;
    single_missing_deposit["sidechain_id"] = id_hex(registry_only_id);
    call_error(port, "createsidechaincheckpointpayload", single_missing_deposit,
               geocoin::rpc::RPC_DATABASE_ERROR);
    json single_wrong_owner = single_checkpoint;
    single_wrong_owner["owner_address"] = address_from_key(189).str();
    call_error(port, "createsidechaincheckpointpayload", single_wrong_owner,
               geocoin::rpc::RPC_INVALID_PARAMETER);
    json single_same_checkpoint = single_checkpoint;
    single_same_checkpoint["checkpoint"] = id_hex(valid_deposit.checkpoint);
    call_error(port, "createsidechaincheckpointpayload", single_same_checkpoint,
               geocoin::rpc::RPC_INVALID_PARAMETER);
    json single_fee_too_high = single_checkpoint;
    single_fee_too_high["fee"] = valid_deposit.amount + 1;
    call_error(port, "createsidechaincheckpointpayload", single_fee_too_high,
               geocoin::rpc::RPC_INVALID_PARAMETER);
    json single_fee_too_low = single_checkpoint;
    single_fee_too_low["fee"] = min_fee > 0 ? min_fee - 1 : 0;
    call_error(port, "createsidechaincheckpointpayload", single_fee_too_low,
               geocoin::rpc::RPC_INVALID_PARAMETER);
    json single_missing_registry = base_request(deposit_only_id, owner_str);
    single_missing_registry["checkpoint"] = id_hex(hash_from_seed(190));
    single_missing_registry["checkpoint_height"] = 2;
    single_missing_registry["fee"] = min_fee;
    single_missing_registry["committee_id"] = id_hex(valid_registry.committee_id);
    call_error(port, "createsidechaincheckpointpayload", single_missing_registry,
               geocoin::rpc::RPC_INVALID_PARAMETER);

    json lock_valid = base_request(hash_from_seed(191), owner_str);
    lock_valid["amount"] = 1000;
    lock_valid["unlock_height"] = 10;
    lock_valid["inputs"] = json::array({tx_input(192, 1)});
    lock_valid["outputs"] = json::array({tx_output(address_from_key(193).str(), 900)});
    expect_true(call_ok(port, "createsidechainlockpayload", lock_valid)["amount"] == 1000,
                "validated lock deposit succeeds when sidechain id is unused");
    json lock_duplicate = lock_valid;
    lock_duplicate["sidechain_id"] = id_hex(deposit_only_id);
    call_error(port, "createsidechainlockpayload", lock_duplicate, geocoin::rpc::RPC_INVALID_PARAMETER);

    json spend_valid = base_request(deposit_only_id, owner_str);
    spend_valid["amount"] = 500;
    spend_valid["outputs"] = json::array({tx_output(address_from_key(194).str(), 500)});
    expect_true(call_ok(port, "createsidechainspendpayload", spend_valid)["amount"] == 500,
                "validated spend deposit succeeds after unlock");
    json spend_missing_deposit = spend_valid;
    spend_missing_deposit["sidechain_id"] = id_hex(hash_from_seed(195));
    call_error(port, "createsidechainspendpayload", spend_missing_deposit,
               geocoin::rpc::RPC_INVALID_PARAMETER);
    json spend_wrong_owner = spend_valid;
    spend_wrong_owner["owner_address"] = address_from_key(196).str();
    call_error(port, "createsidechainspendpayload", spend_wrong_owner,
               geocoin::rpc::RPC_INVALID_PARAMETER);
    json spend_locked = spend_valid;
    spend_locked["sidechain_id"] = id_hex(locked_deposit_id);
    call_error(port, "createsidechainspendpayload", spend_locked, geocoin::rpc::RPC_INVALID_PARAMETER);
    json spend_registered = spend_valid;
    spend_registered["sidechain_id"] = id_hex(valid_id);
    call_error(port, "createsidechainspendpayload", spend_registered, geocoin::rpc::RPC_INVALID_PARAMETER);
    json spend_too_large = spend_valid;
    spend_too_large["amount"] = spend_deposit.amount + 1;
    spend_too_large["outputs"] = json::array({tx_output(address_from_key(197).str(), spend_deposit.amount + 1)});
    call_error(port, "createsidechainspendpayload", spend_too_large, geocoin::rpc::RPC_INVALID_PARAMETER);

    json unlock_valid = base_request(deposit_only_id, owner_str);
    unlock_valid["outputs"] = json::array({tx_output(address_from_key(198).str(), spend_deposit.amount)});
    expect_true(call_ok(port, "createsidechainunlockpayload", unlock_valid)["sidechain_id"] == id_hex(deposit_only_id),
                "validated unlock deposit succeeds after unlock");
    json unlock_missing_deposit = unlock_valid;
    unlock_missing_deposit["sidechain_id"] = id_hex(hash_from_seed(199));
    call_error(port, "createsidechainunlockpayload", unlock_missing_deposit,
               geocoin::rpc::RPC_INVALID_PARAMETER);
    json unlock_wrong_owner = unlock_valid;
    unlock_wrong_owner["owner_address"] = address_from_key(200).str();
    call_error(port, "createsidechainunlockpayload", unlock_wrong_owner,
               geocoin::rpc::RPC_INVALID_PARAMETER);
    json unlock_locked = unlock_valid;
    unlock_locked["sidechain_id"] = id_hex(locked_deposit_id);
    unlock_locked["outputs"] = json::array({tx_output(address_from_key(201).str(), locked_deposit.amount)});
    call_error(port, "createsidechainunlockpayload", unlock_locked, geocoin::rpc::RPC_INVALID_PARAMETER);
    json unlock_registered = unlock_valid;
    unlock_registered["sidechain_id"] = id_hex(valid_id);
    unlock_registered["outputs"] = json::array({tx_output(address_from_key(202).str(), valid_deposit.amount)});
    call_error(port, "createsidechainunlockpayload", unlock_registered, geocoin::rpc::RPC_INVALID_PARAMETER);
    json unlock_wrong_amount = unlock_valid;
    unlock_wrong_amount["outputs"] = json::array({tx_output(address_from_key(203).str(), spend_deposit.amount - 1)});
    call_error(port, "createsidechainunlockpayload", unlock_wrong_amount,
               geocoin::rpc::RPC_INVALID_PARAMETER);

    json redeem_valid = base_request(valid_id, owner_str);
    redeem_valid["amount"] = 500;
    redeem_valid["outputs"] = json::array({tx_output(address_from_key(204).str(), 500)});
    json redeem_out = call_ok(port, "createsidechainredeempayload", redeem_valid);
    expect_true(redeem_out["committee_id"] == id_hex(valid_registry.committee_id),
                "validated redeem fills active committee id");
    json redeem_fill = redeem_valid;
    redeem_fill["validate"] = false;
    redeem_fill.erase("committee_id");
    expect_true(call_ok(port, "createsidechainredeempayload", redeem_fill)["committee_id"] == id_hex(valid_registry.committee_id),
                "redeem validate=false fills committee id when ChainState is available");
    json redeem_missing_registry = redeem_valid;
    redeem_missing_registry["sidechain_id"] = id_hex(deposit_only_id);
    call_error(port, "createsidechainredeempayload", redeem_missing_registry,
               geocoin::rpc::RPC_INVALID_PARAMETER);
    json redeem_low_issued = redeem_valid;
    redeem_low_issued["sidechain_id"] = id_hex(low_issued_id);
    redeem_low_issued["amount"] = low_issued_registry.total_issued + 1;
    redeem_low_issued["outputs"] = json::array({tx_output(address_from_key(205).str(), low_issued_registry.total_issued + 1)});
    call_error(port, "createsidechainredeempayload", redeem_low_issued,
               geocoin::rpc::RPC_INVALID_PARAMETER);
    json redeem_missing_deposit = redeem_valid;
    redeem_missing_deposit["sidechain_id"] = id_hex(registry_only_id);
    call_error(port, "createsidechainredeempayload", redeem_missing_deposit,
               geocoin::rpc::RPC_DATABASE_ERROR);
    json redeem_insufficient_deposit = redeem_valid;
    redeem_insufficient_deposit["sidechain_id"] = id_hex(small_deposit_id);
    redeem_insufficient_deposit["amount"] = small_deposit_registry.total_issued / 10;
    redeem_insufficient_deposit["outputs"] = json::array({tx_output(address_from_key(206).str(), small_deposit_registry.total_issued / 10)});
    call_error(port, "createsidechainredeempayload", redeem_insufficient_deposit,
               geocoin::rpc::RPC_INVALID_PARAMETER);

    json mint_valid = base_request(valid_id, owner_str);
    mint_valid["amount"] = 500;
    json mint_valid_out = call_ok(port, "createsidechainmintpayload", mint_valid);
    expect_true(mint_valid_out["committee_id"] == id_hex(valid_registry.committee_id),
                "validated mint fills active committee id");
    json mint_fill = mint_valid;
    mint_fill["validate"] = false;
    expect_true(call_ok(port, "createsidechainmintpayload", mint_fill)["committee_id"] == id_hex(valid_registry.committee_id),
                "mint validate=false fills committee id when ChainState is available");
    json mint_missing_registry = base_request(deposit_only_id, owner_str);
    mint_missing_registry["amount"] = 500;
    call_error(port, "createsidechainmintpayload", mint_missing_registry,
               geocoin::rpc::RPC_INVALID_PARAMETER);
    json mint_over_issuance = base_request(low_issued_id, owner_str);
    mint_over_issuance["amount"] = 1000001;
    call_error(port, "createsidechainmintpayload", mint_over_issuance,
               geocoin::rpc::RPC_INVALID_PARAMETER);
    json mint_missing_deposit = base_request(registry_only_id, owner_str);
    mint_missing_deposit["amount"] = 500;
    call_error(port, "createsidechainmintpayload", mint_missing_deposit,
               geocoin::rpc::RPC_DATABASE_ERROR);
    json mint_fee_insufficient = base_request(small_deposit_id, owner_str);
    mint_fee_insufficient["amount"] = 100000;
    call_error(port, "createsidechainmintpayload", mint_fee_insufficient,
               geocoin::rpc::RPC_INVALID_PARAMETER);

    json decommission_valid = base_request(zero_issued_id, owner_str);
    json decommission_valid_out = call_ok(port, "createsidechaindecommissionpayload", decommission_valid);
    expect_true(decommission_valid_out["sidechain_id"] == id_hex(zero_issued_id),
                "validated decommission succeeds for zero issued sidechain");
    expect_true(decommission_valid_out["committee_id"] == id_hex(zero_issued_registry.committee_id),
                "validated decommission fills active committee id");
    json decommission_missing_registry = base_request(deposit_only_id, owner_str);
    call_error(port, "createsidechaindecommissionpayload", decommission_missing_registry,
               geocoin::rpc::RPC_INVALID_PARAMETER);
    json decommission_missing_deposit = base_request(registry_only_id, owner_str);
    call_error(port, "createsidechaindecommissionpayload", decommission_missing_deposit,
               geocoin::rpc::RPC_DATABASE_ERROR);
    json decommission_wrong_owner = base_request(zero_issued_id, address_from_key(209).str());
    call_error(port, "createsidechaindecommissionpayload", decommission_wrong_owner,
               geocoin::rpc::RPC_INVALID_PARAMETER);
    json decommission_nonzero_issued = base_request(valid_id, owner_str);
    call_error(port, "createsidechaindecommissionpayload", decommission_nonzero_issued,
               geocoin::rpc::RPC_INVALID_PARAMETER);

    auto slash_request = [&](const geocoin::Hash256& id, geocoin::Amount amount, bool validate) {
        return json{
            {"sidechain_id", id_hex(id)},
            {"owner_address", owner_str},
            {"amount", amount},
            {"validate", validate},
        };
    };

    json slash_fill_committee = call_ok(port, "createsidechainslashpayload", slash_request(valid_id, 100, false));
    expect_true(slash_fill_committee["committee_id"] == id_hex(valid_registry.committee_id),
                "slash validate=false fills active committee id");
    json slash_valid = slash_request(valid_id, 100, true);
    slash_valid["outputs"] = json::array({tx_output(address_from_key(212).str(), 80)});
    slash_valid["fraud_ref_a"] = json{{"txid", id_hex(fraud_ref_a_txid)}};
    slash_valid["fraud_ref_b"] = json{{"txid", id_hex(fraud_ref_b_txid)}};
    json slash_valid_out = call_ok(port, "createsidechainslashpayload", slash_valid);
    expect_true(slash_valid_out["committee_id"] == id_hex(valid_registry.committee_id),
                "validated slash fills active committee id");
    expect_true(slash_valid_out["fraud_checkpoint_index_a"] == 0xFFFF,
                "validated slash resolves legacy checkpoint A sentinel index");
    expect_true(slash_valid_out["fraud_checkpoint_index_b"] == 0xFFFF,
                "validated slash resolves legacy checkpoint B sentinel index");
    call_error(port, "createsidechainslashpayload", slash_request(hash_from_seed(174), 100, true),
               geocoin::rpc::RPC_INVALID_PARAMETER);
    call_error(port, "createsidechainslashpayload", slash_request(registry_only_id, 100, true),
               geocoin::rpc::RPC_DATABASE_ERROR);
    call_error(port, "createsidechainslashpayload", slash_request(valid_id, valid_deposit.amount + 1, true),
               geocoin::rpc::RPC_INVALID_PARAMETER);
    call_error(port, "createsidechainslashpayload", slash_request(valid_id, 100, true),
               geocoin::rpc::RPC_INVALID_PARAMETER);

    server.stop();
}

void test_sidechain_asset_handlers(uint16_t port)
{
    const auto owner = address_from_key(120).str();
    const auto recipient = address_from_key(121).str();
    const auto member1 = address_from_key(122).str();
    const auto member2 = address_from_key(123).str();
    const std::string owner_sig_hex(20, 'a');

    json mint = sidechain_base(owner, 124);
    mint["amount"] = 1000;
    mint["checkpoint"] = hex_hash(125);
    mint["committee_id"] = hex_hash(126);
    mint["fraud_checkpoint_a"] = hex_hash(127);
    mint["fraud_checkpoint_b"] = hex_hash(128);
    mint["fraud_checkpoint_index_a"] = 1;
    mint["fraud_checkpoint_index_b"] = 2;
    json mint_out = call_ok(port, "createsidechainmintpayload", mint);
    expect_true(mint_out["amount"] == 1000, "mint payload returns amount");
    expect_true(mint_out["committee_id"] == hex_hash(126), "mint payload returns committee id");
    json finalized_mint = call_ok_params(port, "finalizesidechainmint",
                                         json::array({mint_out["tx_hex"], json::array({"abcd", "0102"})}));
    expect_true(finalized_mint["tx_hex"].get<std::string>().size() >=
                    mint_out["tx_hex"].get<std::string>().size(),
                "finalize mint returns tx hex");
    call_error_params(port, "finalizesidechainmintandsubmit",
                      json::array({mint_out["tx_hex"], json::array({"abcd"})}),
                      geocoin::rpc::RPC_INTERNAL_ERROR);
    call_error_params(port, "finalizesidechainmint", json::array({"bad", json::array({"abcd"})}),
                      geocoin::rpc::RPC_INVALID_PARAMETER);

    json mint_missing_amount = sidechain_base(owner, 124);
    call_error(port, "createsidechainmintpayload", mint_missing_amount, geocoin::rpc::RPC_INVALID_PARAMETER);
    json mint_zero = mint;
    mint_zero["amount"] = 0;
    call_error(port, "createsidechainmintpayload", mint_zero, geocoin::rpc::RPC_INVALID_PARAMETER);
    json mint_validate = mint;
    mint_validate["validate"] = true;
    call_error(port, "createsidechainmintpayload", mint_validate, geocoin::rpc::RPC_INTERNAL_ERROR);

    json redeem = sidechain_base(owner, 130);
    redeem["amount"] = 500;
    redeem["checkpoint"] = hex_hash(131);
    redeem["committee_id"] = hex_hash(132);
    redeem["outputs"] = json::array({tx_output(recipient, 500)});
    json redeem_out = call_ok(port, "createsidechainredeempayload", redeem);
    expect_true(redeem_out["amount"] == 500, "redeem payload returns amount");
    json finalized_redeem = call_ok_params(port, "finalizesidechainredeem",
                                           json::array({redeem_out["tx_hex"], "abcd"}));
    expect_true(finalized_redeem["tx_hex"].get<std::string>().size() >=
                    redeem_out["tx_hex"].get<std::string>().size(),
                "finalize redeem returns tx hex");
    call_error_params(port, "finalizesidechainredeemandsubmit", json::array({redeem_out["tx_hex"], "abcd"}),
                      geocoin::rpc::RPC_INTERNAL_ERROR);
    call_error_params(port, "finalizesidechainredeem", json::array({mint_out["tx_hex"], "abcd"}),
                      geocoin::rpc::RPC_INVALID_PARAMETER);

    json redeem_missing_outputs = sidechain_base(owner, 130);
    redeem_missing_outputs["amount"] = 500;
    call_error(port, "createsidechainredeempayload", redeem_missing_outputs, geocoin::rpc::RPC_INVALID_PARAMETER);
    json redeem_mismatch = redeem;
    redeem_mismatch["outputs"] = json::array({tx_output(recipient, 499)});
    call_error(port, "createsidechainredeempayload", redeem_mismatch, geocoin::rpc::RPC_INVALID_PARAMETER);
    json redeem_empty_outputs = redeem;
    redeem_empty_outputs["outputs"] = json::array();
    call_error(port, "createsidechainredeempayload", redeem_empty_outputs, geocoin::rpc::RPC_INVALID_PARAMETER);

    json decommission = sidechain_base(owner, 133);
    json decommission_out = call_ok(port, "createsidechaindecommissionpayload", decommission);
    expect_true(decommission_out["sidechain_id"] == hex_hash(133), "decommission payload returns sidechain id");
    json finalized_decommission = call_ok_params(port, "finalizesidechaindecommission",
                                                 json::array({decommission_out["tx_hex"], owner_sig_hex, json::array({"abcd"})}));
    expect_true(finalized_decommission["tx_hex"].get<std::string>().size() >=
                    decommission_out["tx_hex"].get<std::string>().size(),
                "finalize decommission returns tx hex");
    call_error_params(port, "finalizesidechaindecommissionandsubmit",
                      json::array({decommission_out["tx_hex"], owner_sig_hex, json::array({"abcd"})}),
                      geocoin::rpc::RPC_VERIFY_ERROR);
    call_error_params(port, "finalizesidechaindecommission",
                      json::array({mint_out["tx_hex"], owner_sig_hex, json::array({"abcd"})}),
                      geocoin::rpc::RPC_INVALID_PARAMETER);

    json committee = sidechain_base(owner, 140);
    committee["committee_m"] = 2;
    committee["committee_members"] = json::array({member1, member2});
    json committee_out = call_ok(port, "createsidechaincommitteeupdatepayload", committee);
    expect_true(committee_out["committee_m"] == 2, "committee payload returns m");
    expect_true(committee_out["committee_n"] == 2, "committee payload returns n");
    json finalized_committee = call_ok_params(port, "finalizesidechaincommitteeupdate",
                                              json::array({committee_out["tx_hex"], json::array({"abcd"})}));
    expect_true(finalized_committee["tx_hex"].get<std::string>().size() >=
                    committee_out["tx_hex"].get<std::string>().size(),
                "finalize committee update returns tx hex");
    call_error_params(port, "finalizesidechaincommitteeupdate", json::array({mint_out["tx_hex"], json::array({"abcd"})}),
                      geocoin::rpc::RPC_INVALID_PARAMETER);

    json committee_missing = sidechain_base(owner, 140);
    call_error(port, "createsidechaincommitteeupdatepayload", committee_missing, geocoin::rpc::RPC_INVALID_PARAMETER);
    json committee_bad_m = committee;
    committee_bad_m["committee_m"] = 3;
    call_error(port, "createsidechaincommitteeupdatepayload", committee_bad_m, geocoin::rpc::RPC_INVALID_PARAMETER);
    json committee_bad_member_type = committee;
    committee_bad_member_type["committee_members"] = json::array({1});
    call_error(port, "createsidechaincommitteeupdatepayload", committee_bad_member_type,
               geocoin::rpc::RPC_INVALID_PARAMETER);
    json committee_bad_member = committee;
    committee_bad_member["committee_members"] = json::array({"bad-address"});
    call_error(port, "createsidechaincommitteeupdatepayload", committee_bad_member,
               geocoin::rpc::RPC_INVALID_PARAMETER);
    json committee_validate = committee;
    committee_validate["validate"] = true;
    call_error(port, "createsidechaincommitteeupdatepayload", committee_validate,
               geocoin::rpc::RPC_INTERNAL_ERROR);
}

void test_multisig_delegation_handlers(uint16_t port)
{
    const std::vector<geocoin::Address> primary_participants = {
        address_from_key(50),
        address_from_key(51),
        address_from_key(52),
    };
    const std::vector<geocoin::Address> delegated_participants = {
        address_from_key(53),
        address_from_key(54),
    };
    const auto primary = geocoin::wallet::create_multisig_address(2, 3, primary_participants);
    const auto delegated = geocoin::wallet::create_multisig_address(1, 2, delegated_participants);

    json primary_json = json::array();
    for (const auto& address : primary_participants) {
        primary_json.push_back(address.str());
    }
    json delegated_json = json::array();
    for (const auto& address : delegated_participants) {
        delegated_json.push_back(address.str());
    }

    geocoin::core::Transaction proof_draft;
    proof_draft.version = 2;
    proof_draft.type = geocoin::core::TransactionType::DELEGATION_PROOF;
    proof_draft.inputs.push_back({hash_from_seed(59), 0, {}, 0xffffffff, {}, {}});
    proof_draft.primary_multisig_address = geocoin::Address(primary.address);
    proof_draft.delegated_multisig_address = geocoin::Address(delegated.address);
    proof_draft.primary_m = 2;
    proof_draft.primary_n = 3;
    proof_draft.primary_participants = primary_participants;
    proof_draft.delegated_m = 1;
    proof_draft.delegated_n = 2;
    proof_draft.delegated_participants = delegated_participants;
    proof_draft.max_total_delegated = 100000;
    proof_draft.max_per_transaction = 50000;
    proof_draft.max_percent_of_balance = 50;

    const std::string proof_draft_hex =
        geocoin::rpc::bytes_to_hex_string(proof_draft.serialize());
    json proof = call_ok(port, "createdelegationproof", json{
        {"tx_hex", proof_draft_hex},
        {"commitments", json::array({hex_hash(60), hex_hash(61)})},
    });
    expect_true(proof["tx_hex"].get<std::string>().size() > 10, "delegation proof returns tx hex");
    expect_true(proof["delegation_hash"].get<std::string>().size() == 64, "delegation proof returns hash");

    call_error(port, "createdelegationproof", json::object(), geocoin::rpc::RPC_INVALID_PARAMETER);
    call_error(port, "createdelegationproof", json{
        {"tx_hex", proof_draft_hex},
        {"commitments", json::array({"bad"})},
    }, geocoin::rpc::RPC_INVALID_PARAMETER);
    call_error(port, "createdelegationproof", json{
        {"tx_hex", "bad"},
        {"commitments", json::array({hex_hash(60)})},
    }, geocoin::rpc::RPC_INVALID_PARAMETER);

    call_error(port, "createrevokedelegation", json{
        {"tx_hex", "00"},
        {"commitments", json::array({hex_hash(71), hex_hash(72)})},
    }, geocoin::rpc::RPC_INVALID_PARAMETER);

    call_error(port, "createrevokedelegation", json::object(), geocoin::rpc::RPC_INVALID_PARAMETER);
    call_error(port, "createrevokedelegation", json{
        {"tx_hex", "bad"},
        {"commitments", json::array({hex_hash(71)})},
    }, geocoin::rpc::RPC_INVALID_PARAMETER);
    call_error(port, "createrevokedelegation", json{
        {"tx_hex", "bad"},
        {"commitments", json::array({"bad"})},
    }, geocoin::rpc::RPC_INVALID_PARAMETER);
}

void test_multisig_delegation_cli_flow()
{
    const std::vector<geocoin::Address> primary_participants = {
        address_from_key(50),
        address_from_key(51),
        address_from_key(52),
    };
    const std::vector<geocoin::Address> delegated_participants = {
        address_from_key(53),
        address_from_key(54),
    };
    const auto primary = geocoin::wallet::create_multisig_address(2, 3, primary_participants);
    const auto delegated = geocoin::wallet::create_multisig_address(1, 2, delegated_participants);

    json primary_json = json::array();
    for (const auto& address : primary_participants) {
        primary_json.push_back(address.str());
    }
    json delegated_json = json::array();
    for (const auto& address : delegated_participants) {
        delegated_json.push_back(address.str());
    }

    auto chain_state = std::make_shared<geocoin::state::ChainState>(
        std::make_unique<geocoin::state::MemoryStorage>()
    );
    chain_state->set_activation_manifest(geocoin::utils::ActivationManifest::fully_enabled_for_tests("degree"));
    auto mempool = std::make_shared<geocoin::network::Mempool>(chain_state);
    auto mutation_service =
        std::make_shared<geocoin::test::MempoolMutationService>(*mempool);
    const geocoin::core::Transaction coinbase =
        geocoin::core::Transaction::create_coinbase(1, geocoin::Address(primary.address), 50 * geocoin::COIN);
    const geocoin::Amount primary_reward = coinbase.outputs.front().value;
    expect_true(chain_state->apply_transaction(coinbase, 1, true), "primary reward coinbase applies");
    const geocoin::Height mature_height =
        geocoin::consensus::params::COINBASE_MATURITY + 1;
    chain_state->set_height(mature_height);

    geocoin::rpc::RPCServer server;
    expect_true(server.set_context(geocoin::rpc::RPCServerContext{
        .chain_state = chain_state,
        .mempool = mempool,
        .chain_mutations = mutation_service,
    }), "multisig RPC context installs atomically");
    const uint16_t port = find_free_port();
    expect_true(server.start("127.0.0.1", port, "admin", "admin-pass"),
                "failed to start ChainState-backed multisig RPC server");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    json draft = call_ok(port, "createdelegationproofdraft", json{
        {"primary_multisig_address", primary.address},
        {"delegated_multisig_address", delegated.address},
        {"primary_m", 2},
        {"delegated_m", 1},
        {"primary_participants", primary_json},
        {"delegated_participants", delegated_json},
        {"max_total_delegated", 5 * geocoin::COIN},
        {"max_per_transaction", 2 * geocoin::COIN},
        {"max_percent_of_balance", 0},
        {"valid_from", 0},
        {"valid_until", 0},
        {"nonce", 77},
    });
    expect_true(draft["commitment_count"] == 0, "delegation draft has no commitments");
    expect_true(draft["delegation_hash"].get<std::string>().size() == 64,
                "delegation draft returns signing hash");

    json commitment_a = call_ok(port, "signanglecommitment", json{
        {"multisig_address", primary.address},
        {"delegation_hash", draft["delegation_hash"]},
        {"private_key", key_hex(50)},
        {"expected_signer_address", primary_participants[0].str()},
    });
    json commitment_b = call_ok(port, "signanglecommitment", json{
        {"multisig_address", primary.address},
        {"signing_hash", draft["delegation_hash"]},
        {"private_key", key_hex(51)},
        {"expected_signer_address", primary_participants[1].str()},
    });
    expect_true(commitment_a["signer_address"] == primary_participants[0].str(),
                "commitment signer A is returned");
    expect_true(geocoin::test::decode_raw_tx_or_throw(commitment_a["tx_hex"]).verify(),
                "commitment A raw tx verifies");
    call_error(port, "signanglecommitment", json{
        {"multisig_address", primary.address},
        {"signing_hash", draft["delegation_hash"]},
        {"private_key", key_hex(50)},
        {"expected_signer_address", primary_participants[1].str()},
    }, geocoin::rpc::RPC_INVALID_PARAMETER);

    json finalized_proof = call_ok(port, "finalizedelegationproof", json{
        {"tx_hex", draft["tx_hex"]},
        {"commitments", json::array({commitment_a["commitment_hash"], commitment_b["commitment_hash"]})},
    });
    expect_true(finalized_proof["commitment_count"] == 2, "finalized proof has two commitments");
    geocoin::core::Transaction proof_tx =
        geocoin::test::decode_raw_tx_or_throw(finalized_proof["tx_hex"].get<std::string>());
    expect_true(proof_tx.type == geocoin::core::TransactionType::DELEGATION_PROOF,
                "finalized proof is DELEGATION_PROOF");

    json sent_proof = call_ok(port, "senddelegationpackage", json{
        {"commitments", json::array({commitment_a["tx_hex"], commitment_b["tx_hex"]})},
        {"proof", finalized_proof["tx_hex"]},
    });
    expect_true(sent_proof["proof_txid"] == finalized_proof["txid"],
                "senddelegationpackage publishes finalized proof");
    expect_true(chain_state->apply_transaction(proof_tx, mature_height + 1, false),
                "finalized proof is stored for spend draft");
    chain_state->set_height(mature_height + 1);

    const geocoin::Address recipient = address_from_key(90);
    json spend_draft = call_ok(port, "createhierarchicalspenddraft", json{
        {"delegation_proof_txid", finalized_proof["txid"]},
        {"delegated_m", 1},
        {"delegated_participants", delegated_json},
        {"to_address", recipient.str()},
        {"amount", geocoin::COIN},
        {"fee", 1000},
    });
    expect_true(spend_draft["hierarchical_spend_hash"].get<std::string>().size() == 64,
                "hierarchical draft returns signing hash");
    expect_true(spend_draft["total_input"] == proof_tx.outputs.front().value,
                "hierarchical draft selects the delegation change UTXO");
    expect_true(spend_draft["output_count"] == 2,
                "hierarchical draft adds change output");

    json delegated_commitment = call_ok(port, "signanglecommitment", json{
        {"multisig_address", delegated.address},
        {"hierarchical_spend_hash", spend_draft["hierarchical_spend_hash"]},
        {"private_key", key_hex(53)},
        {"expected_signer_address", delegated_participants[0].str()},
    });
    json finalized_spend = call_ok(port, "finalizehierarchicalspend", json{
        {"tx_hex", spend_draft["tx_hex"]},
        {"commitments", json::array({delegated_commitment["commitment_hash"]})},
    });
    expect_true(finalized_spend["commitment_count"] == 1, "finalized spend has delegated commitment");
    geocoin::core::Transaction spend_tx =
        geocoin::test::decode_raw_tx_or_throw(finalized_spend["tx_hex"].get<std::string>());
    expect_true(spend_tx.type == geocoin::core::TransactionType::MULTISIG_SPEND,
                "finalized spend is MULTISIG_SPEND");
    expect_true(spend_tx.delegation_proof_txid == proof_tx.get_hash(),
                "finalized spend points to delegation proof");

    const auto spend_response = send_rpc(
        port,
        "admin",
        "admin-pass",
        "sendmultisigpackage",
        json::array({json{
            {"commitments", json::array({delegated_commitment["tx_hex"]})},
            {"spend", finalized_spend["tx_hex"]},
        }})
    );
    if (spend_response.body.contains("error")) {
        const auto rejects = mempool->get_reject_stats();
        std::ostringstream details;
        details << spend_response.body["error"].dump()
                << "; draft_fee=" << spend_draft["fee"]
                << "; spend_bytes=" << spend_tx.serialize().size()
                << "; commitment_bytes="
                << geocoin::test::decode_raw_tx_or_throw(
                       delegated_commitment["tx_hex"].get<std::string>()).serialize().size();
        for (const auto& [reason, count] : rejects.policy) {
            details << "; policy[" << reason << "]=" << count;
        }
        for (const auto& [reason, count] : rejects.consensus) {
            details << "; consensus[" << reason << "]=" << count;
        }
        throw std::runtime_error("sendmultisigpackage unexpectedly failed: " + details.str());
    }
    json sent_spend = spend_response.body["result"];
    expect_true(sent_spend["spend_txid"] == finalized_spend["txid"],
                "sendmultisigpackage publishes finalized hierarchical spend");

    server.stop();
}

} // namespace

int main()
{
    try {
#ifdef _WIN32
        _putenv_s("GEOCOIN_RPC_RATE_LIMIT_MAX_REQUESTS_PER_WINDOW", "1000");
#else
        setenv("GEOCOIN_RPC_RATE_LIMIT_MAX_REQUESTS_PER_WINDOW", "1000", 1);
#endif
        geocoin::rpc::RPCServer server;
        const uint16_t port = find_free_port();
        expect_true(server.start("127.0.0.1", port, "admin", "admin-pass"), "failed to start RPC server");
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        test_nft_payload_handlers(port);
        test_raw_transaction_builder_guards(port);
        test_raw_transaction_finality_defaults();
        test_checkpoint_proof_handler(port);
        test_sidechain_issue_handler(port);
        test_sidechain_deposit_handlers(port);
        test_sidechain_checkpoint_and_slash_handlers(port);
        test_sidechain_validate_handlers();
        test_sidechain_asset_handlers(port);
        test_multisig_delegation_handlers(port);
        test_multisig_delegation_cli_flow();

        server.stop();
        std::cout << "test_rpc_payload_handlers: ok\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "test_rpc_payload_handlers failed: " << e.what() << "\n";
        return 1;
    }
}
