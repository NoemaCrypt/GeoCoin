#include "geocoin/network/addrman.hpp"
#include "geocoin/network/network_utils.hpp"
#include "geocoin/network/peer_discovery.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

std::filesystem::path temp_file(const std::string& suffix) {
    return std::filesystem::temp_directory_path() /
        ("geocoin-network-policy-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
         suffix);
}

bool contains_addr(const std::vector<std::string>& addrs, const std::string& addr) {
    return std::find(addrs.begin(), addrs.end(), addr) != addrs.end();
}

const geocoin::network::DiscoveredPeer* find_peer(
    const std::vector<geocoin::network::DiscoveredPeer>& peers,
    const std::string& ip,
    uint16_t port
) {
    const auto it = std::find_if(peers.begin(), peers.end(), [&](const auto& peer) {
        return peer.ip == ip && peer.port == port;
    });
    return it == peers.end() ? nullptr : &*it;
}

void test_addrman_limits() {
    geocoin::network::AddrMan addrman;
    addrman.set_limits(3, 2);

    check(addrman.add("10.1.1.1:4040", "test"), "first group address is accepted");
    check(addrman.add("10.1.1.2:4040", "test"), "second group address is accepted");
    check(addrman.add("10.1.1.3:4040", "test"), "third group address evicts within group and is accepted");
    check(addrman.size() == 2, "per-group AddrMan cap is enforced");

    check(addrman.add("10.1.2.1:4040", "test"), "second group address is accepted");
    check(addrman.add("10.1.3.1:4040", "test"), "global cap evicts one address and accepts replacement");
    check(addrman.size() == 3, "global AddrMan cap is enforced");

    check(!addrman.add("0.0.0.0:4040", "test"), "wildcard IPv4 address is rejected");
    check(!addrman.add("[::]:4040", "test"), "wildcard IPv6 address is rejected");
    check(!addrman.add("127.0.0.1:0", "test"), "zero port address is rejected");
}

void test_addrman_selection_and_persistence() {
    {
        geocoin::network::AddrMan addrman;
        check(addrman.select().empty(), "empty AddrMan has no selectable address");
        addrman.set_limits(10, 10);
        check(addrman.add(std::vector<std::string>{
                  "203.0.113.10:3030",
                  "203.0.113.11:3030",
                  "203.0.113.10:3030",
                  "0.0.0.0:3030",
              }, "batch") == 2,
              "batch add counts only unique valid addresses");
        check(addrman.size() == 2, "batch add leaves two valid addresses");
        check(!addrman.add("203.0.113.10:3030", "duplicate"), "duplicate address is rejected");
        check(addrman.remove("203.0.113.11:3030"), "remove deletes an existing address");
        check(!addrman.remove("203.0.113.11:3030"), "remove reports missing address");
        check(addrman.size() == 1, "remove updates AddrMan size");

        const std::string selected = addrman.select();
        check(selected == "203.0.113.10:3030", "select returns the only healthy candidate");
        addrman.attempt("203.0.113.250:3030", false);
        addrman.attempt("203.0.113.10:3030", false);
        check(addrman.select() == "203.0.113.10:3030",
              "recently tried but non-terrible address remains fallback selectable");
        addrman.attempt("203.0.113.10:3030", false);
        addrman.attempt("203.0.113.10:3030", false);
        addrman.attempt("203.0.113.10:3030", false);
        check(addrman.select().empty(), "terrible address is not selected");
        addrman.good("203.0.113.10:3030");
        check(addrman.select() == "203.0.113.10:3030", "good resets attempts and restores fallback selection");
        addrman.clear();
        check(addrman.size() == 0 && addrman.select().empty(), "clear removes all addresses");
    }

    {
        geocoin::network::AddrMan addrman;
        addrman.set_limits(4, 2);
        check(addrman.add("[2001:db8:0:0::1]:3030", "ipv6"), "first IPv6 group address is accepted");
        check(addrman.add("[2001:db8:0:0::2]:3030", "ipv6"), "second IPv6 group address is accepted");
        check(addrman.add("[2001:db8:0:0::3]:3030", "ipv6"),
              "third IPv6 group address evicts within group and is accepted");
        check(addrman.size() == 2, "IPv6 group limit is enforced");
    }

    {
        geocoin::network::AddrMan missing;
        const auto missing_path = temp_file(".missing/peers.dat");
        check(!missing.load(missing_path.string()), "load reports missing AddrMan file");
        check(!missing.save(missing_path.string()), "save reports unwritable AddrMan path");
    }

    const auto peers_path = temp_file(".addrman");
    const auto saved_path = temp_file(".addrman.saved");
    {
        std::ofstream out(peers_path);
        out << "203.0.113.1:3030 111 222 3\n";
        out << "bad-line\n";
        out << "0.0.0.0:3030 1 2 3\n";
        out << "203.0.113.2:3030 1 2 3\n";
        out << "198.51.100.1:3030 1 2 3\n";
    }

    geocoin::network::AddrMan loaded;
    loaded.set_limits(3, 1);
    check(loaded.load(peers_path.string()), "AddrMan loads a valid peers file");
    const auto loaded_addrs = loaded.get_all();
    check(loaded.size() == 2, "AddrMan load skips invalid rows and enforces group limit");
    check(contains_addr(loaded_addrs, "203.0.113.1:3030"), "AddrMan load keeps first group member");
    check(contains_addr(loaded_addrs, "198.51.100.1:3030"), "AddrMan load keeps separate group member");
    check(loaded.save(saved_path.string()), "AddrMan saves known addresses");

    geocoin::network::AddrMan reloaded;
    check(reloaded.load(saved_path.string()), "AddrMan reloads saved file");
    check(reloaded.size() == 2, "AddrMan save/load roundtrip preserves address count");

    std::filesystem::remove(peers_path);
    std::filesystem::remove(saved_path);
}

