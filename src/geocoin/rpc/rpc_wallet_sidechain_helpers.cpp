#include "geocoin/rpc/rpc_amount.hpp"
#include "geocoin/rpc/rpc_wallet_helpers.hpp"
#include "geocoin/rpc/rpc_params.hpp"
#include "geocoin/utils/bytes.hpp"

namespace geocoin::rpc {

namespace {
Hash256 sidechain_committee_id_from_entry(const state::SidechainRegistryEntry& entry) {
    return consensus::compute_sidechain_committee_id(
        entry.committee_m,
        entry.committee_n,
        entry.committee_members
    );
}
}

bool resolve_fraud_checkpoint_ref(
    state::ChainState* chain_state,
    const Hash256& sidechain_id,
    const Hash256& ref_txid,
    bool index_provided,
    uint16_t requested_index,
    ResolvedFraudRef& out_ref,
    std::string& error
) {
    if (!chain_state) {
        error = "ChainState не инициализирован";
        return false;
    }

    const auto ref_tx = chain_state->get_transaction_for_fraud_proof(ref_txid);
    if (!ref_tx) {
        error = "checkpoint tx не найден";
        return false;
    }

    if (ref_tx->type == core::TransactionType::SIDECHAIN_CHECKPOINT) {
        if (index_provided && requested_index != 0xFFFF) {
            error = "для SIDECHAIN_CHECKPOINT индекс должен быть 65535";
            return false;
        }
        if (ref_tx->sidechain_id != sidechain_id) {
            error = "checkpoint tx относится к другому sidechain_id";
            return false;
        }
        out_ref.sidechain_id = ref_tx->sidechain_id;
        out_ref.index = 0xFFFF;
        out_ref.checkpoint = ref_tx->sidechain_checkpoint;
        out_ref.checkpoint_height = ref_tx->sidechain_checkpoint_height;
        return true;
    }

    if (ref_tx->type == core::TransactionType::BATCH_SIDECHAIN_CHECKPOINT) {
        size_t idx = std::numeric_limits<size_t>::max();
        if (index_provided) {
            if (requested_index == 0xFFFF) {
                error = "для BATCH_SIDECHAIN_CHECKPOINT нужно указать индекс элемента";
                return false;
            }
            idx = static_cast<size_t>(requested_index);
            if (idx >= ref_tx->sidechain_batch_checkpoints.size()) {
                error = "индекс batch checkpoint вне диапазона";
                return false;
            }
            if (ref_tx->sidechain_batch_checkpoints[idx].sidechain_id != sidechain_id) {
                error = "указанный индекс batch checkpoint относится к другому sidechain_id";
                return false;
            }
        } else {
            size_t found_idx = std::numeric_limits<size_t>::max();
            for (size_t i = 0; i < ref_tx->sidechain_batch_checkpoints.size(); ++i) {
                if (ref_tx->sidechain_batch_checkpoints[i].sidechain_id == sidechain_id) {
                    if (found_idx != std::numeric_limits<size_t>::max()) {
                        error = "в batch несколько элементов для sidechain_id: укажите fraud_ref.index";
                        return false;
                    }
                    found_idx = i;
                }
            }
            if (found_idx == std::numeric_limits<size_t>::max()) {
                error = "в batch нет элемента для sidechain_id";
                return false;
            }
            idx = found_idx;
        }

        out_ref.index = static_cast<uint16_t>(idx);
        out_ref.sidechain_id = ref_tx->sidechain_batch_checkpoints[idx].sidechain_id;
        out_ref.checkpoint = ref_tx->sidechain_batch_checkpoints[idx].checkpoint;
        out_ref.checkpoint_height = ref_tx->sidechain_batch_checkpoints[idx].checkpoint_height;
        return true;
    }

    error = "fraud ref tx должен быть SIDECHAIN_CHECKPOINT или BATCH_SIDECHAIN_CHECKPOINT";
    return false;
}

bool get_validate_flag(const json& p, const char* context_label) {
    if (!p.contains("validate")) {
        return true;
    }
    if (!p["validate"].is_boolean()) {
        throw RPCException(RPC_INVALID_PARAMETER, std::string(context_label) + ": validate должен быть boolean");
    }
    return p["validate"].get<bool>();
}

Amount calc_sidechain_fee_rpc(Amount amount, uint32_t fee_bps) {
    return calc_sidechain_fee_checked(amount, fee_bps);
}

Amount required_amount_field(const json& p, const std::string& field, const std::string& error_message) {
    return rpc_required_amount_field(p, field, error_message);
}

Amount optional_amount_field(
    const json& p,
    const std::string& field,
    Amount default_value,
    const std::string& error_message
) {
    if (!p.is_object() || !p.contains(field) || p[field].is_null()) {
        return default_value;
    }
    return rpc_required_amount_field(p, field, error_message);
}

SidechainCommitteeFields parse_sidechain_committee_from_rpc(const json& p) {
    if (!p.contains("committee_m")) {
        throw RPCException(RPC_INVALID_PARAMETER, "committee_m обязателен");
    }
    if (!p.contains("committee_members") || !p["committee_members"].is_array()) {
        throw RPCException(RPC_INVALID_PARAMETER, "committee_members обязателен (array адресов)");
    }

    SidechainCommitteeFields out{};
    out.m = required_u32_field(p, "committee_m", "committee_m обязателен");
    for (const auto& it : p["committee_members"]) {
        if (!it.is_string()) {
            throw RPCException(RPC_INVALID_PARAMETER, "committee_members должен быть массивом строк");
        }
        Address member(it.get<std::string>());
        if (!crypto::verify_address(member)) {
            throw RPCException(RPC_INVALID_PARAMETER, "committee_members содержит невалидный адрес");
        }
        out.members.emplace_back(member);
    }
    std::sort(out.members.begin(), out.members.end(), [](const Address& a, const Address& b) {
        return a.str() < b.str();
    });
    out.members.erase(std::unique(out.members.begin(), out.members.end(),
                                  [](const Address& a, const Address& b) { return a.str() == b.str(); }),
                      out.members.end());
    out.n = static_cast<uint32_t>(out.members.size());
    if (out.m == 0 || out.n == 0 || out.m > out.n) {
        throw RPCException(RPC_INVALID_PARAMETER, "committee_m/committee_n недопустимы");
    }
    if (out.n > consensus::params::MAX_SIDECHAIN_COMMITTEE_MEMBERS) {
        throw RPCException(RPC_INVALID_PARAMETER, "committee_members слишком много");
    }
    out.committee_id = consensus::compute_sidechain_committee_id(
        static_cast<uint8_t>(out.m),
        static_cast<uint8_t>(out.n),
        out.members
    );
    return out;
}

void append_transaction_inputs_from_rpc(const json& inputs_json, std::vector<core::TxInput>& inputs) {
    if (!inputs_json.is_array() || inputs.size() > consensus::params::MAX_TX_INPUTS ||
        inputs_json.size() > consensus::params::MAX_TX_INPUTS - inputs.size()) {
        throw RPCException(RPC_INVALID_PARAMETER, "inputs must be an array within the transaction input limit");
    }
    for (const auto& v : inputs_json) {
        if (!v.is_object()) {
            throw RPCException(RPC_INVALID_PARAMETER, "inputs должен быть массивом объектов");
        }
        require_only_fields(v, {"txid", "vout", "sequence", "script_sig_hex"});
        if (!v.contains("txid") || !v["txid"].is_string()) {
            throw RPCException(RPC_INVALID_PARAMETER, "input.txid обязателен");
        }
        Hash256 prev{};
        if (!hex_to_hash256_checked(v["txid"].get<std::string>(), prev)) {
            throw RPCException(RPC_INVALID_PARAMETER, "input.txid должен быть 32-байт hex");
        }
        core::TxInput in;
        in.prev_tx_hash = prev;
        in.vout = required_u32_field(v, "vout", "input.vout обязателен");
        if (v.contains("sequence")) {
            in.sequence = optional_u32_field(v, "sequence", in.sequence, "input.sequence неверный");
        }
        if (v.contains("script_sig_hex")) {
            if (!v["script_sig_hex"].is_string()) {
                throw RPCException(RPC_INVALID_PARAMETER, "script_sig_hex must be a string");
            }
            const auto& script_hex = v["script_sig_hex"].get_ref<const std::string&>();
            if (script_hex.size() > consensus::params::MAX_SCRIPT_SIG_SIZE * 2) {
                throw RPCException(RPC_INVALID_PARAMETER, "input.script_sig_hex exceeds the transaction script limit");
            }
            bytes script;
            if (!hex_to_bytes_checked(script_hex, script)) {
                throw RPCException(RPC_INVALID_PARAMETER, "input.script_sig_hex неверный");
            }
            in.script_sig = std::move(script);
        }
        inputs.push_back(in);
    }
}

Amount append_transaction_outputs_from_rpc(const json& outputs_json, std::vector<core::TxOutput>& outputs) {
    if (!outputs_json.is_array() || outputs.size() > consensus::params::MAX_TX_OUTPUTS ||
        outputs_json.size() > consensus::params::MAX_TX_OUTPUTS - outputs.size()) {
        throw RPCException(RPC_INVALID_PARAMETER, "outputs must be an array within the transaction output limit");
    }
    Amount total_output = 0;
    for (const auto& v : outputs_json) {
        if (!v.is_object()) {
            throw RPCException(RPC_INVALID_PARAMETER, "outputs должен быть массивом объектов");
        }
        if (!v.contains("address") || !v["address"].is_string()) {
            throw RPCException(RPC_INVALID_PARAMETER, "output.address обязателен");
        }
        core::TxOutput out;
        out.address = Address(v["address"].get<std::string>());
        if (!crypto::verify_address(out.address)) {
            throw RPCException(RPC_INVALID_PARAMETER, "output.address is invalid");
        }
        out.value = required_amount_field(v, "value", "output.value обязателен");
        total_output = checked_add_amount_or_throw(
            total_output,
            out.value,
            "Сумма outputs превышает допустимый диапазон"
        );
        outputs.push_back(out);
    }
    return total_output;
}

void apply_optional_sidechain_checkpoint_from_rpc(const json& p, core::Transaction& tx) {
    if (p.contains("checkpoint")) {
        if (!p["checkpoint"].is_string()) {
            throw RPCException(RPC_INVALID_PARAMETER, "checkpoint must be a string");
        }
        Hash256 checkpoint{};
        if (!hex_to_hash256_checked(p["checkpoint"].get<std::string>(), checkpoint)) {
            throw RPCException(RPC_INVALID_PARAMETER, "checkpoint должен быть 32-байт hex");
        }
        tx.sidechain_checkpoint = checkpoint;
    }
}

void apply_optional_sidechain_committee_id_from_rpc(const json& p, core::Transaction& tx) {
    if (p.contains("committee_id")) {
        if (!p["committee_id"].is_string()) {
            throw RPCException(RPC_INVALID_PARAMETER, "committee_id must be a string");
        }
        if (!hex_to_hash256_checked(p["committee_id"].get<std::string>(), tx.sidechain_committee_id)) {
            throw RPCException(RPC_INVALID_PARAMETER, "committee_id должен быть 32-байт hex");
        }
    }
}

void apply_active_sidechain_committee_id_or_throw(
    Hash256& committee_id,
    const state::SidechainRegistryEntry& entry
) {
    const Hash256 expected_id = sidechain_committee_id_from_entry(entry);
    if (committee_id == Hash256{}) {
        committee_id = expected_id;
    } else if (committee_id != expected_id) {
        throw RPCException(RPC_INVALID_PARAMETER, "committee_id не совпадает с активным комитетом");
    }
}

void apply_active_sidechain_committee_id_or_throw(
    core::Transaction& tx,
    const state::SidechainRegistryEntry& entry
) {
    apply_active_sidechain_committee_id_or_throw(tx.sidechain_committee_id, entry);
}

void fill_active_sidechain_committee_id_if_available(
    core::Transaction& tx,
    state::ChainState* chain_state,
    const Hash256& sidechain_id
) {
    if (tx.sidechain_committee_id != Hash256{} || !chain_state) {
        return;
    }
    state::SidechainRegistryEntry entry{};
    if (chain_state->get_sidechain_registry(sidechain_id, entry)) {
        tx.sidechain_committee_id = sidechain_committee_id_from_entry(entry);
    }
}

SidechainCommonFields build_sidechain_tx_common(
    const json& p,
    core::Transaction& tx,
    const char* context_label
) {
    if (!p.contains("sidechain_id") && !p.contains("genesis_hash")) {
        throw RPCException(RPC_INVALID_PARAMETER, std::string(context_label) +
            ": sidechain_id или genesis_hash обязателен (hex 32 bytes)");
    }
    if (!p.contains("owner_address") || !p["owner_address"].is_string()) {
        throw RPCException(RPC_INVALID_PARAMETER, std::string(context_label) + ": owner_address обязателен");
    }

    SidechainCommonFields out{};
    out.owner_address = p["owner_address"].get<std::string>();
    if (!crypto::verify_address(Address(out.owner_address))) {
        throw RPCException(RPC_INVALID_PARAMETER, std::string(context_label) + ": owner_address имеет неверный формат");
    }

    const bool has_sidechain_id = p.contains("sidechain_id");
    out.has_genesis = p.contains("genesis_hash");
    if ((has_sidechain_id && !p["sidechain_id"].is_string()) ||
        (out.has_genesis && !p["genesis_hash"].is_string())) {
        throw RPCException(RPC_INVALID_PARAMETER, "sidechain_id and genesis_hash must be hex strings");
    }
    if (out.has_genesis) {
        if (!hex_to_hash256_checked(p["genesis_hash"].get<std::string>(), out.genesis_hash)) {
            throw RPCException(RPC_INVALID_PARAMETER, std::string(context_label) + ": genesis_hash должен быть 32-байт hex");
        }
        out.sidechain_id = out.genesis_hash;
    }
    if (has_sidechain_id) {
        Hash256 provided{};
        if (!hex_to_hash256_checked(p["sidechain_id"].get<std::string>(), provided)) {
            throw RPCException(RPC_INVALID_PARAMETER, std::string(context_label) + ": sidechain_id должен быть 32-байт hex");
        }
        if (out.has_genesis && provided != out.genesis_hash) {
            throw RPCException(RPC_INVALID_PARAMETER, std::string(context_label) + ": sidechain_id должен совпадать с genesis_hash");
        }
        out.sidechain_id = provided;
    }

    tx.version = 2;
    tx.timestamp = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
    tx.sidechain_id = out.sidechain_id;
    tx.sidechain_owner = Address(out.owner_address);
    tx.sidechain_unlock_height = 0;
    tx.sidechain_checkpoint = Hash256{};

    return out;
}

json finalize_sidechain_tx(const json& params, core::TransactionType expected_type, const char* context_label) {
    require_min_params(params, 2, std::string(context_label) + ": требуется tx_hex и signature_hex");
    bytes tx_data;
    if (!hex_to_bytes_checked(params[0].get<std::string>(), tx_data)) {
        throw RPCException(RPC_INVALID_PARAMETER, std::string(context_label) + ": неверный tx_hex");
    }
    bytes sig;
    if (!hex_to_bytes_checked(params[1].get<std::string>(), sig) || sig.size() < 10 || sig.size() > 4096) {
        throw RPCException(RPC_INVALID_PARAMETER, std::string(context_label) + ": signature_hex имеет неверный размер (ожидается Falcon payload)");
    }
    core::Transaction tx = deserialize_transaction_exact(tx_data, std::string(context_label) + ": tx_hex");
    if (tx.type != expected_type) {
        throw RPCException(RPC_INVALID_PARAMETER, std::string(context_label) + ": tx имеет неверный тип");
    }
    tx.sidechain_signature = sig;

    json out;
    out["tx_hex"] = bytes_to_hex_string(tx.serialize());
    out["txid"] = hash256_to_hex_string(tx.get_hash());
    return out;
}

} // namespace geocoin::rpc
