#include "geocoin/network/peer_discovery.hpp"
#include "geocoin/network/peer_file.hpp"
#include "geocoin/network/dns_seeds.h"
#include "geocoin/network/dns_resolver.hpp"
#include "geocoin/network/network_utils.hpp"
#include "geocoin/utils/logger.hpp"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <ctime>
#include <thread>
#include <filesystem>
#include <limits>

namespace geocoin::network {
namespace {

bool parse_u64_strict(const std::string& raw, uint64_t& out) {
    if (raw.empty()) {
        return false;
    }
    uint64_t value = 0;
    for (unsigned char c : raw) {
        if (c < '0' || c > '9') {
            return false;
        }
        const uint64_t digit = static_cast<uint64_t>(c - '0');
        if (value > (std::numeric_limits<uint64_t>::max() - digit) / 10) {
            return false;
        }
        value = value * 10 + digit;
    }
    out = value;
    return true;
}

bool normalize_peer_endpoint(const std::string& ip, uint16_t port, std::string& normalized_ip) {
    if (port == 0) {
        return false;
    }
    normalized_ip = NetworkUtils::normalize_ip_address(ip);
    if (normalized_ip.empty() || normalized_ip == "0.0.0.0" || normalized_ip == "::") {
        return false;
    }
    return true;
}

bool parse_peer_endpoint(const std::string& endpoint, std::string& ip, uint16_t& port) {
    return NetworkUtils::split_host_port(endpoint, ip, port) &&
           normalize_peer_endpoint(ip, port, ip);
}

}  // namespace

PeerDiscovery::PeerDiscovery(const std::string& peers_file)
    : peers_file_(peers_file)
    , dns_seed_port_(DEFAULT_P2P_PORT)
    , max_attempts_(3)
    , ban_time_(86400)  // 24 часа
{
}

PeerDiscovery::~PeerDiscovery() {
    stop_dns_thread();
    save_to_file();
}

void PeerDiscovery::set_max_attempts(uint32_t max) {
    std::lock_guard<std::mutex> lock(mutex_);
    max_attempts_ = max;
}

void PeerDiscovery::set_ban_time(uint64_t seconds) {
    std::lock_guard<std::mutex> lock(mutex_);
    ban_time_ = seconds;
}

void PeerDiscovery::set_max_peers(size_t max_peers) {
    std::lock_guard<std::mutex> lock(mutex_);
    max_peers_ = max_peers == 0 ? 1 : max_peers;

    while (peers_.size() > max_peers_) {
        auto victim = std::max_element(
            peers_.begin(),
            peers_.end(),
            [](const auto& lhs, const auto& rhs) {
                const auto& a = lhs.second;
                const auto& b = rhs.second;
                if (a.is_seed != b.is_seed) {
                    return a.is_seed && !b.is_seed;
                }
                if (a.attempts != b.attempts) {
                    return a.attempts < b.attempts;
                }
                if (a.last_seen != b.last_seen) {
                    return a.last_seen > b.last_seen;
                }
                return lhs.first < rhs.first;
            }
        );
        if (victim == peers_.end()) {
            break;
        }
        peers_.erase(victim);
    }
}

void PeerDiscovery::initialize(bool use_testnet) {
    // Добавляем DNS seed-узлы
    const auto& seeds = get_dns_seed_hostnames(use_testnet);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        dns_seeds_ = seeds;
        dns_seed_port_ = use_testnet ? DEFAULT_TESTNET_P2P_PORT : DEFAULT_P2P_PORT;
    }

    // Загружаем из файла
    load_from_file();

    // НЕ загружаем из DNS синхронно! (как в Bitcoin)
    // DNS разрешение будет запущено асинхронно через start_dns_thread()
    GEO_LOG_INFO("peers", "PeerDiscovery initialized with " << count() << " peers from file");
}

void PeerDiscovery::add_dns_seed(const std::string& hostname) {
    std::lock_guard<std::mutex> lock(mutex_);
    dns_seeds_.push_back(hostname);
}

void PeerDiscovery::add_peer(const std::string& ip, uint16_t port, bool is_seed) {
    std::lock_guard<std::mutex> lock(mutex_);

    std::string normalized_ip;
    if (!normalize_peer_endpoint(ip, port, normalized_ip)) {
        return;
    }
    std::string key = NetworkUtils::make_endpoint(normalized_ip, port);

    auto it = peers_.find(key);
    if (it != peers_.end()) {
        // Обновляем существующего пира
        it->second.last_seen = current_time();
        if (is_seed) {
            it->second.is_seed = true;
        }
    } else {
        // Добавляем нового пира
        if (peers_.size() >= max_peers_) {
            return;
        }
        DiscoveredPeer info;
        info.ip = normalized_ip;
        info.port = port;
        info.services = 0;
        info.last_seen = current_time();
        info.attempts = 0;
        info.is_seed = is_seed;

        peers_[key] = info;
    }
}

