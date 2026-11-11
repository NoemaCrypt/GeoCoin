#include "geocoin/state/storage.hpp"
#include "storage_scan.hpp"

#include <limits>

namespace geocoin::state {
namespace {
template <class Values, class Updates, class Key>
const typename Values::mapped_type* visible_value(const Values& values, const Updates& updates, const Key& key) {
    if (const auto pending = updates.find(key); pending != updates.end()) {
        return pending->second ? &*pending->second : nullptr;
    }
    const auto found = values.find(key);
    return found != values.end() ? &found->second : nullptr;
}

template <class Values, class Updates>
void apply_updates(Values& values, Updates& updates) {
    for (auto& [key, value] : updates) {
        if (value) values.insert_or_assign(key, std::move(*value));
        else values.erase(key);
    }
    updates.clear();
}
}

bool MemoryStorage::put_transaction(const Hash256& hash, const core::Transaction& tx) {
    if (tx.get_hash() != hash) return false;
    if (in_batch_) batch_transactions_[hash] = tx;
    else transactions_[hash] = tx;
    return true;
}

bool MemoryStorage::get_transaction(const Hash256& hash, core::Transaction& tx) const {
    const auto* value = visible_value(transactions_, batch_transactions_, hash);
    if (!value) return false;
    tx = *value;
    return true;
}

bool MemoryStorage::has_transaction(const Hash256& hash) const {
    return visible_value(transactions_, batch_transactions_, hash) != nullptr;
}

bool MemoryStorage::delete_transaction(const Hash256& hash) {
    if (in_batch_) batch_transactions_[hash] = std::nullopt;
    else transactions_.erase(hash);
    return true;
}

bool MemoryStorage::put_metadata(const std::string& key, const bytes& value) {
    if (in_batch_) batch_metadata_[key] = value;
    else metadata_[key] = value;
    return true;
}

bool MemoryStorage::get_metadata(const std::string& key, bytes& value) const {
    const auto* found = visible_value(metadata_, batch_metadata_, key);
    if (!found) { value.clear(); return false; }
    value = *found;
    return true;
}

bool MemoryStorage::has_metadata(const std::string& key) const {
    return visible_value(metadata_, batch_metadata_, key) != nullptr;
}

bool MemoryStorage::delete_metadata(const std::string& key) {
    if (in_batch_) batch_metadata_[key] = std::nullopt;
    else metadata_.erase(key);
    return true;
}

std::vector<std::string> MemoryStorage::get_all_keys() const {
    return get_keys_with_prefix("");
}

std::vector<std::string> MemoryStorage::get_keys_with_prefix(const std::string& prefix) const {
    return get_keys_page(prefix, 0, (std::numeric_limits<size_t>::max)());
}

std::vector<std::string> MemoryStorage::get_keys_page(const std::string& prefix, size_t offset,
                                                   size_t limit, size_t* total, const std::string& after) const {
    internal::MapKeyCursor cursor(metadata_, prefix, after);
    return internal::scan_metadata_page(cursor, batch_metadata_, prefix, offset, limit, total, after);
}

void MemoryStorage::begin_batch() {
    if (in_batch_) throw std::logic_error("Storage batch is already active");
    in_batch_ = true;
}

bool MemoryStorage::commit_batch() {
    if (!in_batch_) return false;
    apply_updates(transactions_, batch_transactions_);
    apply_updates(metadata_, batch_metadata_);
    in_batch_ = false;
    return true;
}

void MemoryStorage::abort_batch() {
    batch_transactions_.clear();
    batch_metadata_.clear();
    in_batch_ = false;
}

} // namespace geocoin::state
