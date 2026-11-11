#pragma once

#include "geocoin/types.hpp"
#include "geocoin/core/block.hpp"
#include "geocoin/state/state.hpp"
#include "geocoin/network/peer.hpp"
#include "geocoin/services/chain_mutation.hpp"
#include <map>
#include <set>
#include <mutex>
#include <memory>
#include <functional>
#include <chrono>
#include <deque>
#include <string>
#include <vector>
#include <unordered_set>
#include <unordered_map>
#include <atomic>
#include <cstddef>

namespace geocoin::core {
    class Blockchain;
}

namespace geocoin::network {

struct Hash256Hasher {
    std::size_t operator()(const Hash256& hash) const noexcept {
        std::size_t value = 0xcbf29ce484222325ULL;
        for (byte b : hash) {
            value ^= static_cast<std::size_t>(b);
            value *= 0x100000001b3ULL;
        }
        return value;
    }
};

struct HeaderEntry {
    Height height{0};
    Hash256 prev_hash{};
    uint32_t timestamp{0};
    uint256 cumulative_work{};
    core::BlockHeader header{};
};

/**
 * @brief Статус синхронизации
 */
enum class SyncStatus {
    NOT_STARTED,        // Не начата
    SYNCING,            // В процессе
    COMPLETED,          // Завершена
    FAILED              // Провалена
};

/**
 * @brief Информация о синхронизации с пиром
 */
struct PeerSyncInfo {
    std::shared_ptr<Peer> peer;
    Height height{0};                           // Highest header height actually validated from this peer
    Height advertised_height{0};                // Untrusted VERSION/INV hint; diagnostics only
    bool headers_exhausted{false};               // Peer already disproved its current advertised lead
    bool headers_request_pending{false};
    bool headers_request_queued{false};
    uint64_t last_headers_request_ms{0};
    Hash256 last_header_hash{};                  // Validated locator for the next headers page
    std::deque<uint64_t> recent_block_requests_ms;
    std::set<Hash256> requested_blocks;         // Запрошенные блоки
    std::chrono::steady_clock::time_point last_request;
    uint32_t bad_headers_score{0};
    uint32_t timeout_score{0};
    uint32_t invalid_block_score{0};
    uint64_t completed_blocks{0};
    uint64_t timed_out_blocks{0};
    uint64_t failed_blocks{0};
    double response_time_ewma_ms{0.0};
};

/**
 * @brief Статистика синхронизации
 */
struct SyncStats {
    SyncStatus status;
    Height current_height;
    Height target_height;
    size_t blocks_downloaded;
    size_t blocks_verified;
    size_t active_peers;
    size_t download_queue_size;
    size_t downloading_blocks;
    size_t pending_blocks;
    size_t orphan_blocks;
    double progress;                            // 0.0 - 1.0
};

    /**
     * @brief Callback для нового примененного блока
     *
     * height передается явно, чтобы не читать Blockchain/ChainState из чужого потока
     * без синхронизации (иначе возможны гонки и некорректные высоты).
     */
    using BlockCallback = std::function<void(const core::Block& block, Height height)>;
    using PeerDisconnectCallback = std::function<void(std::shared_ptr<Peer> peer, const std::string& reason)>;
    using BlockSubmitter = std::function<services::BlockSubmissionResult(const core::Block& block)>;

/**
 * @brief Менеджер синхронизации блокчейна
 *
 * Управляет загрузкой и верификацией блоков от пиров:
 * - Headers-first синхронизация
 * - Параллельная загрузка от нескольких пиров
 * - Верификация цепочки блоков
 * - Initial Block Download (IBD)
 */
class SyncManager {
#ifdef GEOCOIN_ENABLE_SYNC_MANAGER_TEST_ACCESS
    friend struct SyncManagerTestAccess;
#endif
public:
    SyncManager(
        std::shared_ptr<state::ChainState> chain_state,
        std::shared_ptr<core::Blockchain> blockchain
    );
    ~SyncManager();

    /**
     * @brief Установить callback для новых блоков
     *
     * @param callback Функция обработки
     */
    void set_block_callback(BlockCallback callback);

    /** Route block mutations through the owning node's single writer. */
    void set_block_submitter(BlockSubmitter submitter);

    /**
     * @brief Установить callback для policy disconnect пиров из sync layer.
     */
    void set_peer_disconnect_callback(PeerDisconnectCallback callback);

    /** Remove callbacks supplied by the owning node before teardown. */
    void clear_callbacks();

    /**
     * @brief Начать синхронизацию с пиром
     *
     * @param peer Пир
     * @param peer_height Высота блокчейна пира
     * @return true при успехе
     */
    bool start_sync(std::shared_ptr<Peer> peer, Height peer_height);