void PeerDiscovery::add_peer(const DiscoveredPeer& info) {
    std::lock_guard<std::mutex> lock(mutex_);

    std::string normalized_ip;
    if (!normalize_peer_endpoint(info.ip, info.port, normalized_ip)) {
        return;
    }
    DiscoveredPeer normalized = info;
    normalized.ip = normalized_ip;
    std::string key = NetworkUtils::make_endpoint(normalized.ip, normalized.port);
    if (peers_.find(key) == peers_.end() && peers_.size() >= max_peers_) {
        return;
    }
    peers_[key] = normalized;
}

void PeerDiscovery::remove_peer(const std::string& ip, uint16_t port) {
    std::lock_guard<std::mutex> lock(mutex_);

    std::string normalized_ip;
    if (!normalize_peer_endpoint(ip, port, normalized_ip)) {
        return;
    }
    std::string key = NetworkUtils::make_endpoint(normalized_ip, port);
    peers_.erase(key);
}

void PeerDiscovery::mark_connected(const std::string& ip, uint16_t port) {
    std::lock_guard<std::mutex> lock(mutex_);

    std::string normalized_ip;
    if (!normalize_peer_endpoint(ip, port, normalized_ip)) {
        return;
    }
    std::string key = NetworkUtils::make_endpoint(normalized_ip, port);
    auto it = peers_.find(key);

    if (it != peers_.end()) {
        it->second.last_seen = current_time();
        it->second.attempts = 0;  // Сбрасываем счетчик попыток
    }
}

void PeerDiscovery::mark_failed(const std::string& ip, uint16_t port) {
    std::lock_guard<std::mutex> lock(mutex_);

    std::string normalized_ip;
    if (!normalize_peer_endpoint(ip, port, normalized_ip)) {
        return;
    }
    std::string key = NetworkUtils::make_endpoint(normalized_ip, port);
    auto it = peers_.find(key);

    if (it != peers_.end()) {
        it->second.attempts++;

        // Если превышен лимит попыток, удаляем (кроме seed нод)
        if (it->second.attempts >= max_attempts_ && !it->second.is_seed) {
            peers_.erase(it);
        }
    }
}

std::vector<DiscoveredPeer> PeerDiscovery::get_peers(size_t count) {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<DiscoveredPeer> result;
    result.reserve(count);

    // Собираем всех доступных пиров
    std::vector<DiscoveredPeer> available;
    for (const auto& [key, info] : peers_) {
        if (!is_banned(info)) {
            available.push_back(info);
        }
    }

    // Сортируем по приоритету:
    // 1. Seed ноды
    // 2. Недавно виденные
    // 3. Меньше попыток подключения
    std::sort(available.begin(), available.end(),
        [](const DiscoveredPeer& a, const DiscoveredPeer& b) {
            if (a.is_seed != b.is_seed) return a.is_seed;
            if (a.attempts != b.attempts) return a.attempts < b.attempts;
            return a.last_seen > b.last_seen;
          });

    // Берем первые N
    size_t take = std::min(count, available.size());
    result.insert(result.end(), available.begin(), available.begin() + take);

    return result;
}

std::vector<DiscoveredPeer> PeerDiscovery::get_all_peers() {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<DiscoveredPeer> result;
    result.reserve(peers_.size());

    for (const auto& [key, info] : peers_) {
        result.push_back(info);
    }

    return result;
}

size_t PeerDiscovery::load_from_dns() {
    std::vector<std::string> seeds;
    uint16_t port = 0;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        seeds = dns_seeds_;
        port = dns_seed_port_;
    }

    size_t total_found = 0;

    if (!seeds.empty()) {
        GEO_LOG_INFO("peers", "Resolving DNS seeds...");
        auto addresses = DNSResolver::resolve_seeds_with_port(seeds, port);

        for (const auto& address : addresses) {
            std::string ip;
            uint16_t port = 0;
            if (!parse_peer_endpoint(address, ip, port)) {
                continue;
            }
            add_peer(ip, port, true);
            total_found++;
        }

        GEO_LOG_INFO("peers", "Found " << total_found << " peers from DNS");
    }

    GEO_LOG_INFO("peers", "Total peers from DNS: " << total_found);

    return total_found;
}

bool PeerDiscovery::save_to_file() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!detail::write_peer_file(peers_file_, [&](std::ostream& file) {
        file << "# GeoCoin Peers Database\n";
        file << "# Format: ip,port,services,last_seen,attempts,is_seed\n\n";
        for (const auto& [key, info] : peers_) {
            file << info.ip << "," << info.port << "," << info.services << ","
                 << info.last_seen << "," << info.attempts << ","
                 << (info.is_seed ? "1" : "0") << "\n";
        }
    })) {
        GEO_LOG_ERROR("peers", "Failed to save peers file: " << peers_file_);
        return false;
    }

    GEO_LOG_INFO("peers", "Saved " << peers_.size() << " peers to " << peers_file_);
    return true;
}

