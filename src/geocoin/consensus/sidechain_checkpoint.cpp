#include "geocoin/consensus/sidechain_checkpoint.hpp"

#include "geocoin/consensus/consensus.hpp"
#include "geocoin/core/merkle.hpp"
#include "geocoin/crypto/geo_area.hpp"
#include "geocoin/utils/bytes.hpp"

#include <algorithm>
#include <limits>

namespace geocoin::consensus::sidechain_checkpoint {

namespace {

constexpr size_t kFinancialProofHeaderSize = 4 + 1 + 1 + 2;
constexpr size_t kFinancialProofFixedPayloadSize = 32 + 8 + 8 + 4;
constexpr size_t kFinancialProofBaseSize =
    kFinancialProofHeaderSize + kFinancialProofFixedPayloadSize;
constexpr size_t kAccountingProofSize = 4 + 1 + 3 + 32 + 8 + 32;

struct FinancialProofV2 {
    Hash256 state_root{};
    Height checkpoint_height = 0;
    uint64_t total_supply = 0;
    uint32_t leaf_index = 0;
    std::vector<Hash256> merkle_path;
};

struct AccountingProofV1 {
    Hash256 state_root{};
    Height checkpoint_height = 0;
    Hash256 schema_hash{};
};

uint32_t read_u32_le(const bytes& in, size_t offset) {
    uint32_t value = 0;
    for (size_t i = 0; i < sizeof(value); ++i) {
        value |= static_cast<uint32_t>(in[offset + i]) << (i * 8);
    }
    return value;
}

bool parse_financial_proof_v2(const bytes& proof, FinancialProofV2& out) {
    if (proof.size() < kFinancialProofBaseSize ||
        proof[0] != 'S' || proof[1] != 'C' || proof[2] != 'P' || proof[3] != '2' ||
        proof[4] != 1 || proof[6] != 0 || proof[7] != 0) {
        return false;
    }

    const size_t path_length = proof[5];
    if (path_length > params::MAX_BATCH_CHECKPOINT_MERKLE_DEPTH ||
        proof.size() != kFinancialProofBaseSize + path_length * Hash256{}.size()) {
        return false;
    }

    size_t offset = kFinancialProofHeaderSize;
    std::copy_n(proof.begin() + offset, out.state_root.size(), out.state_root.begin());
    offset += out.state_root.size();
    out.checkpoint_height = static_cast<Height>(utils::read_u64_le_unchecked(proof, offset));
    offset += sizeof(uint64_t);
    out.total_supply = utils::read_u64_le_unchecked(proof, offset);
    offset += sizeof(uint64_t);
    out.leaf_index = read_u32_le(proof, offset);
    offset += sizeof(uint32_t);

    out.merkle_path.clear();
    out.merkle_path.reserve(path_length);
    for (size_t i = 0; i < path_length; ++i) {
        Hash256 sibling{};
        std::copy_n(proof.begin() + offset, sibling.size(), sibling.begin());
        out.merkle_path.push_back(sibling);
        offset += sibling.size();
    }
    return true;
}

bool parse_accounting_proof_v1(const bytes& proof, AccountingProofV1& out) {
    if (proof.size() != kAccountingProofSize ||
        proof[0] != 'S' || proof[1] != 'C' || proof[2] != 'A' || proof[3] != '1' ||
        proof[4] != 1 || proof[5] != 0 || proof[6] != 0 || proof[7] != 0) {
        return false;
    }

    size_t offset = 8;
    std::copy_n(proof.begin() + offset, out.state_root.size(), out.state_root.begin());
    offset += out.state_root.size();
    out.checkpoint_height = static_cast<Height>(utils::read_u64_le_unchecked(proof, offset));
    offset += sizeof(uint64_t);
    std::copy_n(proof.begin() + offset, out.schema_hash.size(), out.schema_hash.begin());
    return true;
}

} // namespace

Hash256 build_financial_supply_leaf_v2(
    const Hash256& sidechain_id,
    Height checkpoint_height,
    uint64_t total_supply
) {
    static constexpr char kDomain[] = "geocoin:sidechain:v2:supply_leaf";
    bytes data;
    data.insert(data.end(), kDomain, kDomain + sizeof(kDomain) - 1);
    data.insert(data.end(), sidechain_id.begin(), sidechain_id.end());
    utils::append_u64_le(data, checkpoint_height);
    utils::append_u64_le(data, total_supply);
    return crypto::double_sha256(data);
}

Hash256 build_financial_commitment_v2(
    const Hash256& sidechain_id,
    const Hash256& state_root,
    Height checkpoint_height,
    uint64_t total_supply
) {
    static constexpr char kDomain[] = "geocoin:sidechain:v2:checkpoint_commitment";
    bytes data;
    data.insert(data.end(), kDomain, kDomain + sizeof(kDomain) - 1);
    data.insert(data.end(), sidechain_id.begin(), sidechain_id.end());
    data.insert(data.end(), state_root.begin(), state_root.end());
    utils::append_u64_le(data, checkpoint_height);
    utils::append_u64_le(data, total_supply);
    return crypto::double_sha256(data);
}

bytes build_financial_proof_v2(
    const Hash256& state_root,
    Height checkpoint_height,
    uint64_t total_supply,
    uint32_t leaf_index,
    const std::vector<Hash256>& merkle_path
) {
    bytes proof;
    proof.reserve(kFinancialProofBaseSize + merkle_path.size() * Hash256{}.size());
    proof.insert(proof.end(), {'S', 'C', 'P', '2', 1});
    proof.push_back(static_cast<byte>(merkle_path.size()));
    proof.push_back(0);
    proof.push_back(0);
    proof.insert(proof.end(), state_root.begin(), state_root.end());
    utils::append_u64_le(proof, checkpoint_height);
    utils::append_u64_le(proof, total_supply);
    utils::append_u32_le(proof, leaf_index);
    for (const auto& sibling : merkle_path) {
        proof.insert(proof.end(), sibling.begin(), sibling.end());
    }
    return proof;
}

bool verify_financial_v2(
    const core::BatchCheckpointItem& item,
    Amount issuance_limit,
    Amount total_issued
) {
    if (item.proof.empty() || item.proof.size() > params::MAX_BATCH_CHECKPOINT_PROOF) {
        return false;
    }

    FinancialProofV2 proof;
    if (!parse_financial_proof_v2(item.proof, proof) ||
        proof.checkpoint_height == 0 ||
        proof.checkpoint_height != item.checkpoint_height ||
        proof.total_supply > static_cast<uint64_t>((std::numeric_limits<Amount>::max)())) {
        return false;
    }

    const Amount total_supply = static_cast<Amount>(proof.total_supply);
    if (total_issued < 0 ||
        proof.total_supply != static_cast<uint64_t>(total_issued) ||
        total_supply < 0 ||
        total_supply > issuance_limit) {
        return false;
    }
    if (!core::merkle_leaf_index_in_range(proof.merkle_path.size(), proof.leaf_index)) {
        return false;
    }

    const Hash256 supply_leaf = build_financial_supply_leaf_v2(
        item.sidechain_id,
        proof.checkpoint_height,
        proof.total_supply
    );
    if (!core::verify_merkle_path(
            supply_leaf,
            proof.merkle_path,
            proof.state_root,
            proof.leaf_index)) {
        return false;
    }

    return item.checkpoint == build_financial_commitment_v2(
        item.sidechain_id,
        proof.state_root,
        proof.checkpoint_height,
        proof.total_supply
    );
}

Hash256 build_accounting_commitment_v1(
    const Hash256& sidechain_id,
    const Hash256& state_root,
    Height checkpoint_height,
    const Hash256& schema_hash
) {
    static constexpr char kDomain[] =
        "geocoin:sidechain:accounting:v1:checkpoint_commitment";
    bytes data;
    data.insert(data.end(), kDomain, kDomain + sizeof(kDomain) - 1);
    data.insert(data.end(), sidechain_id.begin(), sidechain_id.end());
    data.insert(data.end(), state_root.begin(), state_root.end());
    utils::append_u64_le(data, checkpoint_height);
    data.insert(data.end(), schema_hash.begin(), schema_hash.end());
    return crypto::double_sha256(data);
}

bytes build_accounting_proof_v1(
    const Hash256& state_root,
    Height checkpoint_height,
    const Hash256& schema_hash
) {
    bytes proof;
    proof.reserve(kAccountingProofSize);
    proof.insert(proof.end(), {'S', 'C', 'A', '1', 1, 0, 0, 0});
    proof.insert(proof.end(), state_root.begin(), state_root.end());
    utils::append_u64_le(proof, checkpoint_height);
    proof.insert(proof.end(), schema_hash.begin(), schema_hash.end());
    return proof;
}

bool verify_accounting_v1(
    const core::BatchCheckpointItem& item,
    const Hash256& schema_hash
) {
    AccountingProofV1 proof;
    if (!parse_accounting_proof_v1(item.proof, proof) ||
        proof.checkpoint_height == 0 ||
        proof.checkpoint_height != item.checkpoint_height ||
        proof.schema_hash != schema_hash) {
        return false;
    }

    return item.checkpoint == build_accounting_commitment_v1(
        item.sidechain_id,
        proof.state_root,
        proof.checkpoint_height,
        proof.schema_hash
    );
}

} // namespace geocoin::consensus::sidechain_checkpoint
