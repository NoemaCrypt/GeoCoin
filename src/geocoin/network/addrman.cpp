#include "geocoin/network/addrman.hpp"
#include "geocoin/network/network_utils.hpp"
#include "geocoin/utils/logger.hpp"
#include "geocoin/utils/logging.hpp"
#include <openssl/rand.h>
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <algorithm>
#include <ctime>
#include <vector>
#include "geocoin/network/peer_file.hpp"

namespace geocoin::network {

namespace {
inline bool NodeVerboseEnabled() {
    static const bool enabled = (std::getenv("GEOCOIN_NODE_VERBOSE") != nullptr);
    return enabled;
}

bool normalize_endpoint(
    const std::string& address,
    std::string& endpoint,
    std::string& host,
    uint16_t& port
) {
    endpoint.clear();
    host.clear();
    port = 0;
    if (!NetworkUtils::split_host_port(address, host, port)) {
        return false;
    }
    if (host.empty() || host == "0.0.0.0" || host == "::") {
        return false;
    }
    endpoint = NetworkUtils::make_endpoint(host, port);
    return true;
}

std::string address_group_key(const std::string& address) {
    std::string endpoint;
    std::string host;
    uint16_t port = 0;
    if (!normalize_endpoint(address, endpoint, host, port)) {
        return {};
    }
    (void)endpoint;
    (void)port;
    return NetworkUtils::network_group(host);
}

bool secure_random_below(uint64_t upper_bound, uint64_t& result) {
    if (upper_bound == 0) {
        return false;
    }

    // Rejection sampling avoids the modulo bias that would otherwise make some
    // entries slightly easier for an eclipse attacker to predict or favor.
    const uint64_t minimum = (uint64_t{0} - upper_bound) % upper_bound;
    uint64_t random_value = 0;
    do {
        if (RAND_bytes(
                reinterpret_cast<unsigned char*>(&random_value),
                static_cast<int>(sizeof(random_value))) != 1) {
            return false;
        }
    } while (random_value < minimum);

    result = random_value % upper_bound;
    return true;
}
}

AddrMan::AddrMan() = default;

AddrMan::~AddrMan() {
}

bool AddrMan::add(const std::string& address, const std::string& source) {
    return add_impl(address, source, {});
}

bool AddrMan::add_from_gossip(
    const std::string& address,
    const std::string& source_endpoint,
    uint16_t expected_p2p_port
) {
    std::string endpoint;
    std::string host;
    uint16_t port = 0;
    if (expected_p2p_port == 0 ||
        !normalize_endpoint(address, endpoint, host, port) ||
        port != expected_p2p_port ||
        !NetworkUtils::is_global_unicast_ip(host)) {
        return false;
    }

    std::string source_host;
    uint16_t source_port = 0;
    if (NetworkUtils::split_host_port(source_endpoint, source_host, source_port)) {
        (void)source_port;
    } else if (!NetworkUtils::canonicalize_numeric_ip(source_endpoint, source_host)) {
        return false;
    }
    const std::string source_group = NetworkUtils::network_group(source_host);
    if (source_group.empty()) {
        return false;
    }
    return add_impl(endpoint, "addr:" + source_endpoint, source_group);
}

bool AddrMan::add_impl(
    const std::string& address,
    const std::string& source,
    const std::string& gossip_source_group
) {
    std::string canonical_address;
    std::string host;
    uint16_t port = 0;
    if (!normalize_endpoint(address, canonical_address, host, port)) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    // Если адрес уже есть, не добавляем
    if (addrs_.find(canonical_address) != addrs_.end()) {
        return false;
    }

    if (!gossip_source_group.empty() && max_entries_per_gossip_source_ > 0) {
        const size_t source_count = static_cast<size_t>(std::count_if(
            gossip_sources_.begin(),
            gossip_sources_.end(),
            [&](const auto& entry) { return entry.second == gossip_source_group; }
        ));
        if (source_count >= max_entries_per_gossip_source_) {
            return false;
        }
    }

    const std::string group = NetworkUtils::network_group(host);
    if (!group.empty() && max_entries_per_group_ > 0) {
        size_t group_count = 0;
        for (const auto& [addr, info] : addrs_) {
            (void)info;
            if (address_group_key(addr) == group) {
                ++group_count;
            }
        }
        if (group_count >= max_entries_per_group_ && !evict_one_unlocked(group)) {
            return false;
        }
    }

    if (max_entries_ > 0) {
        while (addrs_.size() >= max_entries_) {
            if (!evict_one_unlocked()) {
                return false;
            }
        }
    }

    AddrInfo info;
    info.address = canonical_address;
    addrs_[canonical_address] = info;
    if (!gossip_source_group.empty()) {
        gossip_sources_[canonical_address] = gossip_source_group;
    }

    if (NodeVerboseEnabled()) {
        geocoin::utils::LogPrintf("AddrMan: Added %s from %s\n", canonical_address.c_str(), source.c_str());
    }

    return true;
}

size_t AddrMan::add(const std::vector<std::string>& addresses, const std::string& source) {
    size_t added = 0;
    for (const auto& addr : addresses) {
        if (add(addr, source)) {
            added++;
        }
    }
    return added;
}

bool AddrMan::remove(const std::string& address) {
    std::string canonical_address;
    std::string host;
    uint16_t port = 0;
    if (!normalize_endpoint(address, canonical_address, host, port)) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    gossip_sources_.erase(canonical_address);
    return addrs_.erase(canonical_address) > 0;
}

std::string AddrMan::select() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (addrs_.empty()) {
        return "";
    }