    /**
     * @brief Остановить синхронизацию
     */
    void stop_sync();

    /**
     * @brief Обработать INV сообщение
     *
     * @param peer Пир
     * @param block_hashes Хеши блоков
     */
    void handle_inv(std::shared_ptr<Peer> peer, const std::vector<Hash256>& block_hashes);

    /**
     * @brief Обработать HEADERS сообщение от пира
     *
     * Возвращает true, если пакет заголовков принят в локальный индекс синхронизации.
     */
    bool handle_headers(
        std::shared_ptr<Peer> peer,
        const std::vector<core::BlockHeader>& headers
    );

    /**
     * @brief Обработать полученный блок
     *
     * @param peer Пир
     * @param block Блок
     * @return true если блок принят
     */
    bool handle_block(std::shared_ptr<Peer> peer, const core::Block& block);

    /**
     * @brief Обработать GETHEADERS запрос
     *
     * @param peer Пир
     * @param locator_hashes Хеши известных блоков
     * @param stop_hash Хеш последнего нужного блока
     * @return Список заголовков для отправки
     */
    std::vector<core::BlockHeader> handle_getheaders(
        std::shared_ptr<Peer> peer,
        const std::vector<Hash256>& locator_hashes,
        const Hash256& stop_hash
    );

    /**
     * @brief Обновить состояние синхронизации
     *
     * Запрашивает недостающие блоки, проверяет таймауты
     */
    void update();

    /**
     * @brief Проверить, идет ли синхронизация
     *
     * @return true если в процессе
     */
    bool is_syncing() const;

    /**
     * @brief Получить статистику
     *
     * @return Статистика синхронизации
     */
    SyncStats get_stats() const;

    /**
     * @brief Установить максимальное количество параллельных загрузок
     *
     * @param max_parallel По умолчанию 16
     */
    void set_max_parallel_downloads(size_t max_parallel);

private:
    mutable std::mutex peers_mutex_;
    mutable std::mutex queue_mutex_;
    mutable std::mutex pending_mutex_;
    mutable std::mutex callback_mutex_;
    std::shared_ptr<state::ChainState> chain_state_;
    std::shared_ptr<core::Blockchain> blockchain_;
    BlockCallback block_callback_;
    BlockSubmitter block_submitter_;
    PeerDisconnectCallback peer_disconnect_callback_;

    std::atomic<SyncStatus> status_;
    std::atomic<Height> target_height_;
    std::atomic<size_t> blocks_downloaded_;
    std::atomic<size_t> blocks_verified_;
    size_t max_parallel_downloads_;
    size_t max_blocks_per_getdata_{4};
    size_t max_blocks_per_peer_{8};
    size_t min_download_window_size_{64};
    size_t max_download_window_size_{512};
    uint64_t block_download_timeout_ms_{30000};

    // Пиры для синхронизации
    std::map<std::string, PeerSyncInfo> sync_peers_;

    struct BlockDownloadInfo {
        std::shared_ptr<Peer> peer;
        std::string peer_address;
        Height height{0};
        uint64_t requested_ms{0};
        uint32_t attempt{0};
    };

    // Блоки в процессе загрузки
    std::map<Hash256, BlockDownloadInfo> downloading_blocks_;

    // Очередь получения или повторного применения блоков, по высоте.
    std::multimap<Height, Hash256> download_queue_;
    std::unordered_set<Hash256, Hash256Hasher> download_queue_set_;
    std::unordered_map<Hash256, uint32_t, Hash256Hasher> download_attempts_;
    std::unordered_map<Hash256, std::string, Hash256Hasher> download_avoid_peer_;
    std::unordered_map<Hash256, Height, Hash256Hasher> header_heights_;
    std::unordered_map<Hash256, HeaderEntry, Hash256Hasher> headers_index_;
    // Ancestors of the single globally best header tip, indexed by height.
    // This is deliberately not "best independently at each height": such a
    // map can splice unrelated forks and omit a winning tip's parent.
    std::map<Height, Hash256> best_header_by_height_;
    Hash256 best_header_tip_hash_{};
    uint256 best_header_tip_work_{};
    Height best_header_tip_height_{0};
    std::atomic<uint64_t> next_local_block_retry_ms_{0};
    size_t max_headers_index_size_{200000};
    size_t headers_prune_keep_depth_{4096};
    size_t download_window_size_{64};

