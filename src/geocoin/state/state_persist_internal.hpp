#pragma once

#include "geocoin/state/storage.hpp"
#include "geocoin/utils/bytes.hpp"

namespace geocoin::state::internal {

// Registry keys use a nonzero canonical hash. Read errors propagate; invalid
// records are reported without modifying the original stored values.
template <class LoadRecord>
bool load_hash_metadata_records(Storage& storage, const std::string& prefix, LoadRecord&& load_record) {
    constexpr size_t page_size = 1024;
    bool valid = true;
    std::string after;
    while (true) {
        const auto keys = storage.get_keys_page(prefix, 0, page_size, nullptr, after);
        for (const auto& key : keys) {
            const auto id_hex = key.substr(prefix.size());
            Hash256 id{};
            bytes raw;
            if (!utils::hex_to_hash256_checked(id_hex, id) || utils::hash256_to_hex(id) != id_hex ||
                utils::is_null_hash256(id) || !storage.get_metadata(key, raw) ||
                !load_record(id_hex, id, raw)) valid = false;
        }
        if (keys.size() < page_size) break;
        after = keys.back();
    }
    return valid;
}

} // namespace geocoin::state::internal