    const std::time_t wall_time = std::time(nullptr);
    const uint64_t now = wall_time > 0 ? static_cast<uint64_t>(wall_time) : 0;

    // Собираем кандидатов (не "ужасные" и не пробовали недавно)
    std::vector<std::pair<std::string, uint64_t>> candidates;
    std::vector<std::string> fallback;
    uint64_t total_weight = 0;

    for (const auto& [addr, info] : addrs_) {
        // Пропускаем "ужасные" адреса
        if (info.is_terrible()) {
            continue;
        }

        fallback.push_back(addr);

        // Пропускаем адреса, которые пробовали менее 10 минут назад
        if (info.last_try > now || now - info.last_try < 600) {
            continue;
        }

        // Healthy entries have attempts in [0, 3]. Integer weights preserve
        // the old 1/(1+attempts) ratios without floating-point edge cases.
        const int attempts = std::clamp(info.attempts, 0, 3);
        const uint64_t weight = 12U / static_cast<uint64_t>(1 + attempts);
        candidates.push_back({addr, weight});
        total_weight += weight;
    }

    if (candidates.empty()) {
        if (fallback.empty()) {
            return "";
        }
        if (fallback.size() == 1) {
            return fallback.front();
        }

        uint64_t index = 0;
        if (!secure_random_below(static_cast<uint64_t>(fallback.size()), index)) {
            geocoin::utils::LogPrintf("AddrMan: secure random peer selection failed\n");
            return "";
        }
        return fallback[static_cast<size_t>(index)];
    }

    if (candidates.size() == 1) {
        return candidates.front().first;
    }

    uint64_t selected_weight = 0;
    if (!secure_random_below(total_weight, selected_weight)) {
        geocoin::utils::LogPrintf("AddrMan: secure random peer selection failed\n");
        return "";
    }

    for (const auto& [address, weight] : candidates) {
        if (selected_weight < weight) {
            return address;
        }
        selected_weight -= weight;
    }

    // total_weight is the exact sum of the small integer weights above.
    return "";
}

void AddrMan::attempt(const std::string& address, bool success) {
    std::string canonical_address;
    std::string host;
    uint16_t port = 0;
    if (!normalize_endpoint(address, canonical_address, host, port)) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = addrs_.find(canonical_address);
    if (it == addrs_.end()) {
        return;
    }

    uint64_t now = std::time(nullptr);
    it->second.last_try = now;

    if (success) {
        it->second.last_success = now;
        it->second.attempts = 0;
    } else {
        if (it->second.attempts < 4) {
            ++it->second.attempts;
        }
    }
}

