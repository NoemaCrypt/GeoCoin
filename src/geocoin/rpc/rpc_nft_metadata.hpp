#pragma once

#include "geocoin/rpc/rpc_types.hpp"

namespace geocoin::nft {
struct NFTMetadata;
}

namespace geocoin::rpc {

nft::NFTMetadata parse_nft_metadata_from_rpc(const json& value, const nft::NFTMetadata& defaults);
json nft_metadata_to_json(const nft::NFTMetadata& metadata);
json nft_geography_to_json(const nft::NFTMetadata& metadata);

} // namespace geocoin::rpc
