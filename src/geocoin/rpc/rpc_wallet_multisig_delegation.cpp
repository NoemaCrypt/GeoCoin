#include "geocoin/rpc/rpc_wallet_helpers.hpp"
#include "geocoin/utils/secure_memory.hpp"
#include "geocoin/rpc/rpc_amount.hpp"
#include "geocoin/rpc/rpc_params.hpp"
#include "geocoin/wallet/multisig.hpp"
#include "geocoin/wallet/multisig_service.hpp"

#include <algorithm>
#include <chrono>

namespace geocoin::rpc {

namespace {

std::vector<Address> parse_participants(const json& value, const std::string& field_name) {
    if (!value.is_array()) {
        throw RPCException(RPC_INVALID_PARAMETER, field_name + " обязателен (array)");
    }

    std::vector<Address> participants;
    participants.reserve(value.size());
    for (const auto& item : value) {
        if (!item.is_string()) {
            throw RPCException(RPC_INVALID_PARAMETER, field_name + " должен содержать строки");
        }
        participants.emplace_back(item.get<std::string>());
    }

    if (participants.empty() || participants.size() > 15) {
        throw RPCException(RPC_INVALID_PARAMETER, field_name + " должен содержать от 1 до 15 адресов");
    }
    return participants;
}

void verify_multisig_address_preimage(
    const Address& address,
    uint8_t m,
    const std::vector<Address>& participants,
    const std::string& label
) {
    if (m == 0 || m > participants.size() || participants.size() > 15) {
        throw RPCException(RPC_INVALID_PARAMETER, label + " m/n недопустимы");
    }
    const auto computed = wallet::create_multisig_address(
        m,
        static_cast<uint8_t>(participants.size()),
        participants
    );
    if (computed.address != address.str()) {
        throw RPCException(
            RPC_INVALID_PARAMETER,
            label + " address не соответствует m/n и participants"
        );
    }
}

void append_commitment_hashes_from_rpc(const json& commitments, core::Transaction& tx) {
    if (!commitments.is_array()) {
        throw RPCException(RPC_INVALID_PARAMETER, "commitments обязателен (array hex txid)");
    }

    for (const auto& v : commitments) {
        if (!v.is_string()) {
            throw RPCException(RPC_INVALID_PARAMETER, "commitments должен содержать строки");
        }
        Hash256 h{};
        if (!hex_to_hash256_checked(v.get<std::string>(), h)) {
            throw RPCException(RPC_INVALID_PARAMETER, "commitment hash должен быть 32-байт hex");
        }
        tx.angle_commitment_hashes.push_back(h);
    }
}

Amount select_primary_inputs(
    state::ChainState& chain_state,
    const Address& primary_address,
    Amount total_needed,
    std::vector<core::TxInput>& inputs
);

Amount estimated_commitment_package_fee_or_throw(
    const core::Transaction& root,
    size_t maximum_commitments,
    Amount requested_fee
) {
    Amount fee = 0;
    std::string error;
    if (!wallet::estimate_commitment_package_fee(
            root, maximum_commitments, requested_fee, fee, error)) {
        throw RPCException(RPC_INVALID_PARAMETER, error);
    }
    return fee;
}

core::Transaction build_delegation_proof_from_rpc(
    const json& p,
    state::ChainState& chain_state
) {
    if (!p.contains("primary_multisig_address") || !p["primary_multisig_address"].is_string()) {
        throw RPCException(RPC_INVALID_PARAMETER, "primary_multisig_address обязателен");
    }
    if (!p.contains("delegated_multisig_address") || !p["delegated_multisig_address"].is_string()) {
        throw RPCException(RPC_INVALID_PARAMETER, "delegated_multisig_address обязателен");
    }
    required_amount_field(p, "max_total_delegated", "max_total_delegated обязателен (angles)");

    core::Transaction tx;
    tx.version = 2;
    tx.type = core::TransactionType::DELEGATION_PROOF;
    tx.timestamp = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();

    tx.primary_multisig_address = Address(p["primary_multisig_address"].get<std::string>());
    tx.delegated_multisig_address = Address(p["delegated_multisig_address"].get<std::string>());
    tx.primary_m = required_u8_field(p, "primary_m", "primary_m обязателен");
    tx.delegated_m = required_u8_field(p, "delegated_m", "delegated_m обязателен");
    tx.primary_participants = parse_participants(required_field(p, "primary_participants", "primary_participants обязателен"), "primary_participants");
    tx.delegated_participants = parse_participants(required_field(p, "delegated_participants", "delegated_participants обязателен"), "delegated_participants");
    tx.primary_n = static_cast<uint8_t>(tx.primary_participants.size());
    tx.delegated_n = static_cast<uint8_t>(tx.delegated_participants.size());

    verify_multisig_address_preimage(tx.primary_multisig_address, tx.primary_m, tx.primary_participants, "primary");
    verify_multisig_address_preimage(tx.delegated_multisig_address, tx.delegated_m, tx.delegated_participants, "delegated");

    tx.max_total_delegated = required_amount_field(
        p,
        "max_total_delegated",
        "max_total_delegated обязателен (angles)"
    );
    tx.max_per_transaction = optional_amount_field(
        p,
        "max_per_transaction",
        0,
        "max_per_transaction вне диапазона"
    );
    tx.max_percent_of_balance = optional_u8_field(
        p,
        "max_percent_of_balance",
        0,
        "max_percent_of_balance вне диапазона"
    );
    tx.valid_from = optional_u64_field(p, "valid_from", 0, "valid_from вне диапазона");
    tx.valid_until = optional_u64_field(p, "valid_until", 0, "valid_until вне диапазона");
    if (tx.max_total_delegated <= 0 || tx.max_per_transaction < 0 ||
        (tx.max_per_transaction > 0 &&
         tx.max_per_transaction > tx.max_total_delegated)) {
        throw RPCException(RPC_INVALID_PARAMETER, "лимиты делегирования недопустимы");
    }
    if (tx.max_percent_of_balance > 100) {
        throw RPCException(RPC_INVALID_PARAMETER,
                           "max_percent_of_balance должен быть от 0 до 100");
    }
    if (tx.valid_until != 0 && tx.valid_from > tx.valid_until) {
        throw RPCException(RPC_INVALID_PARAMETER, "valid_from не может быть позже valid_until");
    }
    tx.nonce = optional_u32_field(
        p,
        "nonce",
        static_cast<uint32_t>(tx.timestamp & 0xFFFFFFFFU),
        "nonce вне диапазона"
    );

    const Amount requested_fee = optional_amount_field(p, "fee", 0, "fee вне диапазона");
    const Amount fee = estimated_commitment_package_fee_or_throw(
        tx, tx.primary_n, requested_fee);
    const Amount selected = select_primary_inputs(
        chain_state, tx.primary_multisig_address, fee, tx.inputs);
    Amount change = selected - fee;
    if (change > 0 && change < static_cast<Amount>(consensus::params::DUST_LIMIT)) {
        change = 0;
    }
    if (change > 0) {
        tx.outputs.push_back(core::TxOutput{change, tx.primary_multisig_address, {}});
    }

    return tx;
}

json delegation_proof_result(const core::Transaction& tx) {
    json result;
    result["tx_hex"] = bytes_to_hex_string(tx.serialize());
    result["txid"] = hash256_to_hex_string(tx.get_hash());
    result["delegation_hash"] = hash256_to_hex_string(tx.get_delegation_proof_hash());
    result["primary_m"] = tx.primary_m;
    result["primary_n"] = tx.primary_n;
    result["delegated_m"] = tx.delegated_m;
    result["delegated_n"] = tx.delegated_n;
    result["commitment_count"] = tx.angle_commitment_hashes.size();
    return result;
}

Hash256 read_hash_field_any(
    const json& p,
    const std::vector<std::string>& names,
    const std::string& error_message
) {
    for (const auto& name : names) {
        if (p.contains(name)) {
            if (!p[name].is_string()) {
                throw RPCException(RPC_INVALID_PARAMETER, error_message);
            }
            Hash256 hash{};
            if (!hex_to_hash256_checked(p[name].get<std::string>(), hash)) {
                throw RPCException(RPC_INVALID_PARAMETER, error_message);
            }
            return hash;
        }
    }
    throw RPCException(RPC_INVALID_PARAMETER, error_message);
}

bool hash_is_null(const Hash256& hash) {
    return std::all_of(hash.begin(), hash.end(), [](byte b) { return b == 0; });
}

void require_commitment_quorum(const core::Transaction& tx, uint8_t required, const std::string& label) {
    if (tx.angle_commitment_hashes.size() < required) {
        throw RPCException(
            RPC_INVALID_PARAMETER,
            label + ": недостаточно commitments"
        );
    }
}

std::vector<core::TxOutput> parse_multisig_outputs_from_rpc(const json& p) {
    std::vector<core::TxOutput> outputs;
    if (p.contains("outputs")) {
        if (!p["outputs"].is_array() || p["outputs"].empty()) {
            throw RPCException(RPC_INVALID_PARAMETER, "outputs должен быть непустым массивом");
        }
        append_transaction_outputs_from_rpc(p["outputs"], outputs);
        return outputs;
    }

    if (!p.contains("to_address") || !p["to_address"].is_string()) {
        throw RPCException(RPC_INVALID_PARAMETER, "to_address или outputs обязательны");
    }
    core::TxOutput output;
    output.address = Address(p["to_address"].get<std::string>());
    output.value = required_amount_field(p, "amount", "amount обязателен");
    outputs.push_back(output);
    return outputs;
}

Amount sum_outputs_or_throw(const std::vector<core::TxOutput>& outputs) {
    Amount total = 0;
    for (const auto& output : outputs) {
        total = checked_add_amount_or_throw(
            total,
            output.value,
            "Сумма outputs превышает допустимый диапазон"
        );
    }
    return total;
}

Amount select_primary_inputs(
    state::ChainState& chain_state,
    const Address& primary_address,
    Amount total_needed,
    std::vector<core::TxInput>& inputs
) {
    auto utxos = chain_state.get_utxos_for_address(primary_address);
    std::sort(utxos.begin(), utxos.end(), [](const state::UTXO& a, const state::UTXO& b) {
        if (a.height != b.height) {
            return a.height < b.height;
        }
        if (a.tx_hash != b.tx_hash) {
            return a.tx_hash < b.tx_hash;
        }
        return a.vout < b.vout;
    });

    const Height spend_height = chain_state.get_height() + 1;
    Amount selected = 0;
    for (const auto& utxo : utxos) {
        if (utxo.value <= 0) {
            continue;
        }
        if (utxo.is_coinbase) {
            if (spend_height < utxo.height) {
                continue;
            }
            const Height confirmations = spend_height - utxo.height + 1;
            if (confirmations < consensus::params::COINBASE_MATURITY) {
                continue;
            }
        }

        core::TxInput input;
        input.prev_tx_hash = utxo.tx_hash;
        input.vout = utxo.vout;
        inputs.push_back(input);
        selected = checked_add_amount_or_throw(
            selected,
            utxo.value,
            "Сумма selected UTXO превышает допустимый диапазон"
        );
        if (selected >= total_needed) {
            return selected;
        }
    }

    throw RPCException(RPC_WALLET_INSUFFICIENT_FUNDS, "Недостаточно UTXO на primary multisig address");
}

} // namespace

void RPCServer::register_wallet_multisig_delegation_methods() {

    // createdelegationproofdraft - Создает DELEGATION_PROOF без commitments для подписи
    register_method({
        .name = "createdelegationproofdraft",
        .method = [this](const json& params) -> json {
            const json& p = required_object_param(
                params,
                0,
                "Требуется объект с полями primary/delegated/limits"
            );
            core::Transaction tx = build_delegation_proof_from_rpc(p, require_chain_state());
            if (!tx.verify()) {
                throw RPCException(RPC_VERIFY_ERROR, "Локальная верификация DELEGATION_PROOF draft не прошла");
            }
            return delegation_proof_result(tx);
        },
        .category = "multisig",
        .help = "createdelegationproofdraft {\"primary_multisig_address\":\"GEOM...\",\"delegated_multisig_address\":\"GEOM...\",\"primary_m\":5,\"primary_participants\":[\"GEO...\"],\"delegated_m\":2,\"delegated_participants\":[\"GEO...\"],\"max_total_delegated\":100000000,\"max_per_transaction\":50000000,\"max_percent_of_balance\":25,\"valid_from\":0,\"valid_until\":0,\"nonce\":1}\n\nСоздает DELEGATION_PROOF без commitments и возвращает delegation_hash для подписи.\n",
        .requires_wallet = false,
        .param_names = {"request"}
    });

    // signanglecommitment - Создает ANGLE_COMMITMENT для произвольного multisig signing hash
    register_method({
        .name = "signanglecommitment",
        .method = [this](const json& params) -> json {
            const json& p = required_object_param(
                params,
                0,
                "Требуется объект с multisig_address, signing_hash и private_key или wallet_address"
            );
            const Address multisig_address(
                required_string_field(p, "multisig_address", "multisig_address обязателен").get<std::string>()
            );
            const Hash256 signing_hash = read_hash_field_any(
                p,
                {"signing_hash", "spending_tx_hash", "delegation_hash", "revoke_hash", "hierarchical_spend_hash"},
                "signing_hash должен быть 32-байт hex"
            );

            bytes private_key;
            utils::ScopedContainerCleanser<bytes> privateKeyCleanser(private_key);

            if (p.contains("private_key")) {
                if (!p["private_key"].is_string() ||
                    !hex_to_bytes_checked(p["private_key"].get<std::string>(), private_key) ||
                    private_key.size() != 32) {
                    throw RPCException(RPC_INVALID_PARAMETER, "private_key должен быть 32-байт hex");
                }
            } else if (p.contains("wallet_address")) {
                auto& wallet = require_wallet();
                if (!p["wallet_address"].is_string()) {
                    throw RPCException(RPC_INVALID_PARAMETER, "wallet_address должен быть строкой");
                }
                std::string password = optional_object_password(p);
                utils::ScopedContainerCleanser<std::string> passwordCleanser(password);
                require_wallet_password(
                    wallet,
                    password,
                    "Кошелек защищен паролем: укажите password в объекте запроса"
                );
                if (!wallet.get_private_key(Address(p["wallet_address"].get<std::string>()), private_key)) {
                    throw RPCException(RPC_WALLET_ERROR, "Приватный ключ wallet_address не найден в кошельке");
                }
            } else {
                throw RPCException(RPC_INVALID_PARAMETER, "private_key или wallet_address обязателен");
            }

            Address signer_address;
            if (!crypto::compute_address_from_private_key(private_key, signer_address)) {
                throw RPCException(RPC_INVALID_PARAMETER, "Не удалось вычислить адрес подписанта");
            }
            if (p.contains("expected_signer_address") && !p["expected_signer_address"].is_null()) {
                const Address expected(
                    required_string_field(p, "expected_signer_address", "expected_signer_address должен быть строкой")
                        .get<std::string>()
                );
                if (expected.str() != signer_address.str()) {
                    throw RPCException(RPC_INVALID_PARAMETER, "private_key не соответствует expected_signer_address");
                }
            }

            wallet::CommitmentResult commitment;
            std::string error;
            if (!wallet::sign_angle_commitment(multisig_address, signing_hash, private_key, commitment, error)) {
                throw RPCException(RPC_VERIFY_ERROR, error);
            }

            json result;
            result["tx_hex"] = bytes_to_hex_string(commitment.commitment_tx.serialize());
            result["txid"] = hash256_to_hex_string(commitment.commitment_hash);
            result["commitment_hash"] = hash256_to_hex_string(commitment.commitment_hash);
            result["signer_address"] = signer_address.str();
            result["multisig_address"] = multisig_address.str();
            result["signing_hash"] = hash256_to_hex_string(signing_hash);
            return result;
        },
        .category = "multisig",
        .help = "signanglecommitment {\"multisig_address\":\"GEOM...\",\"signing_hash\":\"hex32\",\"private_key\":\"hex32\"|\"wallet_address\":\"GEO...\",\"password\":\"...\"?,\"expected_signer_address\":\"GEO...\"?}\n\nСоздает ANGLE_COMMITMENT. signing_hash может быть spending_tx_hash, delegation_hash или hierarchical_spend_hash. password обязателен для wallet_address на зашифрованном кошельке.\n",
        .requires_wallet = false,
        .param_names = {"request"},
        .side_effects = "uses a private key supplied by RPC or loaded from the local wallet"
    });

    // finalizedelegationproof - Добавляет commitment txid в DELEGATION_PROOF draft
    register_method({
        .name = "finalizedelegationproof",
        .method = [](const json& params) -> json {
            const json& p = required_object_param(
                params,
                0,
                "Требуется объект с tx_hex и commitments"
            );
            core::Transaction tx = deserialize_transaction_hex_or_throw(
                required_string_field(p, "tx_hex", "tx_hex обязателен").get<std::string>(),
                "Неверная hex-строка DELEGATION_PROOF",
                "tx_hex"
            );
            if (tx.type != core::TransactionType::DELEGATION_PROOF) {
                throw RPCException(RPC_INVALID_PARAMETER, "tx_hex должен содержать DELEGATION_PROOF");
            }
            tx.angle_commitment_hashes.clear();
            append_commitment_hashes_from_rpc(required_field(p, "commitments", "commitments обязателен"), tx);
            require_commitment_quorum(tx, tx.primary_m, "finalizedelegationproof");
            if (!tx.verify()) {
                throw RPCException(RPC_VERIFY_ERROR, "Локальная верификация DELEGATION_PROOF не прошла");
            }
            return delegation_proof_result(tx);
        },
        .category = "multisig",
        .help = "finalizedelegationproof {\"tx_hex\":\"draft_hex\",\"commitments\":[\"commitment_txid\",...]}\n\nВозвращает финальный DELEGATION_PROOF hex.\n",
        .requires_wallet = false,
        .param_names = {"request"}
    });

    // createhierarchicalspenddraft - Создает delegated spend draft по confirmed delegation proof
    register_method({
        .name = "createhierarchicalspenddraft",
        .method = [this](const json& params) -> json {
            auto& chain_state = require_chain_state();
            const json& p = required_object_param(
                params,
                0,
                "Требуется объект с delegation_proof_txid, delegated_m, delegated_participants и outputs"
            );
            Hash256 proof_txid{};
            if (!hex_to_hash256_checked(
                    required_string_field(p, "delegation_proof_txid", "delegation_proof_txid обязателен")
                        .get<std::string>(),
                    proof_txid)) {
                throw RPCException(RPC_INVALID_PARAMETER, "delegation_proof_txid должен быть 32-байт hex");
            }

            const auto proof = chain_state.get_transaction(proof_txid);
            if (!proof || proof->type != core::TransactionType::DELEGATION_PROOF) {
                throw RPCException(RPC_INVALID_PARAMETER, "delegation_proof_txid не найден или не является DELEGATION_PROOF");
            }

            std::vector<Address> delegated_participants = parse_participants(
                required_field(p, "delegated_participants", "delegated_participants обязателен"),
                "delegated_participants"
            );
            const uint8_t delegated_m = required_u8_field(p, "delegated_m", "delegated_m обязателен");
            verify_multisig_address_preimage(
                proof->delegated_multisig_address,
                delegated_m,
                delegated_participants,
                "delegated"
            );
            if (delegated_m != proof->delegated_m ||
                delegated_participants.size() != proof->delegated_participants.size()) {
                throw RPCException(RPC_INVALID_PARAMETER, "delegated config не совпадает с delegation proof");
            }

            std::vector<core::TxOutput> outputs = parse_multisig_outputs_from_rpc(p);
            const Amount requested_output = sum_outputs_or_throw(outputs);
            const Amount fee = std::max<Amount>(
                optional_amount_field(p, "fee", 0, "fee вне диапазона"),
                consensus::params::MIN_TX_FEE
            );
            core::Transaction fee_template = core::Transaction::create_multisig_spend(
                {}, outputs, {}, delegated_m,
                static_cast<uint8_t>(delegated_participants.size())
            );
            fee_template.delegation_proof_txid = proof_txid;
            const Amount package_fee = estimated_commitment_package_fee_or_throw(
                fee_template, delegated_participants.size(), fee);
            const Amount total_needed = checked_add_amount_or_throw(
                requested_output,
                package_fee,
                "Сумма outputs + fee превышает допустимый диапазон"
            );

            std::vector<core::TxInput> inputs;
            Amount selected = 0;
            if (p.contains("inputs") && !p["inputs"].is_null()) {
                if (!p["inputs"].is_array() || p["inputs"].empty()) {
                    throw RPCException(RPC_INVALID_PARAMETER, "inputs должен быть непустым массивом");
                }
                append_transaction_inputs_from_rpc(p["inputs"], inputs);
                for (const auto& input : inputs) {
                    state::UTXO utxo;
                    if (!chain_state.get_utxo(input.prev_tx_hash, input.vout, utxo)) {
                        throw RPCException(RPC_INVALID_PARAMETER, "input UTXO не найден");
                    }
                    if (utxo.address.str() != proof->primary_multisig_address.str()) {
                        throw RPCException(RPC_INVALID_PARAMETER, "input UTXO не принадлежит primary multisig address");
                    }
                    selected = checked_add_amount_or_throw(
                        selected,
                        utxo.value,
                        "Сумма selected UTXO превышает допустимый диапазон"
                    );
                }
                if (selected < total_needed) {
                    throw RPCException(RPC_WALLET_INSUFFICIENT_FUNDS, "Недостаточно selected inputs");
                }
            } else {
                selected = select_primary_inputs(chain_state, proof->primary_multisig_address, total_needed, inputs);
            }

            Amount change = selected - total_needed;
            Amount fee_used = package_fee;
            if (change > 0 && change < static_cast<Amount>(consensus::params::DUST_LIMIT)) {
                fee_used = checked_add_amount_or_throw(
                    fee_used,
                    change,
                    "fee и dust-change превышают допустимый диапазон"
                );
                change = 0;
            }
            if (change > 0) {
                outputs.push_back(core::TxOutput{change, proof->primary_multisig_address, {}});
            }

            core::Transaction tx = core::Transaction::create_multisig_spend(
                inputs,
                outputs,
                {},
                delegated_m,
                static_cast<uint8_t>(delegated_participants.size())
            );
            tx.delegation_proof_txid = proof_txid;

            json result;
            result["tx_hex"] = bytes_to_hex_string(tx.serialize());
            result["txid"] = hash256_to_hex_string(tx.get_hash());
            result["hierarchical_spend_hash"] = hash256_to_hex_string(tx.get_hierarchical_spend_hash());
            result["delegation_proof_txid"] = hash256_to_hex_string(proof_txid);
            result["primary_multisig_address"] = proof->primary_multisig_address.str();
            result["delegated_multisig_address"] = proof->delegated_multisig_address.str();
            result["delegated_m"] = tx.m;
            result["delegated_n"] = tx.n;
            result["input_count"] = tx.inputs.size();
            result["output_count"] = tx.outputs.size();
            result["total_input"] = selected;
            result["total_output"] = sum_outputs_or_throw(tx.outputs);
            result["fee"] = fee_used;
            return result;
        },
        .category = "multisig",
        .help = "createhierarchicalspenddraft {\"delegation_proof_txid\":\"hex32\",\"delegated_m\":2,\"delegated_participants\":[\"GEO...\"],\"to_address\":\"GEO...\",\"amount\":100000000,\"fee\":1000}\n\nСоздает MULTISIG_SPEND draft по delegation proof и возвращает hierarchical_spend_hash для delegated signatures.\n",
        .requires_wallet = false,
        .param_names = {"request"}
    });

    // finalizehierarchicalspend - Добавляет delegated commitment txid в hierarchical spend draft
    register_method({
        .name = "finalizehierarchicalspend",
        .method = [](const json& params) -> json {
            const json& p = required_object_param(
                params,
                0,
                "Требуется объект с tx_hex и commitments"
            );
            core::Transaction tx = deserialize_transaction_hex_or_throw(
                required_string_field(p, "tx_hex", "tx_hex обязателен").get<std::string>(),
                "Неверная hex-строка MULTISIG_SPEND",
                "tx_hex"
            );
            if (tx.type != core::TransactionType::MULTISIG_SPEND || hash_is_null(tx.delegation_proof_txid)) {
                throw RPCException(RPC_INVALID_PARAMETER, "tx_hex должен содержать hierarchical MULTISIG_SPEND");
            }
            tx.angle_commitment_hashes.clear();
            append_commitment_hashes_from_rpc(required_field(p, "commitments", "commitments обязателен"), tx);
            require_commitment_quorum(tx, tx.m, "finalizehierarchicalspend");
            if (!tx.verify()) {
                throw RPCException(RPC_VERIFY_ERROR, "Локальная верификация MULTISIG_SPEND не прошла");
            }

            json result;
            result["tx_hex"] = bytes_to_hex_string(tx.serialize());
            result["txid"] = hash256_to_hex_string(tx.get_hash());
            result["hierarchical_spend_hash"] = hash256_to_hex_string(tx.get_hierarchical_spend_hash());
            result["delegation_proof_txid"] = hash256_to_hex_string(tx.delegation_proof_txid);
            result["commitment_count"] = tx.angle_commitment_hashes.size();
            return result;
        },
        .category = "multisig",
        .help = "finalizehierarchicalspend {\"tx_hex\":\"draft_hex\",\"commitments\":[\"commitment_txid\",...]}\n\nВозвращает финальный hierarchical MULTISIG_SPEND hex.\n",
        .requires_wallet = false,
        .param_names = {"request"}
    });

    // createdelegationproof - Создает сырую DELEGATION_PROOF транзакцию
    register_method({
        .name = "createdelegationproof",
        .method = [](const json& params) -> json {
            const json& p = required_object_param(
                params,
                0,
                "Требуется объект с tx_hex и commitments"
            );
            core::Transaction tx = deserialize_transaction_hex_or_throw(
                required_string_field(p, "tx_hex", "tx_hex draft обязателен").get<std::string>(),
                "Неверная hex-строка DELEGATION_PROOF",
                "tx_hex"
            );
            if (tx.type != core::TransactionType::DELEGATION_PROOF ||
                !tx.angle_commitment_hashes.empty()) {
                throw RPCException(RPC_INVALID_PARAMETER,
                                   "tx_hex должен содержать unsigned DELEGATION_PROOF draft");
            }
            append_commitment_hashes_from_rpc(
                required_field(p, "commitments", "commitments обязателен"), tx);
            require_commitment_quorum(tx, tx.primary_m, "createdelegationproof");
            if (!tx.verify()) {
                throw RPCException(RPC_VERIFY_ERROR, "Локальная верификация DELEGATION_PROOF не прошла");
            }
            return delegation_proof_result(tx);
        },
        .category = "multisig",
        .help = "createdelegationproof {\"tx_hex\":\"draft_hex\",\"commitments\":[\"txid1\",...]}\n\nФинализирует неизменяемый DELEGATION_PROOF draft.\n",
        .requires_wallet = false,
        .param_names = {"request"}
    });

    // Build the complete, fee-paying revoke intent before any participant signs.
    register_method({
        .name = "createrevokedelegationdraft",
        .method = [this](const json& params) -> json {
            auto& chain_state = require_chain_state();
            const json& p = required_object_param(
                params, 0, "Требуется revoked_delegation_proof_txid и optional fee");
            Hash256 revoked{};
            if (!hex_to_hash256_checked(
                    required_string_field(
                        p,
                        "revoked_delegation_proof_txid",
                        "revoked_delegation_proof_txid обязателен").get<std::string>(),
                    revoked)) {
                throw RPCException(RPC_INVALID_PARAMETER,
                                   "revoked_delegation_proof_txid должен быть 32-байт hex");
            }
            const auto proof = chain_state.get_transaction(revoked);
            if (!proof || proof->type != core::TransactionType::DELEGATION_PROOF) {
                throw RPCException(RPC_INVALID_PARAMETER,
                                   "revoked delegation proof не найден в active chain");
            }
            if (chain_state.is_delegation_revoked(revoked)) {
                throw RPCException(RPC_VERIFY_REJECTED, "delegation proof уже отозван");
            }

            core::Transaction tx;
            tx.version = 2;
            tx.type = core::TransactionType::REVOKE_DELEGATION;
            tx.timestamp = optional_u64_field(
                p,
                "timestamp",
                static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count()),
                "timestamp вне диапазона"
            );
            tx.revoked_delegation_proof_txid = revoked;

            const Amount requested_fee = optional_amount_field(p, "fee", 0, "fee вне диапазона");
            const Amount fee = estimated_commitment_package_fee_or_throw(
                tx, proof->primary_n, requested_fee);
            const Amount selected = select_primary_inputs(
                chain_state, proof->primary_multisig_address, fee, tx.inputs);
            Amount change = selected - fee;
            if (change > 0 && change < static_cast<Amount>(consensus::params::DUST_LIMIT)) {
                change = 0;
            }
            if (change > 0) {
                tx.outputs.push_back(core::TxOutput{change, proof->primary_multisig_address, {}});
            }

            json result;
            result["tx_hex"] = bytes_to_hex_string(tx.serialize());
            result["txid"] = hash256_to_hex_string(tx.get_hash());
            result["revoke_hash"] = hash256_to_hex_string(tx.get_revoke_delegation_hash());
            result["primary_multisig_address"] = proof->primary_multisig_address.str();
            result["primary_m"] = proof->primary_m;
            result["primary_n"] = proof->primary_n;
            result["fee"] = selected - (tx.outputs.empty() ? 0 : tx.outputs.front().value);
            return result;
        },
        .category = "multisig",
        .help = "createrevokedelegationdraft {\"revoked_delegation_proof_txid\":\"txid\",\"fee\":500?}\n\nСоздает полный fee-paying draft и revoke_hash для подписания.\n",
        .requires_wallet = false,
        .param_names = {"request"}
    });

