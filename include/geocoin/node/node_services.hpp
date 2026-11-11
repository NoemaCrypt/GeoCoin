#pragma once

#include "geocoin/node/node_types.hpp"
#include "geocoin/services/chain_mutation.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace geocoin::node {

/** Read-only chain access used by frontends and adapters. */
class ChainQueryService {
public:
    virtual ~ChainQueryService() = default;

    virtual Amount get_balance(const Address& address) const = 0;
    virtual bool get_block(const Hash256& block_hash, core::Block& block) const = 0;
    virtual bool get_block_by_height(Height height, core::Block& block) const = 0;
    virtual bool get_transaction(const Hash256& tx_hash, core::Transaction& tx) const = 0;
    virtual std::vector<core::Transaction> get_mempool_transactions() const = 0;
};

/** Runtime status and pruning information without exposing mutable components. */
class NodeInfoService {
public:
    virtual ~NodeInfoService() = default;

    virtual bool is_running() const = 0;
    virtual NodeStatus get_status() const = 0;
    virtual NodeConfig get_config() const = 0;
    virtual bool is_initial_block_download() const = 0;
    virtual bool is_pruned_mode() const = 0;
    virtual Height get_pruned_block_retention() const = 0;
    virtual PrunedDataInfo get_pruned_data_info() const = 0;
    virtual bool is_height_pruned_for_api(Height height) const = 0;
    virtual bool is_block_pruned_for_api(
        const Hash256& block_hash,
        Height* out_height = nullptr
    ) const = 0;
    virtual std::string describe_pruned_api_error(Height requested_height) const = 0;
};

class MiningControlService {
public:
    virtual ~MiningControlService() = default;

    virtual bool start_mining() = 0;
    virtual bool start_mining(size_t threads, const Address& mining_address) = 0;
    virtual void stop_mining() = 0;
    virtual bool is_mining() const = 0;
};

class NetworkControlService {
public:
    virtual ~NetworkControlService() = default;

    virtual bool connect_to_peer(const std::string& address) = 0;
    virtual bool request_witness(const Hash256& block_hash) = 0;
};

class NodeEventService {
public:
    using TransactionCallback =
        std::function<void(const core::Transaction&, Height, uint64_t)>;
    using ChainMutationEventCallback =
        std::function<void(const services::ChainMutationEvent&)>;

    virtual ~NodeEventService() = default;

    virtual void set_transaction_callback(TransactionCallback callback) = 0;
    virtual void clear_transaction_callback() = 0;
    virtual void set_chain_mutation_event_callback(ChainMutationEventCallback callback) = 0;
    virtual void clear_chain_mutation_event_callback() = 0;
};

/** Owning, typed service view over one node runtime. */
struct NodeServiceHandle {
    std::shared_ptr<ChainQueryService> chain_query;
    std::shared_ptr<NodeInfoService> info;
    std::shared_ptr<MiningControlService> mining;
    std::shared_ptr<NetworkControlService> network;
    std::shared_ptr<NodeEventService> events;
    std::shared_ptr<services::ChainMutationService> mutations;

    bool complete() const noexcept {
        return chain_query && info && mining && network && events && mutations;
    }
};

} // namespace geocoin::node