bool PeerDiscovery::load_from_file() {
    std::ifstream file(peers_file_);
    if (!file.is_open()) {
        GEO_LOG_INFO("peers", "Peers file not found: " << peers_file_);
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    std::string line;
    size_t loaded = 0;

    while (std::getline(file, line)) {
        // Пропускаем комментарии и пустые строки
        if (line.empty() || line[0] == '#') {
            continue;
        }

        // Парсим: ip,port,services,last_seen,attempts,is_seed
        std::istringstream iss(line);
        std::string ip, port_str, services_str, last_seen_str, attempts_str, is_seed_str;

        if (std::getline(iss, ip, ',') &&
            std::getline(iss, port_str, ',') &&
            std::getline(iss, services_str, ',') &&
            std::getline(iss, last_seen_str, ',') &&
            std::getline(iss, attempts_str, ',') &&
            std::getline(iss, is_seed_str)) {

            uint64_t parsed_port = 0;
            uint64_t parsed_services = 0;
            uint64_t parsed_last_seen = 0;
            uint64_t parsed_attempts = 0;
            std::string normalized_ip;
            if (!parse_u64_strict(port_str, parsed_port) ||
                parsed_port == 0 ||
                parsed_port > 65535 ||
                !parse_u64_strict(services_str, parsed_services) ||
                !parse_u64_strict(last_seen_str, parsed_last_seen) ||
                !parse_u64_strict(attempts_str, parsed_attempts) ||
                parsed_attempts > std::numeric_limits<uint32_t>::max() ||
                !normalize_peer_endpoint(ip, static_cast<uint16_t>(parsed_port), normalized_ip) ||
                (is_seed_str != "0" && is_seed_str != "1")) {
                GEO_LOG_WARN("peers", "Failed to parse peer line: " << line);
                continue;
            }

            if (peers_.size() >= max_peers_) {
                break;
            }

            DiscoveredPeer info;
            info.ip = normalized_ip;
            info.port = static_cast<uint16_t>(parsed_port);
            info.services = parsed_services;
            info.last_seen = parsed_last_seen;
            info.attempts = static_cast<uint32_t>(parsed_attempts);
            info.is_seed = (is_seed_str == "1");

            std::string key = NetworkUtils::make_endpoint(info.ip, info.port);
            peers_[key] = info;
            loaded++;
        }
    }

    GEO_LOG_INFO("peers", "Loaded " << loaded << " peers from " << peers_file_);
    return true;
}

void PeerDiscovery::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    peers_.clear();
}

size_t PeerDiscovery::count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return peers_.size();
}

bool PeerDiscovery::is_banned(const DiscoveredPeer& info) const {
    if (info.is_seed) {
        return false;  // Сид-ноды никогда не банятся
    }

    if (info.attempts < max_attempts_) {
        return false;
    }

    uint64_t now = current_time();
    uint64_t ban_until = info.last_seen + ban_time_;

    return now < ban_until;
}

uint64_t PeerDiscovery::current_time() {
    return static_cast<uint64_t>(std::time(nullptr));
}

void PeerDiscovery::start_dns_thread(size_t peer_count, bool force) {
    std::lock_guard<std::mutex> lifecycle_lock(dns_lifecycle_mutex_);
    // Останавливаем предыдущий поток если он еще работает
    stop_dns_thread_unlocked();

    // Запускаем новый поток
    dns_thread_running_ = true;
    try {
        dns_thread_ = std::thread([this, peer_count, force]() {
            try {
                // Как в Bitcoin: только запрашиваем DNS если нужда острая
                // (addrman.size() > 0 && !forcednsseed) -> ждем 11 секунд и проверяем пиров
                if (count() > 0 && !force) {
                    GEO_LOG_INFO("peers", "DNS: Waiting 11 seconds to check if peers are available...");

                    // Ждем 11 секунд (как в Bitcoin)
                    for (int i = 0; i < 110 && dns_thread_running_; i++) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    }

                    if (!dns_thread_running_) {
                        return;  // Поток остановлен
                    }

                    // Проверяем количество пиров (как в Bitcoin: if (vNodes.size() >= 2))
                    if (peer_count >= 2) {
                        GEO_LOG_INFO("peers", "P2P peers available (" << peer_count << "). Skipped DNS seeding.");
                        dns_thread_running_ = false;
                        return;
                    }
                }

                GEO_LOG_INFO("peers", "Loading addresses from DNS seeds (could take a while)...");

                // Загружаем из DNS
                size_t found = load_from_dns();

                GEO_LOG_INFO("peers", found << " addresses found from DNS seeds");

                // Сохраняем в файл
                if (found > 0) {
                    save_to_file();
                }

            } catch (const std::exception& error) {
                GEO_LOG_ERROR("peers", "DNS worker failed: " << error.what());
            } catch (...) {
                GEO_LOG_ERROR("peers", "DNS worker failed");
            }
            dns_thread_running_ = false;
        });
    } catch (...) {
        dns_thread_running_ = false;
        throw;
    }
}

void PeerDiscovery::stop_dns_thread() {
    std::lock_guard<std::mutex> lifecycle_lock(dns_lifecycle_mutex_);
    stop_dns_thread_unlocked();
}

void PeerDiscovery::stop_dns_thread_unlocked() {
    dns_thread_running_ = false;
    // Completion does not make a std::thread non-joinable.
    if (dns_thread_.joinable()) {
        dns_thread_.join();
    }
}

} // namespace geocoin::network
