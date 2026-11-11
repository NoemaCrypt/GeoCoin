#pragma once

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace geocoin::state::internal {

// Merge an ordered persisted cursor with pending puts/tombstones. Only the
// requested page is retained. Cursor::next must report iterator errors even
// when advancing past the last requested key.
template <class Cursor, class Updates>
std::vector<std::string> scan_metadata_page(Cursor& persisted, const Updates& updates,
                                          const std::string& prefix, size_t offset, size_t limit,
                                          size_t* total, const std::string& after, size_t strip = 0) {
    std::vector<std::string> result;
    size_t count = 0;
    auto pending = !after.empty() && after >= prefix ? updates.upper_bound(after) : updates.lower_bound(prefix);
    while (limit != 0 || total) {
        const bool has_base = persisted.valid() && persisted.key().starts_with(prefix);
        const bool has_pending = pending != updates.end() && pending->first.starts_with(prefix);
        if (!has_base && !has_pending) break;
        const std::string_view key = !has_pending || (has_base && persisted.key() < pending->first)
            ? persisted.key() : std::string_view(pending->first);
        const bool advance_base = has_base && persisted.key() == key;
        const bool advance_pending = has_pending && pending->first == key;
        const bool visible = !advance_pending || pending->second.has_value();
        if (visible) {
            if (count >= offset && result.size() < limit) result.emplace_back(key.substr(strip));
            ++count;
        }
        if (advance_base) persisted.next();
        if (advance_pending) ++pending;
        if (!total && result.size() == limit) break;
    }
    if (total) *total = count;
    return result;
}

template <class Map>
class MapKeyCursor {
public:
    MapKeyCursor(const Map& values, const std::string& prefix, const std::string& after)
        : end_(values.end()), current_(!after.empty() && after >= prefix ? values.upper_bound(after) : values.lower_bound(prefix)) {}
    bool valid() const { return current_ != end_; }
    std::string_view key() const { return current_->first; }
    void next() { ++current_; }

private:
    typename Map::const_iterator end_, current_;
};

} // namespace geocoin::state::internal
