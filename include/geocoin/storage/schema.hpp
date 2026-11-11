#pragma once
#include <array>
#include <set>
#include <string>
#include <string_view>

namespace geocoin::storage::schema {
enum class Store : size_t { BLOCKS, INDEX, STATE, ADDRESSES };
inline constexpr std::array<std::string_view, 4> paths{
    "chaindata/blocks", "index/blocks", "chaindata/state", "addresses/state"};

struct Record {
    std::string_view key;
    bool prefix;
    Store store;
    const char* column;
    int bytes; // -1: variable, otherwise exact length.
    const char* fields;
};

// Physical keys, including the state backend's meta: envelope. Index values
// carry references, never another material copy of a block or transaction.
inline constexpr Record records[] = {
    {"b:", true, Store::BLOCKS, "block_bodies", -1, "block header; ordered transactions"},
    {"witness:", true, Store::BLOCKS, "witness_payloads", -1, "ordered transaction/input witness payloads"},
    {"witnessh:", true, Store::BLOCKS, "witness_payload_hashes", 32, "SHA-256 of witness payload"},
    {"bi:", true, Store::INDEX, "block_index", 136, "BIR1; height:u64; header:124"},
    {"bf:", true, Store::INDEX, "block_index", 1, "permanently invalid block"},
    {"hh:", true, Store::INDEX, "height_index", 0, "height and block hash in key"},
    {"a:", true, Store::INDEX, "active_chain", 32, "active block hash at height"},
    {"geo:", true, Store::INDEX, "geography_archive", 40, "pruned active block hash:32; GeoPoW v1 tag:u64; height in key"},
    {"pc:", true, Store::INDEX, "parent_child", 0, "parent and child hash in key"},
    {"bw:", true, Store::INDEX, "chainwork", 32, "cumulative chain work"},
    {"best", false, Store::INDEX, "active_chain", 40, "height:u64; highest stored block hash:32"},
    {"active_tip", false, Store::INDEX, "active_chain", 40, "height:u64; active block hash:32"},
    {"active_transition", false, Store::INDEX, "active_chain", 86, "ACT1; kind:u8; old tip exists:u8; old height/hash; new height/hash"},
    {"consensus_ruleset", false, Store::INDEX, "default", -1, "consensus ruleset identifier"},
    {"policy_checkpoint", false, Store::INDEX, "pruning_meta", 40, "height:u64; block hash:32"},
    {"prune_stats", false, Store::INDEX, "pruning_meta", 32, "total blocks; pruned blocks; body bytes reduced; total bytes (u64)"},
    {"witness_pruned_height", false, Store::INDEX, "pruning_meta", 8, "witness pruning cursor:u64"},
    {"block_body_prune_next_height", false, Store::INDEX, "pruning_meta", 8, "next body pruning height:u64"},
    {"pruned_index_anchor", false, Store::INDEX, "pruning_meta", 76, "PIA1; height:u64; hash:32; chain work:32"},
    {"split_write_pending", false, Store::INDEX, "pruning_meta", 1, "unfinished block/index write"},
    {"tx:", true, Store::STATE, "default", -1, "archived transaction; or retained checkpoint evidence"},
    {"meta:chainstate_height", false, Store::STATE, "default", 8, "applied state height:u64"},
    {"meta:chainstate_tip_hash", false, Store::STATE, "default", 32, "applied state tip hash"},
    {"meta:chainstate_utxo_count", false, Store::STATE, "default", 8, "UTXO count:u64"},
    {"meta:query_index_tip", false, Store::STATE, "default", 40, "height:u64; hash:32 of indexed state"},
    {"meta:chainstate_journal", false, Store::STATE, "default", 42, "version:u8; phase:u8; height:u64; hash:32"},
    {"meta:chainstate_rebuild_pending", false, Store::STATE, "default", 1, "unfinished state rebuild"},
    {"meta:idx_epoch", false, Store::STATE, "default", 32, "32 lowercase hexadecimal characters"},
    {"meta:idx:", true, Store::STATE, "default", -1, "epoch and kind and identity in key; dp value:txid32; other kinds:1"},
    {"meta:atx:", true, Store::STATE, "default", 48, "height:u64; active block hash:32; transaction count:u64"},
    {"meta:qaddr:", true, Store::STATE, "default", 0, "hex address; height20; txid in key"},
    {"meta:qnft:", true, Store::STATE, "default", 0, "hex NFT identifier; height20; txid in key"},
    {"meta:qrefs:", true, Store::STATE, "default", -1, "txid in key; sorted forward query keys, each prefixed by u32 length"},
    {"meta:undo:", true, Store::STATE, "default", -1, "height; block hash; transaction undo list; interest pool delta"},
    {"meta:delegation:", true, Store::STATE, "default", -1, "DGR1; primary address; nonce:u32; total spent:i64; proof key:32"},
    {"meta:scd:", true, Store::STATE, "default", -1, "owner; amount; unlock height; checkpoint; L1 checkpoint height; sidechain checkpoint height"},
    {"meta:sc:reg:", true, Store::STATE, "default", -1, "owner; issuance limit; total issued; multiplier; fee bps; max multiplier; checkpoint policy/version; symbol; committee; kind/status; decommission height; namespace; schema hash"},
    {"meta:card:prog:", true, Store::STATE, "default", -1, "issuer; vendor; OS; AID; applet version/hash; policy; scope; created height; explicit corporate policy version and role authorities"},
    {"meta:card:uid:", true, Store::STATE, "default", -1, "program; issuer; UID; public key hash; validity; status; metadata hash; issued/updated heights"},
    {"meta:nft:owner:", true, Store::STATE, "default", -1, "current owner address"},
    {"meta:nft:meta:", true, Store::STATE, "default", -1, "canonical NFTMetadata payload"},
    {"meta:foundation_cfg", false, Store::STATE, "default", -1, "address; m; n; participants"},
    {"meta:interest_pool_balance", false, Store::STATE, "default", 8, "nonnegative interest pool balance:i64"},
    {"meta:wpk:", true, Store::STATE, "default", 897, "witness public key identified by SHA-256"},
    {"meta:wpkref:", true, Store::STATE, "default", 8, "positive witness public key reference count:u64"},
    {"meta:wpk_stats", false, Store::STATE, "default", 24, "cache hits; cache misses; bytes saved (u64)"},
    {"meta:utxo:", true, Store::ADDRESSES, "default", -1, "txid; output index; value; address; height; coinbase flag"},
    {"meta:addr:utxo:", true, Store::ADDRESSES, "default", 0, "address and UTXO key; no material value copy"}
};

inline const Record* find(std::string_view key) {
    for (const auto& record : records) {
        if (record.prefix ? key.starts_with(record.key) : key == record.key) return &record;
    }
    return nullptr;
}

inline std::set<std::string> columns(Store store) {
    std::set<std::string> result{"default"};
    for (const auto& record : records) if (record.store == store) result.insert(record.column);
    return result;
}
}