void test_peer_discovery_file_policy() {
    const auto path = temp_file(".dat");
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << "# test peers\n";
        out << "127.0.0.1,4040,1,2,3,1\n";
        out << "0.0.0.0,4040,1,2,3,0\n";
        out << "127.0.0.2,70000,1,2,3,0\n";
        out << "127.0.0.3,4040,not-number,2,3,0\n";
        out << "127.0.0.4,4040,1,2,3,0\n";
        out << "127.0.0.5,4040,1,2,3,0\n";
    }

    {
        geocoin::network::PeerDiscovery discovery(path.string());
        discovery.set_max_peers(2);
        check(discovery.load_from_file(), "PeerDiscovery loads an existing file");
        check(discovery.count() == 2, "PeerDiscovery enforces max peers and skips invalid rows");

        discovery.add_peer("0.0.0.0", 4040);
        check(discovery.count() == 2, "PeerDiscovery rejects wildcard add_peer input");
        discovery.add_peer("127.0.0.9", 0);
        check(discovery.count() == 2, "PeerDiscovery rejects zero-port add_peer input");
    }

    check(std::filesystem::exists(path), "PeerDiscovery destructor writes peers file atomically");
    std::filesystem::remove(path);
    std::filesystem::remove(path.string() + ".tmp");
}

