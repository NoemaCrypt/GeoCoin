#pragma once

#include "geocoin/node/node_services.hpp"

#include <memory>

namespace geocoin {
namespace core { class Blockchain; }
namespace state { class ChainState; }
namespace network {
class Mempool;
class PeerManager;
class SyncManager;
}

namespace node {

/**
 * Owning composition-root view of one node runtime.
 *
 * GUI/RPC adapters receive one coherent bundle instead of collecting concrete
 * subsystems through several independent Node getters. Business code should
 * depend on the narrow ports in services and not on these concrete handles.
 */
struct NodeComponentHandle {
    NodeServiceHandle services{};
    std::shared_ptr<core::Blockchain> blockchain;
    std::shared_ptr<state::ChainState> chain_state;
    std::shared_ptr<network::Mempool> mempool;
    std::shared_ptr<network::PeerManager> peer_manager;
    std::shared_ptr<network::SyncManager> sync_manager;

    [[nodiscard]] bool complete() const noexcept {
        return services.complete() && blockchain && chain_state && mempool &&
            peer_manager && sync_manager;
    }

    [[nodiscard]] bool complete_for_rpc() const noexcept {
        return services.complete() && blockchain && chain_state && mempool &&
            peer_manager;
    }
};

} // namespace node
} // namespace geocoin
