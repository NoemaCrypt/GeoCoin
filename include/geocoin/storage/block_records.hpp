#pragma once

#include "geocoin/core/block.hpp"
#include <string_view>

namespace geocoin::storage {

// Current record codecs shared by the live store and offline snapshot checks.
bytes encode_chain_work_record(const uint256& value);
bool decode_chain_work_record(std::string_view value, uint256& out);
bytes encode_block_geography_record(const Hash256& hash, uint64_t geo_tag);
bool decode_block_geography_record(std::string_view value, Hash256& hash, uint64_t& geo_tag);
bytes encode_block_index_record(const core::BlockHeader& header, Height height);
bool decode_block_index_record(const Hash256& hash, std::string_view value,
                               Height& height, core::BlockHeader& header);
bytes encode_pruned_index_anchor_record(Height height, const Hash256& hash, const uint256& chain_work);
bool decode_pruned_index_anchor_record(std::string_view value, Height& height,
                                      Hash256& hash, uint256& chain_work);

} // namespace geocoin::storage