void test_peer_discovery_lifecycle_policy() {
    const auto path = temp_file(".peers.lifecycle");
    {
        geocoin::network::PeerDiscovery discovery(path.string());
        discovery.set_max_attempts(2);
        discovery.set_ban_time(3600);
        discovery.set_max_peers(3);

        discovery.add_peer("0.0.0.0", 3030);
        discovery.add_peer("127.0.0.1", 0);
        check(discovery.count() == 0, "PeerDiscovery rejects invalid direct peers");

        discovery.add_peer("192.0.2.10", 3030);
        discovery.add_peer("192.0.2.10", 3030, true);
        discovery.add_peer("192.0.2.11", 3030);
        discovery.add_peer("192.0.2.12", 3030);
        discovery.add_peer("192.0.2.13", 3030);
        check(discovery.count() == 3, "PeerDiscovery enforces max peer cap on direct add");

        auto all = discovery.get_all_peers();
        const auto* upgraded_seed = find_peer(all, "192.0.2.10", 3030);
        check(upgraded_seed != nullptr && upgraded_seed->is_seed, "duplicate direct add upgrades peer to seed");

        geocoin::network::DiscoveredPeer overflow_info;
        overflow_info.ip = "198.51.100.99";
        overflow_info.port = 3030;
        overflow_info.services = 7;
        overflow_info.last_seen = static_cast<uint64_t>(std::time(nullptr));
        overflow_info.attempts = 0;
        overflow_info.is_seed = false;
        discovery.add_peer(overflow_info);
        check(discovery.count() == 3, "PeerDiscovery enforces max peer cap on DiscoveredPeer add");

        discovery.set_max_peers(2);
        check(discovery.count() == 2, "PeerDiscovery trims peers when max cap shrinks");

        discovery.remove_peer("0.0.0.0", 3030);
        discovery.remove_peer("192.0.2.12", 3030);
        check(discovery.count() <= 2, "PeerDiscovery remove handles invalid and optional existing peer");

        discovery.clear();
        check(discovery.count() == 0, "PeerDiscovery clear removes peers");
        discovery.set_max_peers(3);

        geocoin::network::DiscoveredPeer banned;
        banned.ip = "198.51.100.10";
        banned.port = 3030;
        banned.services = 1;
        banned.last_seen = static_cast<uint64_t>(std::time(nullptr));
        banned.attempts = 2;
        banned.is_seed = false;
        discovery.add_peer(banned);

        geocoin::network::DiscoveredPeer seed;
        seed.ip = "198.51.100.11";
        seed.port = 3030;
        seed.services = 1;
        seed.last_seen = 1;
        seed.attempts = 99;
        seed.is_seed = true;
        discovery.add_peer(seed);

        geocoin::network::DiscoveredPeer normal;
        normal.ip = "198.51.100.12";
        normal.port = 3030;
        normal.services = 1;
        normal.last_seen = static_cast<uint64_t>(std::time(nullptr)) + 1;
        normal.attempts = 0;
        normal.is_seed = false;
        discovery.add_peer(normal);

        const auto peers = discovery.get_peers(10);
        check(peers.size() == 2, "PeerDiscovery get_peers excludes temporarily banned non-seed");
        check(peers[0].ip == "198.51.100.11" && peers[0].is_seed,
              "PeerDiscovery prioritizes seed peers even with many attempts");
        check(discovery.get_peers(1).size() == 1, "PeerDiscovery get_peers respects requested count");

        discovery.mark_connected("198.51.100.10", 3030);
        all = discovery.get_all_peers();
        const auto* reset_peer = find_peer(all, "198.51.100.10", 3030);
        check(reset_peer != nullptr && reset_peer->attempts == 0, "mark_connected resets attempts");

        discovery.mark_failed("198.51.100.12", 3030);
        discovery.mark_failed("198.51.100.12", 3030);
        check(find_peer(discovery.get_all_peers(), "198.51.100.12", 3030) == nullptr,
              "mark_failed removes non-seed peer at attempt limit");
        discovery.mark_failed("198.51.100.11", 3030);
        discovery.mark_failed("198.51.100.11", 3030);
        check(find_peer(discovery.get_all_peers(), "198.51.100.11", 3030) != nullptr,
              "mark_failed does not remove seed peer");

        discovery.mark_connected("0.0.0.0", 3030);
        discovery.mark_failed("0.0.0.0", 3030);
        discovery.remove_peer("0.0.0.0", 3030);
        check(discovery.count() >= 1, "invalid peer lifecycle calls are no-ops");

        check(discovery.save_to_file(), "PeerDiscovery saves lifecycle peers");
        discovery.clear();
        check(discovery.load_from_file(), "PeerDiscovery reloads lifecycle peers");
        check(discovery.count() >= 1, "PeerDiscovery reload restores saved peers");

        check(discovery.load_from_dns() == 0, "PeerDiscovery DNS load with no seeds is empty and offline");
        discovery.start_dns_thread(0, true);
        discovery.stop_dns_thread();
    }
    std::filesystem::remove(path);
    std::filesystem::remove(path.string() + ".tmp");

    {
        geocoin::network::PeerDiscovery missing(temp_file(".missing/peers.dat").string());
        check(!missing.load_from_file(), "PeerDiscovery load reports missing file");
        check(!missing.save_to_file(), "PeerDiscovery save reports unwritable path");
    }

    {
        const auto init_path = temp_file(".peers.init");
        {
            std::ofstream out(init_path, std::ios::binary | std::ios::trunc);
            out << "203.0.113.30,3030,1,2,0,0\n";
        }
        geocoin::network::PeerDiscovery initialized(init_path.string());
        initialized.initialize(false);
        check(initialized.count() == 1, "PeerDiscovery initialize loads peers file");
        initialized.start_dns_thread(2, false);
        initialized.stop_dns_thread();
        std::filesystem::remove(init_path);
        std::filesystem::remove(init_path.string() + ".tmp");
    }
}

