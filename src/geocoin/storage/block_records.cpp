#include "geocoin/storage/block_records.hpp"
#include "geocoin/utils/bytes.hpp"
#include "geocoin/consensus/geo_tag.hpp"
#include <cstring>
#include <exception>

namespace geocoin::storage {
namespace {
constexpr std::string_view kBlockIndexMagic = "BIR1";
constexpr std::string_view kPrunedAnchorMagic = "PIA1";

Height read_height(std::string_view value) {
    Height height = 0;
    for (size_t i = 0; i < 8; ++i) height |= static_cast<Height>(static_cast<byte>(value[i])) << (8 * i);
    return height;
}
}

bytes encode_chain_work_record(const uint256& value) {
    bytes data;
    data.reserve(32);
    for (const auto word : value.data) {
        const auto encoded = utils::encode_u64_le(word);
        data.insert(data.end(), encoded.begin(), encoded.end());
    }
    return data;
}

bytes encode_block_geography_record(const Hash256& hash, uint64_t geo_tag) {
    bytes out(hash.begin(), hash.end());
    const auto tag = utils::encode_u64_le(geo_tag);
    out.insert(out.end(), tag.begin(), tag.end());
    return out;
}
bool decode_block_geography_record(std::string_view value, Hash256& hash, uint64_t& geo_tag) {
    if (value.size() != 40) return false;
    std::copy_n(value.begin(), 32, hash.begin());
    geo_tag = read_height(value.substr(32));
    return hash != Hash256{} && consensus::geo::decode_geo_tag(geo_tag).has_value();
}

bool decode_chain_work_record(std::string_view value, uint256& out) {
    if (value.size() != 32) return false;
    for (size_t word = 0; word < 4; ++word) out.data[word] = read_height(value.substr(word * 8, 8));
    return true;
}

bytes encode_block_index_record(const core::BlockHeader& header, Height height) {
    bytes data(kBlockIndexMagic.begin(), kBlockIndexMagic.end());
    const auto encoded_height = utils::encode_u64_le(height);
    const auto encoded_header = header.serialize();
    data.reserve(12 + encoded_header.size());
    data.insert(data.end(), encoded_height.begin(), encoded_height.end());
    data.insert(data.end(), encoded_header.begin(), encoded_header.end());
    return data;
}

bool decode_block_index_record(const Hash256& hash, std::string_view value,
                               Height& height, core::BlockHeader& header) {
    if (value.size() != 12 + 124 || !value.starts_with(kBlockIndexMagic)) return false;
    try {
        auto decoded = core::BlockHeader::deserialize(bytes(value.begin() + 12, value.end()));
        if (decoded.get_hash() != hash) return false;
        height = read_height(value.substr(4, 8));
        header = decoded;
        return true;
    } catch (const std::exception&) { return false; }
}

bytes encode_pruned_index_anchor_record(Height height, const Hash256& hash, const uint256& chain_work) {
    bytes data(4 + 8 + 32 + 32);
    std::copy(kPrunedAnchorMagic.begin(), kPrunedAnchorMagic.end(), data.begin());
    const auto encoded_height = utils::encode_u64_le(height);
    std::copy(encoded_height.begin(), encoded_height.end(), data.begin() + 4);
    std::copy(hash.begin(), hash.end(), data.begin() + 12);
    const auto encoded_work = encode_chain_work_record(chain_work);
    std::copy(encoded_work.begin(), encoded_work.end(), data.begin() + 44);
    return data;
}

bool decode_pruned_index_anchor_record(std::string_view value, Height& height,
                                      Hash256& hash, uint256& chain_work) {
    if (value.size() != 4 + 8 + 32 + 32 || !value.starts_with(kPrunedAnchorMagic)) return false;
    height = read_height(value.substr(4, 8));
    std::memcpy(hash.data(), value.data() + 12, 32);
    return decode_chain_work_record(value.substr(44), chain_work);
}

} // namespace geocoin::storage
