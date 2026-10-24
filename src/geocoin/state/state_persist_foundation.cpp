#include "geocoin/state/state.hpp"
#include "geocoin/state/records.hpp"
#include "geocoin/state/state_internal.hpp"

#include "geocoin/consensus/chainparams.hpp"
#include "geocoin/consensus/consensus.hpp"
#include "geocoin/crypto/addresses.hpp"
#include "geocoin/multisig/multisig.hpp"
#include "geocoin/utils/bytes.hpp"
#include "geocoin/utils/logger.hpp"

#include <limits>
#include <string>

namespace geocoin::state {
using namespace records;

namespace {
constexpr const char* kInterestPoolBalanceKeyLocal = "interest_pool_balance";
}

bool records::validate_foundation_config(const FoundationConfig& config) {
    if (config.m == 0 || config.m > config.n || config.n > 15 ||
        config.participants.size() != config.n) {
        return false;
    }
    // Address derivation alone accepts arbitrary strings and repeated members.
    // Use the same complete multisig policy check as other consensus records.
    return multisig::validate_multisig_config(config.address,
        multisig::MultisigConfig(config.m, config.n, config.participants));
}

bytes records::serialize_foundation_config(const FoundationConfig& config) {
    bytes data;
    const std::string addr_str = config.address.str();
    uint32_t addr_len = static_cast<uint32_t>(addr_str.size());
    for (int i = 0; i < 4; i++) {
        data.push_back((addr_len >> (i * 8)) & 0xFF);
    }
    data.insert(data.end(), addr_str.begin(), addr_str.end());

    data.push_back(config.m);
    data.push_back(config.n);

    uint32_t count = static_cast<uint32_t>(config.participants.size());
    for (int i = 0; i < 4; i++) {
        data.push_back((count >> (i * 8)) & 0xFF);
    }
    for (const auto& p : config.participants) {
        const std::string s = p.str();
        uint32_t len = static_cast<uint32_t>(s.size());
        for (int i = 0; i < 4; i++) {
            data.push_back((len >> (i * 8)) & 0xFF);
        }
        data.insert(data.end(), s.begin(), s.end());
    }
    return data;
}

bool records::deserialize_foundation_config(const bytes& data, FoundationConfig& out) {
    size_t offset = 0;
    if (offset + 4 > data.size()) {
        return false;
    }
    uint32_t addr_len = 0;
    for (int i = 0; i < 4; i++) {
        addr_len |= (uint32_t)data[offset++] << (i * 8);
    }
    if (addr_len > consensus::params::MAX_ADDRESS_SIZE ||
        addr_len > data.size() - offset) {
        return false;
    }
    out.address = Address(std::string(data.begin() + offset, data.begin() + offset + addr_len));
    offset += addr_len;

    if (offset + 2 > data.size()) {
        return false;
    }
    out.m = data[offset++];
    out.n = data[offset++];

    if (offset + 4 > data.size()) {
        return false;
    }
    uint32_t count = 0;
    for (int i = 0; i < 4; i++) {
        count |= (uint32_t)data[offset++] << (i * 8);
    }
    if (count > 15) {
        return false;
    }
    out.participants.clear();
    for (uint32_t i = 0; i < count; i++) {
        if (offset + 4 > data.size()) {
            return false;
        }
        uint32_t len = 0;
        for (int j = 0; j < 4; j++) {
            len |= (uint32_t)data[offset++] << (j * 8);
        }
        if (len > consensus::params::MAX_ADDRESS_SIZE || len > data.size() - offset) {
            return false;
        }
        out.participants.emplace_back(std::string(data.begin() + offset, data.begin() + offset + len));
        offset += len;
    }
    return offset == data.size() && validate_foundation_config(out);
}

void ChainState::persist_foundation_config_to_storage() {
    if (!storage_) {
        return;
    }
    record_persistence_result_(
        storage_->put_metadata("foundation_cfg", serialize_foundation_config(foundation_config_)),
        "persist foundation configuration"
    );
}

void ChainState::reset_foundation_config_to_defaults_() {
    FoundationConfig defaults;
    const auto& chain_params = consensus::Params();
    defaults.address = chain_params.foundation_address();
    defaults.m = chain_params.foundation_m();
    defaults.n = chain_params.foundation_n();
    defaults.participants.clear();
    defaults.participants = chain_params.foundation_participants();

    if (!validate_foundation_config(defaults)) {
        GEO_LOG_ERROR(
            "chainstate",
            "Foundation defaults contain an invalid multisig configuration"
        );
    }
    foundation_config_ = std::move(defaults);
}

void ChainState::load_foundation_config_from_storage() {
    reset_foundation_config_to_defaults_();
    if (!storage_) {
        return;
    }

    bytes data;
    if (storage_->get_metadata("foundation_cfg", data) && !data.empty()) {
        FoundationConfig tmp;
        if (deserialize_foundation_config(data, tmp)) {
            foundation_config_ = std::move(tmp);
            return;
        }
        derived_state_storage_valid_ = false;

        GEO_LOG_ERROR("chainstate", "Invalid persisted foundation config; rebuild required");
    } else if (storage_->has_metadata("foundation_cfg") || chain_position_loaded_from_storage_) {
        derived_state_storage_valid_ = false;

        GEO_LOG_ERROR("chainstate", "Unreadable persisted foundation config; rebuild required");
    }
}

void ChainState::load_interest_pool_balance_from_storage() {
    interest_pool_balance_ = 0;
    if (!storage_) {
        return;
    }

    if (!storage_->has_metadata(kInterestPoolBalanceKeyLocal)) {
        if (chain_position_loaded_from_storage_) derived_state_storage_valid_ = false;
        return;
    }
    bytes data;
    if (!storage_->get_metadata(kInterestPoolBalanceKeyLocal, data) || data.size() != 8) {
        derived_state_storage_valid_ = false;

        GEO_LOG_ERROR("chainstate", "Invalid persisted interest pool balance; rebuild required");
        return;
    }

    uint64_t raw = 0;
    for (int i = 0; i < 8; i++) {
        raw |= static_cast<uint64_t>(data[i]) << (i * 8);
    }
    if (raw > static_cast<uint64_t>((std::numeric_limits<Amount>::max)())) {
        derived_state_storage_valid_ = false;

        GEO_LOG_ERROR("chainstate", "Out-of-range persisted interest pool balance; rebuild required");
        return;
    }
    interest_pool_balance_ = static_cast<Amount>(raw);
}

void ChainState::persist_interest_pool_balance_to_storage() {
    if (!storage_) {
        return;
    }

    bytes data(8, 0);
    const uint64_t raw = static_cast<uint64_t>(interest_pool_balance_);
    for (int i = 0; i < 8; i++) {
        data[i] = static_cast<byte>((raw >> (i * 8)) & 0xFF);
    }
    record_persistence_result_(
        storage_->put_metadata(kInterestPoolBalanceKeyLocal, data),
        "persist interest pool balance"
    );
}

} // namespace geocoin::state