void test_endpoint_parser_policy() {
    std::string host;
    uint16_t port = 0;

    check(geocoin::network::NetworkUtils::split_host_port("127.0.0.1:4040", host, port),
          "valid IPv4 endpoint parses");
    check(host == "127.0.0.1" && port == 4040, "valid IPv4 endpoint values are preserved");

    check(geocoin::network::NetworkUtils::split_host_port("[::1]:4040", host, port),
          "valid bracketed IPv6 endpoint parses");
    check(host == "::1" && port == 4040, "valid IPv6 endpoint values are preserved");

    check(!geocoin::network::NetworkUtils::split_host_port("127.0.0.1:4040junk", host, port),
          "endpoint parser rejects trailing junk after port");
    check(!geocoin::network::NetworkUtils::split_host_port("127.0.0.1:0", host, port),
          "endpoint parser rejects zero port");
    check(!geocoin::network::NetworkUtils::split_host_port("127.0.0.1:70000", host, port),
          "endpoint parser rejects oversized port");
    check(!geocoin::network::NetworkUtils::split_host_port("127.0.0.1:40 40", host, port),
          "endpoint parser rejects whitespace in port");
    check(!geocoin::network::NetworkUtils::split_host_port("127.0.0.1:", host, port),
          "endpoint parser rejects empty port");
    check(!geocoin::network::NetworkUtils::split_host_port("127.0.0.1:123456", host, port),
          "endpoint parser rejects overlong port token");
    check(!geocoin::network::NetworkUtils::split_host_port("127.0.0.1", host, port),
          "endpoint parser rejects endpoint without port");
    check(!geocoin::network::NetworkUtils::split_host_port("2001:db8::1:3030", host, port),
          "endpoint parser rejects unbracketed IPv6 endpoint");
    check(!geocoin::network::NetworkUtils::split_host_port("[::1]4040", host, port),
          "endpoint parser rejects bracketed IPv6 without port separator");
    check(!geocoin::network::NetworkUtils::split_host_port("[bad host]:4040", host, port),
          "endpoint parser rejects whitespace inside bracketed host");
    check(!geocoin::network::NetworkUtils::split_host_port(
              std::string(300, 'a') + ":4040", host, port),
          "endpoint parser rejects overlong host");
    check(!geocoin::network::NetworkUtils::split_host_port(std::string(600, 'a'), host, port),
          "endpoint parser rejects overlong endpoint");
    check(!geocoin::network::NetworkUtils::split_host_port("[::1:4040", host, port),
          "endpoint parser rejects missing IPv6 closing bracket");
    check(!geocoin::network::NetworkUtils::split_host_port("[]:4040", host, port),
          "endpoint parser rejects empty bracketed host");
    check(!geocoin::network::NetworkUtils::split_host_port("127.0.0.1:abcd", host, port),
          "endpoint parser rejects non-numeric port");

    check(geocoin::network::NetworkUtils::parse_peer_address("Seed.Example.NET", host, port),
          "peer address parser accepts bare host with default port");
    check(host == "seed.example.net" && port == 3030,
          "peer address parser normalizes bare host and applies default port");
    check(!geocoin::network::NetworkUtils::parse_peer_address("bad host", host, port),
          "peer address parser rejects whitespace in bare host");
    check(!geocoin::network::NetworkUtils::parse_peer_address("seed.example.net", host, port, 0),
          "peer address parser rejects bare host without default port");
    check(!geocoin::network::NetworkUtils::parse_peer_address(std::string(300, 'a'), host, port),
          "peer address parser rejects overlong bare host");

    check(geocoin::network::NetworkUtils::normalize_ip_address("").empty(),
          "empty address normalizes to empty string");
    check(geocoin::network::NetworkUtils::normalize_ip_address("[::FFFF:192.0.2.1]") == "192.0.2.1",
          "IPv4-mapped IPv6 addresses normalize to IPv4");
    check(geocoin::network::NetworkUtils::normalize_ip_address("::FFFF:not-ip") == "::ffff:not-ip",
          "invalid IPv4-mapped suffix remains normalized IPv6-like text");
    check(geocoin::network::NetworkUtils::make_endpoint("2001:DB8::1", 3030) == "[2001:db8::1]:3030",
          "make_endpoint brackets normalized IPv6 hosts");
    check(geocoin::network::NetworkUtils::make_endpoint("Seed.Example.NET", 3030) == "seed.example.net:3030",
          "make_endpoint lowercases DNS hosts");
    check(!geocoin::network::NetworkUtils::is_self_address("not-an-endpoint", 3030),
          "self-address check rejects malformed endpoints");
    check(!geocoin::network::NetworkUtils::is_self_address("127.0.0.1:3031", 3030),
          "self-address check rejects local host with different port");
    check(geocoin::network::NetworkUtils::is_self_address("127.0.0.1:3030", 3030),
          "self-address check accepts loopback with matching port");
    check(geocoin::network::NetworkUtils::is_local_address("localhost"),
          "local-address check accepts localhost");
    check(geocoin::network::NetworkUtils::is_local_address("::"),
          "local-address check accepts wildcard IPv6 local address");
    const auto local_addresses = geocoin::network::NetworkUtils::get_local_addresses();
    for (const auto& local_ip : local_addresses) {
        check(!geocoin::network::NetworkUtils::normalize_ip_address(local_ip).empty(),
              "local address list contains normalizable entries");
    }
}