    // createrevokedelegation - Финализирует неизменяемый revoke draft.
    register_method({
        .name = "createrevokedelegation",
        .method = [this](const json& params) -> json {
            const json& p = required_object_param(
                params,
                0,
                "Требуется объект с tx_hex и commitments"
            );
            if (!p.contains("commitments") || !p["commitments"].is_array()) {
                throw RPCException(RPC_INVALID_PARAMETER, "commitments обязателен (array hex txid)");
            }
            core::Transaction tx = deserialize_transaction_hex_or_throw(
                required_string_field(p, "tx_hex", "tx_hex draft обязателен").get<std::string>(),
                "Неверная hex-строка REVOKE_DELEGATION",
                "tx_hex"
            );
            if (tx.type != core::TransactionType::REVOKE_DELEGATION ||
                !tx.angle_commitment_hashes.empty()) {
                throw RPCException(RPC_INVALID_PARAMETER,
                                   "tx_hex должен содержать unsigned REVOKE_DELEGATION draft");
            }
            auto& chain_state = require_chain_state();
            const auto proof = chain_state.get_transaction(tx.revoked_delegation_proof_txid);
            if (!proof || proof->type != core::TransactionType::DELEGATION_PROOF) {
                throw RPCException(RPC_INVALID_PARAMETER, "delegation proof не найден");
            }

            for (const auto& v : p["commitments"]) {
                if (!v.is_string()) {
                    throw RPCException(RPC_INVALID_PARAMETER, "commitments должен содержать строки");
                }
                Hash256 h{};
                if (!hex_to_hash256_checked(v.get<std::string>(), h)) {
                    throw RPCException(RPC_INVALID_PARAMETER, "commitment hash должен быть 32-байт hex");
                }
                tx.angle_commitment_hashes.push_back(h);
            }
            require_commitment_quorum(tx, proof->primary_m, "createrevokedelegation");

            if (!tx.verify()) {
                throw RPCException(RPC_VERIFY_ERROR, "Локальная верификация REVOKE_DELEGATION не прошла");
            }

            json result;
            result["tx_hex"] = bytes_to_hex_string(tx.serialize());
            result["txid"] = hash256_to_hex_string(tx.get_hash());
            result["revoke_hash"] = hash256_to_hex_string(tx.get_revoke_delegation_hash());
            return result;
        },
        .category = "multisig",
        .help = "createrevokedelegation {\"tx_hex\":\"draft_hex\",\"commitments\":[\"txid1\",...]}\n\nФинализирует неизменяемый REVOKE_DELEGATION draft.\n",
        .requires_wallet = false,
        .param_names = {"request"}
    });