void AddrMan::good(const std::string& address) {
    attempt(address, true);
}

void AddrMan::set_limits(size_t max_entries, size_t max_entries_per_group) {
    std::lock_guard<std::mutex> lock(mutex_);
    max_entries_ = std::max<size_t>(1, max_entries);
    max_entries_per_group_ = std::max<size_t>(1, max_entries_per_group);
    while (addrs_.size() > max_entries_) {
        if (!evict_one_unlocked()) {
            break;
        }
    }
}

size_t AddrMan::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return addrs_.size();
}

bool AddrMan::load(const std::string& filepath) {
    std::ifstream file(filepath);
    if (!file.is_open()) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    addrs_.clear();
    gossip_sources_.clear();

    std::string line;
    int loaded = 0;

    while (std::getline(file, line)) {
        if (addrs_.size() >= max_entries_) {
            break;
        }
        // Формат: address last_try last_success attempts
        std::istringstream iss(line);
        AddrInfo info;

        if (!(iss >> info.address >> info.last_try >> info.last_success >> info.attempts)) {
            continue;
        }

        // Сбрасываем last_try и attempts чтобы адрес был доступен сразу после загрузки
        info.last_try = 0;
        info.attempts = 0;

        std::string canonical_address;
        std::string host;
        uint16_t port = 0;
        if (normalize_endpoint(info.address, canonical_address, host, port)) {
            info.address = canonical_address;
            const std::string group = NetworkUtils::network_group(host);
            if (!group.empty() && max_entries_per_group_ > 0) {
                size_t group_count = 0;
                for (const auto& [addr, existing] : addrs_) {
                    (void)existing;
                    if (address_group_key(addr) == group) {
                        ++group_count;
                    }
                }
                if (group_count >= max_entries_per_group_) {
                    continue;
                }
            }
            addrs_[info.address] = info;
            loaded++;
        }
    }

    if (NodeVerboseEnabled()) {
        geocoin::utils::LogPrintf("AddrMan: Loaded %d addresses from %s\n", loaded, filepath.c_str());
    }

    return true;
}

bool AddrMan::save(const std::string& filepath) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!detail::write_peer_file(filepath, [&](std::ostream& file) {
        for (const auto& [addr, info] : addrs_) {
            file << info.address << " " << info.last_try << " "
                 << info.last_success << " " << info.attempts << "\n";
        }
    })) {
        return false;
    }

    if (NodeVerboseEnabled()) {
        geocoin::utils::LogPrintf("AddrMan: Saved %zu addresses to %s\n", addrs_.size(), filepath.c_str());
    }

    return true;
}

void AddrMan::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    addrs_.clear();
    gossip_sources_.clear();
}

std::vector<std::string> AddrMan::get_all() const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<std::string> result;
    for (const auto& [addr, info] : addrs_) {
        result.push_back(addr);
    }

    return result;
}

bool AddrMan::evict_one_unlocked(const std::string& preferred_group) {
    if (addrs_.empty()) {
        return false;
    }

    auto best = addrs_.end();
    for (auto it = addrs_.begin(); it != addrs_.end(); ++it) {
        if (!preferred_group.empty() && address_group_key(it->first) != preferred_group) {
            continue;
        }
        if (best == addrs_.end()) {
            best = it;
            continue;
        }

        const AddrInfo& a = it->second;
        const AddrInfo& b = best->second;
        if (a.is_terrible() != b.is_terrible()) {
            if (a.is_terrible()) {
                best = it;
            }
            continue;
        }
        if (a.last_success != b.last_success) {
            if (a.last_success < b.last_success) {
                best = it;
            }
            continue;
        }
        if (a.last_try != b.last_try) {
            if (a.last_try < b.last_try) {
                best = it;
            }
            continue;
        }
        if (a.attempts > b.attempts) {
            best = it;
        }
    }

    if (best == addrs_.end()) {
        return false;
    }
    gossip_sources_.erase(best->first);
    addrs_.erase(best);
    return true;
}

} // namespace geocoin::network