void test_numeric_ip_and_network_group_policy() {
    using geocoin::network::NetworkUtils;

    std::string canonical;
    check(NetworkUtils::canonicalize_numeric_ip(
              "2001:0DB8:0000:0000:0000:0000:0000:0001", canonical) &&
              canonical == "2001:db8::1",
          "expanded IPv6 literal canonicalizes to compressed lowercase form");
    check(NetworkUtils::canonicalize_numeric_ip("[2606:4700:4700::1111]", canonical) &&
              canonical == "2606:4700:4700::1111",
          "bracketed compressed IPv6 literal canonicalizes successfully");
    check(!NetworkUtils::canonicalize_numeric_ip("Seed.Example.NET", canonical),
          "numeric IP canonicalizer rejects DNS names");
    check(NetworkUtils::normalize_ip_address(
              "2001:0DB8:0000:0000:0000:0000:0000:0001") == "2001:db8::1",
          "normalizer uses binary IPv6 canonicalization");

    const std::string ipv4_group = NetworkUtils::network_group("8.8.8.1");
    check(ipv4_group == "8.8.8.0/24", "IPv4 network group is a canonical /24");
    check(ipv4_group == NetworkUtils::network_group("8.8.8.254"),
          "IPv4 addresses in the same /24 share a network group");
    check(ipv4_group != NetworkUtils::network_group("8.8.9.1"),
          "IPv4 addresses in different /24s have different network groups");

    const std::string ipv6_group = NetworkUtils::network_group("2606:4700:4700::1");
    check(ipv6_group == "2606:4700:4700::/64", "IPv6 network group is a canonical /64");
    check(ipv6_group == NetworkUtils::network_group(
              "2606:4700:4700:0000:0000:0000:0000:abcd"),
          "compressed and expanded IPv6 addresses in the same /64 share a group");
    check(ipv6_group != NetworkUtils::network_group("2606:4700:4700:1::1"),
          "IPv6 addresses in different /64s have different network groups");

    check(NetworkUtils::abuse_key("8.8.8.1") == "8.8.8.1",
          "IPv4 abuse limits remain per public address");
    check(NetworkUtils::abuse_key("::ffff:192.0.2.9") == "192.0.2.9",
          "IPv4-mapped abuse keys canonicalize to IPv4");
    check(NetworkUtils::abuse_key("2606:4700:4700::1") == ipv6_group,
          "IPv6 abuse limits group rotating interface identifiers by /64");
    check(NetworkUtils::abuse_key("2606:4700:4700::abcd") ==
              NetworkUtils::abuse_key("2606:4700:4700::1"),
          "IPv6 addresses in one routed prefix cannot bypass connection limits");
    check(NetworkUtils::abuse_key("not-an-ip").empty(),
          "abuse keys reject non-numeric network identities");

    check(NetworkUtils::is_global_unicast_ip("8.8.8.8"),
          "public IPv4 is accepted as global unicast");
    check(NetworkUtils::is_global_unicast_ip("2606:4700:4700::1111"),
          "public IPv6 is accepted as global unicast");

    const std::vector<std::string> rejected = {
        "10.1.2.3",             // RFC1918
        "172.16.1.2",           // RFC1918
        "192.168.1.2",          // RFC1918
        "127.0.0.1",            // loopback
        "169.254.1.2",          // link-local
        "224.0.0.1",            // multicast
        "192.0.2.1",            // documentation
        "::1",                  // loopback
        "fe80::1",              // link-local
        "fc00::1",              // unique-local
        "ff02::1",              // multicast
        "2001:db8::1",          // documentation
        "seed.example.net",     // hostname
    };
    for (const auto& address : rejected) {
        check(!NetworkUtils::is_global_unicast_ip(address),
              "non-global address is rejected: " + address);
    }
}