    // senddelegationpackage - Последовательно отправляет angle commitments и DELEGATION_PROOF
    register_method({
        .name = "senddelegationpackage",
        .method = [this](const json& params) -> json {
            auto& chain_state = require_chain_state();
            const json& payload = required_object_param(
                params,
                0,
                "Требуется объект с полями commitments и proof"
            );
            if (!payload.contains("commitments") || !payload["commitments"].is_array()) {
                throw RPCException(RPC_INVALID_PARAMETER, "commitments должен быть массивом hex-строк");
            }
            if (!payload.contains("proof") || !payload["proof"].is_string()) {
                throw RPCException(RPC_INVALID_PARAMETER, "proof должен быть hex-строкой");
            }

            return submit_commitment_package_from_rpc(
                payload,
                "proof",
                "proof_txid",
                chain_state_,
                false,
                chain_mutations_
            );
        },
        .category = "multisig",
        .help = "senddelegationpackage {\"commitments\":[\"hex1\",\"hex2\",...],\"proof\":\"hex\"}\n\nПоследовательно публикует commitments, затем DELEGATION_PROOF.\n",
        .requires_wallet = false,
        .param_names = {"request"},
        .mutates = true,
        .side_effects = "submits a delegation package to the local mempool"
    });

    // sendrevokepackage - Последовательно отправляет angle commitments и REVOKE_DELEGATION
    register_method({
        .name = "sendrevokepackage",
        .method = [this](const json& params) -> json {
            auto& chain_state = require_chain_state();
            const json& payload = required_object_param(
                params,
                0,
                "Требуется объект с полями commitments и revoke"
            );
            if (!payload.contains("commitments") || !payload["commitments"].is_array()) {
                throw RPCException(RPC_INVALID_PARAMETER, "commitments должен быть массивом hex-строк");
            }
            if (!payload.contains("revoke") || !payload["revoke"].is_string()) {
                throw RPCException(RPC_INVALID_PARAMETER, "revoke должен быть hex-строкой");
            }

            return submit_commitment_package_from_rpc(
                payload,
                "revoke",
                "revoke_txid",
                chain_state_,
                false,
                chain_mutations_
            );
        },
        .category = "multisig",
        .help = "sendrevokepackage {\"commitments\":[\"hex1\",\"hex2\",...],\"revoke\":\"hex\"}\n\nПоследовательно публикует commitments, затем REVOKE_DELEGATION.\n",
        .requires_wallet = false,
        .param_names = {"request"},
        .mutates = true,
        .side_effects = "submits a delegation revoke package to the local mempool"
    });

}

} // namespace geocoin::rpc
