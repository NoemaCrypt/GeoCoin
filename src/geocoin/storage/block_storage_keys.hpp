#pragma once

namespace geocoin::storage::internal::block_keys {
inline constexpr const char* kPruneStatsKey = "prune_stats";
inline constexpr const char* kWitnessPrunedHeightKey = "witness_pruned_height";
inline constexpr const char* kBlockBodyPruneNextHeightKey = "block_body_prune_next_height";
inline constexpr const char* kPrunedIndexAnchorKey = "pruned_index_anchor";
inline constexpr const char* kWitnessPayloadPrefix = "witness:";
inline constexpr const char* kWitnessPayloadHashPrefix = "witnessh:";
inline constexpr const char* kBlockIndexPrefix = "bi:";
inline constexpr const char* kFailedBlockPrefix = "bf:";
inline constexpr const char* kConsensusRulesetKey = "consensus_ruleset";
inline constexpr const char* kSplitWritePendingKey = "split_write_pending";
inline constexpr const char kActiveTransitionMagic[4] = {'A', 'C', 'T', '1'};

} // namespace geocoin::storage::internal::block_keys