void test_addrman_gossip_policy() {
    using geocoin::network::AddrMan;

    constexpr uint16_t p2p_port = 3030;
    const std::string source = "9.9.9.9:3030";

    AddrMan addrman;
    check(addrman.add("127.0.0.1:3030", "manual"),
          "manual AddrMan input still accepts localhost");
    check(addrman.add("10.23.45.67:3030", "manual"),
          "manual AddrMan input still accepts private test-network addresses");
    check(addrman.add("node.test.invalid:3030", "manual"),
          "manual AddrMan input still accepts hostnames");

    const std::vector<std::string> rejected_gossip = {
        "10.1.2.3:3030",              // RFC1918
        "172.16.1.2:3030",            // RFC1918
        "192.168.1.2:3030",           // RFC1918
        "127.99.88.77:3030",          // loopback
        "169.254.10.20:3030",         // link-local
        "224.0.0.1:3030",             // multicast
        "192.0.2.10:3030",            // documentation
        "[::1]:3030",                 // loopback
        "[fe80::1]:3030",             // link-local
        "[fc00::1]:3030",             // unique-local
        "[ff02::1]:3030",             // multicast
        "[2001:db8::10]:3030",        // documentation
        "internal.example.test:3030", // hostname
        "8.8.4.4:4040",               // wrong P2P port
    };
    for (const auto& address : rejected_gossip) {
        check(!addrman.add_from_gossip(address, source, p2p_port),
              "untrusted gossip address is rejected: " + address);
    }

    check(addrman.add_from_gossip("8.8.8.8:3030", source, p2p_port),
          "public IPv4 gossip address on the configured P2P port is accepted");
    check(addrman.add_from_gossip(
              "[2606:4700:4700:0000:0000:0000:0000:1111]:3030", source, p2p_port),
          "public IPv6 gossip address on the configured P2P port is accepted");
    const auto accepted = addrman.get_all();
    check(contains_addr(accepted, "8.8.8.8:3030"),
          "accepted public IPv4 gossip is stored");
    check(contains_addr(accepted, "[2606:4700:4700::1111]:3030"),
          "accepted expanded IPv6 gossip is stored canonically");

    AddrMan quota_limited;
    for (int i = 0; i < 64; ++i) {
        const std::string address =
            "11." + std::to_string(i) + ".0.1:" + std::to_string(p2p_port);
        check(quota_limited.add_from_gossip(address, source, p2p_port),
              "gossip source can contribute address " + std::to_string(i + 1) + " of 64");
    }
    check(quota_limited.size() == 64, "per-source gossip quota permits exactly 64 entries");
    check(!quota_limited.add_from_gossip("12.0.0.1:3030", source, p2p_port),
          "per-source gossip quota rejects the 65th entry");
    check(quota_limited.size() == 64, "rejected gossip does not grow AddrMan past source quota");
}

} // namespace

int main() {
    test_addrman_limits();
    test_addrman_selection_and_persistence();
    test_peer_discovery_file_policy();
    test_peer_discovery_lifecycle_policy();
    test_endpoint_parser_policy();
    test_numeric_ip_and_network_group_policy();
    test_addrman_gossip_policy();

    if (failures == 0) {
        std::cout << "Network policy tests passed.\n";
        return 0;
    }

    std::cerr << failures << " network policy test(s) failed.\n";
    return 1;
}