    // Header batches that do not produce a new best tip are cheap to mine where
    // difficulty is low. Each network group may add this many per day, across
    // reconnections. Guarded by peers_mutex_. Easy-diff networks use a tighter
    // default, applied in the constructor from the selected ChainParams.
    size_t max_side_headers_per_source_{1000};
    struct SideHeaderBudget {
        double tokens{0.0};
        std::chrono::steady_clock::time_point updated{};
    };
    std::unordered_map<std::string, SideHeaderBudget> side_header_budgets_;

    // Буфер блоков которые пришли не по порядку (ждут своей очереди)
    std::map<Height, std::shared_ptr<core::Block>> pending_blocks_;
    size_t max_pending_blocks_{32};
    std::atomic<uint64_t> throughput_window_start_ms_{0};
    std::atomic<size_t> throughput_window_blocks_{0};

    // Сторож: если синхронизация простаивает, переотправляем GETBLOCKS (как у Bitcoin)
    std::atomic<uint64_t> last_getblocks_request_ms_;

    /**
     * @brief Запросить блоки у пира
     */
    bool request_blocks(std::shared_ptr<Peer> peer, const std::vector<Hash256>& hashes);

    /**
     * @brief Отправить GETBLOCKS для запроса недостающих блоков
     */
    void send_getblocks(std::shared_ptr<Peer> peer);

    /**
     * @brief Queue GETHEADERS, coalescing discoveries and pacing each peer.
     */
    void send_getheaders(std::shared_ptr<Peer> peer);
    void update_headers_requests();
    PeerSyncInfo& get_sync_peer_locked(const std::shared_ptr<Peer>& peer);

    /**
     * @brief Найти лучшего пира для загрузки
     */
    std::shared_ptr<Peer> find_best_peer();
    std::shared_ptr<Peer> find_best_peer_for_block(const Hash256& block_hash);
    std::shared_ptr<Peer> find_best_peer_impl(
        const std::string& avoid_address,
        bool enforce_download_capacity,
        Height required_height = 0
    );

    /**
     * @brief Проверить таймауты запросов
     */
    void check_timeouts();
    void record_block_download_success(const BlockDownloadInfo& info, uint64_t elapsed_ms);
    void record_block_download_failure(const BlockDownloadInfo& info, bool invalid_block);
    // Retry a missing body or unapplied stored candidate on the best header path.
    void requeue_block_download(const Hash256& hash, const std::string& avoid_peer = {});
    void forget_download_retry_locked(const Hash256& hash);
    void maybe_adjust_download_window();

    /**
     * @brief Отключить peer через внешний owner, если callback настроен.
     */
    void disconnect_peer(std::shared_ptr<Peer> peer, const std::string& reason);

    /**
     * @brief Верифицировать и применить блок
     */
    services::BlockSubmissionResult verify_and_apply_block(const core::Block& block);

    void prune_headers_state_locked(Height current_height);
    void rebuild_best_header_chain_locked(
        const Hash256& tip_hash,
        Height tip_height,
        const uint256& tip_work
    );
    bool is_best_header_locked(Height height, const Hash256& hash) const;
    // A header must carry more work than the active block MAX_REORG_DEPTH
    // below the tip; deeper forks can never be activated by local policy.
    uint256 minimum_new_header_work() const;
    void apply_network_header_limits();
    bool has_side_header_budget(const std::shared_ptr<Peer>& peer, size_t header_count);
    bool charge_side_header_budget(const std::shared_ptr<Peer>& peer, size_t header_count);
    bool charge_side_header_budget_impl(
        const std::shared_ptr<Peer>& peer, size_t header_count, bool consume
    );
    void refresh_sync_target();
    // Only Blockchain's failed-index flags establish permanent invalidity.
    bool discard_failed_header_branch(const Hash256& hash);
    void activate_headers_sync(std::shared_ptr<Peer> peer, Height hinted_target);

    struct OrphanBlockEntry {
        std::shared_ptr<core::Block> block;
        std::shared_ptr<Peer> peer;
        uint64_t received_ms{0};
    };

    void store_orphan_block(const Hash256& block_hash, const core::Block& block, std::shared_ptr<Peer> peer);
    std::vector<OrphanBlockEntry> take_orphan_children(const Hash256& parent_hash);
    void drain_orphan_blocks(const Hash256& parent_hash);
    void erase_orphan_index_locked(const Hash256& block_hash);

    mutable std::mutex orphan_mutex_;
    std::unordered_map<Hash256, OrphanBlockEntry, Hash256Hasher> orphan_blocks_;
    std::unordered_multimap<Hash256, Hash256, Hash256Hasher> orphan_blocks_by_prev_;
    // Blocks are up to 4 MB on the wire and expand further as C++ objects.
    // Keep these buffers aligned with the bounded parallel-download window.
    size_t max_orphan_blocks_{16};
};

} // namespace geocoin::network
