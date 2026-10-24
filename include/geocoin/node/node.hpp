#pragma once

#include "geocoin/node/node_components.hpp"
#include "geocoin/node/node_services.hpp"
#include "geocoin/core/blockchain.hpp"
#include "geocoin/state/state.hpp"
#include "geocoin/storage/block_storage.hpp"
#include "geocoin/network/peer_manager.hpp"
#include "geocoin/network/p2p_server.hpp"
#include "geocoin/network/mempool.hpp"
#include "geocoin/network/sync_manager.hpp"
#include "geocoin/consensus/proof_of_work.hpp"
#include "geocoin/services/chain_mutation.hpp"
#include "geocoin/utils/activation_manifest.hpp"
#include <chrono>
#include <deque>
#include <memory>
#include <atomic>
#include <mutex>
#include <functional>
#include <vector>
#include <map>
#include <set>
#include <string>
#include <thread>

namespace geocoin::network {
class AddrMan;
}

namespace geocoin::node {

class ChainCoordinator;
class ChainEventDispatcher;

/**
 * @brief Полная нода GeoCoin (degree)
 *
 * Интегрирует все компоненты:
 * - ChainState (UTXO, блоки)
 * - PeerManager (P2P соединения)
 * - P2PServer (входящие соединения)
 * - Mempool (неподтвержденные транзакции)
 * - SyncManager (синхронизация блоков)
 * - Miner (майнинг блоков)
 */
class Node : public std::enable_shared_from_this<Node>,
             public services::ChainMutationService,
             public ChainQueryService,
             public NodeInfoService,
             public MiningControlService,
             public NetworkControlService,
             public NodeEventService {
#ifdef GEOCOIN_ENABLE_NODE_TEST_ACCESS
    friend struct NodeTestAccess;
#endif

public:
    Node(const NodeConfig& config);
    ~Node() override;

    /**
     * @brief Запустить ноду
     *
     * Экземпляр Node одноразовый: после успешного stop() нужно создать новый
     * объект, чтобы заново открыть storage/runtime компоненты безопасно.
     *
     * @param private_key Приватный ключ ноды для p2p-аутентификации
     * @return true при успехе
     */
    bool start(const bytes& private_key);

    /**
     * Start the node and retain a machine-readable failure category.
     * Frontends must use this overload when deciding how to report or recover
     * from startup failures. No category authorizes automatic chain deletion.
     */
    [[nodiscard]] NodeStartResult start_with_result(const bytes& private_key);

    /**
     * @brief Остановить ноду
     */
    void stop();

    /**
     * @brief Проверить, запущена ли нода
     *
     * @return true если запущена
     */
    bool is_running() const override { return running_; }

    /**
     * @brief Подключиться к пиру
     *
     * @param address Адрес пира (IP:port)
     * @return true при успехе
     */
    bool connect_to_peer(const std::string& address) override;

    /**
     * @brief Отправить транзакцию в сеть
     *
     * @param tx Транзакция
     * @return true если принята в mempool
     */
    bool submit_transaction(const core::Transaction& tx);

    /** Ordered mutation port used by RPC, P2P, sync and mining. */
    services::BlockSubmissionResult process_block(const core::Block& block) override;
    services::ChainTipActivationResult activate_tip(const Hash256& tip_hash) override;
    services::TransactionSubmissionResult admit_transaction(const core::Transaction& tx) override;

    using TransactionCallback = NodeEventService::TransactionCallback;
    void set_transaction_callback(TransactionCallback cb) override;
    void clear_transaction_callback() override;

    using ChainMutationEventCallback = NodeEventService::ChainMutationEventCallback;
    void set_chain_mutation_event_callback(ChainMutationEventCallback cb) override;
    void clear_chain_mutation_event_callback() override;

    /**
     * @brief Получить статус ноды
     *
     * @return Статус
     */
    NodeStatus get_status() const override;

    /**
     * @brief Получить снимок конфигурации, безопасный для чтения вызывающей стороной.
     */
    NodeConfig get_config() const override;

    /**
     * @brief Получить баланс адреса
     *
     * @param address Адрес
     * @return Баланс в angles
     */
    Amount get_balance(const Address& address) const override;

    /**
     * @brief Получить блок по хешу
     *
     * @param block_hash Хеш блока
     * @param block [out] Блок
     * @return true если найден
     */
    bool get_block(const Hash256& block_hash, core::Block& block) const override;

    /**
     * @brief Получить блок по высоте
     *
     * @param height Высота
     * @param block [out] Блок
     * @return true если найден
     */
    bool get_block_by_height(Height height, core::Block& block) const override;

    /**
     * @brief Получить транзакцию
     *
     * @param tx_hash Хеш транзакции
     * @param tx [out] Транзакция
     * @return true если найдена (в блоке или mempool)
     */
    bool get_transaction(const Hash256& tx_hash, core::Transaction& tx) const override;
    std::vector<core::Transaction> get_mempool_transactions() const override;

    bool is_pruned_mode() const override { return config_.node_type == NodeType::PRUNED; }
    Height get_pruned_block_retention() const override { return config_.pruned_block_retention; }
    PrunedDataInfo get_pruned_data_info() const override;
    bool is_height_pruned_for_api(Height height) const override;
    bool is_block_pruned_for_api(
        const Hash256& block_hash,
        Height* out_height = nullptr
    ) const override;
    std::string describe_pruned_api_error(Height requested_height) const override;

    /**
     * @brief Запросить witness-полезную нагрузку для блока у пиров.
     *
     * Реализация использует GETDATA(BLOCK): архивные пиры возвращают полный блок,
     * после чего локальная копия блока обновляется с восстановленным witness.
     */
    bool request_witness(const Hash256& block_hash) override;

    /**
     * @brief Начать майнинг
     *
     * @return true при успехе
     */
    bool start_mining() override;

    /**
     * @brief Начать майнинг с указанными параметрами
     *
     * @param threads Количество потоков
     * @param mining_address Адрес для награды
     * @return true при успехе
     */
    bool start_mining(size_t threads, const Address& mining_address) override;

    /**
     * @brief Остановить майнинг
     */
    void stop_mining() override;

    /**
     * @brief Проверить, идет ли майнинг
     *
     * @return true если майнинг активен
     */
    bool is_mining() const override { return mining_; }

    /**
     * @brief Проверить, идет ли начальная загрузка блокчейна (IBD)
     *
     * Как в Bitcoin: IBD если высота < best peer height - 144 блока
     * ИЛИ последний блок старше 24 часов
     *
     * @return true если идет IBD
     */
    bool is_initial_block_download() const override;

    /** Owning narrow service view for adapters and asynchronous consumers. */
    NodeServiceHandle service_handle();

    /** Coherent owning bundle for GUI/RPC composition roots. */
    NodeComponentHandle component_handle();

    /**
     * @brief Получить компоненты (для тестирования)
     */
    std::shared_ptr<state::ChainState> get_chain_state() const { return chain_state_; }
    std::shared_ptr<network::PeerManager> get_peer_manager() const { return peer_manager_; }
    std::shared_ptr<network::Mempool> get_mempool() const { return mempool_; }
    std::shared_ptr<network::SyncManager> get_sync_manager() const { return sync_manager_; }
    std::shared_ptr<core::Blockchain> get_blockchain() const { return blockchain_; }
private:
    enum class LifecycleState {
        CREATED,
        STARTING,
        RUNNING,
        STOPPING,
        STOPPED
    };

    enum class BlockchainStartupStatus {
        READY,
        INITIALIZATION_FAILED,
        EMPTY_CHAIN,
    };

    NodeConfig config_;
    bytes private_key_;
    std::atomic<bool> running_;
    std::atomic<bool> mining_;

    // Компоненты
    std::shared_ptr<storage::BlockStorage> block_storage_;
    std::shared_ptr<state::ChainState> chain_state_;
    std::shared_ptr<core::Blockchain> blockchain_;  // Главный контроллер
    std::shared_ptr<network::PeerManager> peer_manager_;
    std::shared_ptr<network::P2PServer> p2p_server_;
    std::shared_ptr<network::Mempool> mempool_;
    std::shared_ptr<network::SyncManager> sync_manager_;
    std::unique_ptr<ChainEventDispatcher> chain_event_dispatcher_;
    std::unique_ptr<ChainCoordinator> chain_coordinator_;

    // Потоки
    std::thread update_thread_;
    std::vector<std::thread> mining_threads_;
    // Serialize complete stop operations without holding the lifecycle mutex
    // while joining workers: event consumers may query Node services there.
    std::mutex stop_control_mutex_;
    mutable std::mutex lifecycle_control_mutex_;
    LifecycleState lifecycle_state_{LifecycleState::CREATED};
    mutable std::mutex mining_control_mutex_;

    // Статистика майнинга
    mutable std::atomic<uint64_t> total_hashes_;
    mutable std::atomic<uint64_t> mined_blocks_accepted_;
    mutable uint64_t last_reported_hashes_;
    mutable double current_hashrate_;
    std::chrono::steady_clock::time_point mining_start_time_;
    mutable std::chrono::steady_clock::time_point mining_last_hashrate_time_;
    mutable std::mutex mining_stats_mutex_;
    std::mutex mining_output_mutex_;  // Для синхронизации вывода
    mutable std::mutex witness_request_mutex_;
    std::map<std::string, std::chrono::steady_clock::time_point>
        pending_witness_requests_;
    struct PendingTransactionInventory {
        std::weak_ptr<network::Peer> peer;
        std::set<Hash256> hashes;
        std::deque<Hash256> order;
        std::map<Hash256, std::chrono::steady_clock::time_point> requested;
        std::chrono::steady_clock::time_point next_request{};
    };
    std::mutex transaction_request_mutex_;
    std::map<std::string, PendingTransactionInventory> pending_transaction_inventory_;
    void queue_transaction_requests(std::shared_ptr<network::Peer> peer,
                                    const std::vector<Hash256>& hashes);
    void request_pending_transactions();

    mutable std::mutex tx_callback_mutex_;
    TransactionCallback tx_callback_;
    uint64_t tx_callback_min_sequence_{1};
    mutable std::mutex chain_event_callback_mutex_;
    ChainMutationEventCallback chain_event_callback_;
    uint64_t chain_event_callback_min_sequence_{1};

    // Автоматическое подключение к пирам
    std::thread connection_thread_;
    std::atomic<bool> connection_thread_running_;
    mutable std::mutex relay_mutex_;
    Hash256 pending_block_inventory_{};
    std::shared_ptr<network::AddrMan> addrman_;
    mutable std::mutex addrman_mutex_;
    std::set<std::string> outbound_reconnect_targets_;
    mutable std::mutex outbound_reconnect_targets_mutex_;

    /**
     * @brief Получить список seed nodes по умолчанию
     */
    std::vector<std::string> get_default_seed_nodes() const;

    /**
     * @brief Главный цикл обновления
     */
    void update_loop();

    /**
     * @brief Поток автоматического подключения к пирам
     */
    void connection_loop();

    /**
     * @brief Announce the latest committed tip and queued transactions in one INV batch.
     */
    void relay_inventory(std::shared_ptr<network::Peer> exclude_peer = nullptr);

    void handle_chain_mutation_event(const services::ChainMutationEvent& event);
    void handle_active_chain_transition(const services::ChainTransition& transition);

    /**
     * @brief Удалить тела блоков за пределами окна PrunedNode.
     */
    void prune_old_block_bodies_if_needed();

    /**
     * @brief Обработать новую транзакцию (от пира или локально)
     * @param tx Транзакция
     * @param source_peer Источник (nullptr если локальная)
     */
    void handle_new_transaction(const core::Transaction& tx, std::shared_ptr<network::Peer> source_peer = nullptr);
    services::TransactionSubmissionResult admit_transaction_from_(
        const core::Transaction& tx,
        std::shared_ptr<network::Peer> source_peer
    );

    /**
     * @brief Обработать сообщение от пира
     */
    void handle_peer_message(
        std::shared_ptr<network::Peer> peer,
        network::MessageType type,
        const bytes& payload
    );

    /**
     * @brief Обработать событие аутентификации пира
     */
    void handle_peer_authenticated(
        std::shared_ptr<network::Peer> peer,
        Height peer_height
    );

    Height get_active_height() const;
    Height get_best_peer_height(Height floor_height) const;

    void notify_transaction_callback(
        const core::Transaction& tx,
        Height height,
        uint64_t timestamp,
        uint64_t event_sequence
    ) const;

    void select_network_and_resolve_data_dir();
    storage::StorageOptions load_storage_options_from_config();
    void open_storage_components(const storage::StorageOptions& storage_options);
    void create_runtime_components();
    void initialize_metrics();

    bool apply_startup_config(std::string& error);
    void load_activation_manifest();
    BlockchainStartupStatus initialize_blockchain_for_start();
    void configure_peer_callbacks();
    bool start_p2p_listener();
    bool start_background_threads();
    void stop_background_threads();
    void clear_runtime_callbacks();
    void shutdown_runtime_components();
    void disconnect_all_peers();

    /**
     * @brief Поток майнинга
     */
    bool start_mining_unlocked();
    void mining_thread(int thread_id);

    /**
     * @brief Создать блок для майнинга
     */
    core::Block create_mining_block();
    core::Block create_mining_block_for_tip(const core::ActiveTipSnapshot& tip_snapshot);

    /**
     * @brief Вычислить сложность для следующего блока
     *
     * Использует консенсусный per-block DAA:
     * - Сложность пересчитывается на каждый блок
     * - Основан на скользящем окне median-time-past
     * - Целевое время блока: 10 минут (600 секунд)
     *
     * @return Новый bits для следующего блока
     */
    uint32_t calculate_next_difficulty(Timestamp block_timestamp);
};

} // namespace geocoin::node
