// Regression coverage for the auto-connect loop's handling of AddrMan
// candidates that PeerManager currently holds in outbound backoff.
//
// Before the fix, a backed-off candidate made the loop `continue` straight
// back to AddrMan::select() without sleeping and charged the address one
// failed attempt per spin. A single real connection failure therefore made
// the endpoint "terrible" (attempts > 3) within microseconds, excluding it
// from address selection for the rest of the session, and the loop skipped
// its periodic peers.dat save while spinning.

#include "geocoin/node/node.hpp"
#include "geocoin/network/peer_manager.hpp"
#include "rpc_test_utils.hpp"
#include "test_utils.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

using namespace std::chrono_literals;
using geocoin::test::expect;

struct PersistedAddress {
    uint64_t last_try = 0;
    uint64_t last_success = 0;
    int attempts = 0;
};

// Format written by AddrMan::save(): "address last_try last_success attempts".
std::map<std::string, PersistedAddress> read_peers_file(const std::filesystem::path& path)
{
    std::map<std::string, PersistedAddress> entries;
    std::ifstream file(path);
    expect(file.is_open(), "peers.dat is readable");
    std::string line;
    while (std::getline(file, line)) {
        std::istringstream iss(line);
        std::string address;
        PersistedAddress info;
        if (iss >> address >> info.last_try >> info.last_success >> info.attempts) {
            entries[address] = info;
        }
    }
    return entries;
}

std::optional<geocoin::network::PeerOutboundAttemptInfo> find_attempt(
    geocoin::node::Node& node,
    const std::string& target
)
{
    const auto diagnostics = node.get_peer_manager()->get_connection_diagnostics();
    const auto it = std::find_if(
        diagnostics.outbound_attempts.begin(),
        diagnostics.outbound_attempts.end(),
        [&](const auto& attempt) { return attempt.address == target; }
    );
    if (it == diagnostics.outbound_attempts.end()) {
        return std::nullopt;
    }
    return *it;
}

bool wait_for_completed_attempt(
    geocoin::node::Node& node,
    const std::string& target,
    std::chrono::steady_clock::duration budget
)
{
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto attempt = find_attempt(node, target);
        if (attempt && !attempt->in_progress && attempt->fail_count >= 1) {
            return true;
        }
        std::this_thread::sleep_for(25ms);
    }
    return false;
}

geocoin::node::NodeConfig loadnet_autoconnect_config(const std::filesystem::path& dir)
{
    geocoin::node::NodeConfig config;
    config.data_dir = dir.string();
    // LOADNET has no DNS or hardcoded public seeds, so the only outbound
    // candidates are the loopback fallbacks and whatever peers.dat contains.
    config.network = "degree-load";
    config.enable_auto_connect = true;
    config.enable_p2p_listen = false;
    config.enable_mining = false;
    config.dns_seed_delay_ms = 0;
    config.outbound_connect_timeout_ms = 1;
    return config;
}

void test_backoff_skip_is_not_charged_as_addrman_failure()
{
    const auto dir = geocoin::test::make_temp_dir("geocoin-node-connection-backoff");
    std::error_code ec;
    try {
        // Nothing listens here: every connect fails immediately with
        // ECONNREFUSED, which puts the endpoint into regular (5 s+) backoff.
        const std::string target =
            "127.0.0.1:" + std::to_string(geocoin::test::find_free_port());

        geocoin::node::Node node(loadnet_autoconnect_config(dir));
        const std::filesystem::path addresses_dir =
            std::filesystem::path(node.get_config().data_dir) / "addresses";
        std::filesystem::create_directories(addresses_dir);
        const std::filesystem::path peers_file = addresses_dir / "peers.dat";
        {
            // Seed the address book only; the target is deliberately not a
            // configured seed so it is reached through AddrMan::select().
            std::ofstream out(peers_file, std::ios::trunc);
            out << target << " 0 0 0\n";
        }

        expect(node.start(geocoin::test::make_key(0x5a)), "auto-connect node starts");
        expect(wait_for_completed_attempt(node, target, 15s),
               "connection loop attempts the AddrMan candidate once");
        {
            const auto attempt = find_attempt(node, target);
            expect(attempt.has_value(), "attempt diagnostics are recorded");
            expect(!attempt->manual, "AddrMan candidate is not a manual target");
            expect(attempt->fail_count == 1, "first real attempt fails exactly once");
            expect(attempt->in_backoff, "failed candidate enters outbound backoff");
        }

        // Let the loop run through more cycles (2 s cadence while below the
        // outbound target) while the candidate is still in backoff.
        std::this_thread::sleep_for(3500ms);

        {
            const auto attempt = find_attempt(node, target);
            expect(attempt.has_value(), "attempt diagnostics persist across cycles");
            expect(attempt->fail_count == 1,
                   "backoff is honoured: no second real attempt inside the backoff window");
        }
        expect(node.is_running(), "connection loop keeps the node running");

        node.stop();
        expect(!node.is_running(), "auto-connect node stops cleanly");
        expect(std::filesystem::exists(peers_file), "address book is persisted on shutdown");

        const auto persisted = read_peers_file(peers_file);
        const auto it = persisted.find(target);
        expect(it != persisted.end(), "failed candidate stays in the address book");
        expect(it->second.attempts == 1,
               "a backed-off candidate is charged exactly one failed attempt, not one per spin");
        expect(it->second.last_try != 0, "the real attempt updates last_try");
        for (const auto& [address, info] : persisted) {
            expect(info.attempts <= 1,
                   ("no address is marked terrible by backoff skips: " + address).c_str());
        }

        std::filesystem::remove_all(dir, ec);
    } catch (...) {
        std::filesystem::remove_all(dir, ec);
        throw;
    }
}

} // namespace

int main()
{
    try {
        test_backoff_skip_is_not_charged_as_addrman_failure();
        std::cout << "test_node_connection_backoff: ok\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "test_node_connection_backoff failed: " << e.what() << "\n";
        return 1;
    }
}
