#pragma once

#include "geocoin/types.hpp"
#include "geocoin/core/block.hpp"
#include "geocoin/core/transaction.hpp"
#include "geocoin/state/state.hpp"
#include "geocoin/nft/nft_types.hpp"
#include "geocoin/wallet/multisig.hpp"  // Для MultisigConfig
#include "geocoin/utils/observer.hpp"
#include <map>
#include <span>
#include <vector>
#include <unordered_map>
#include <memory>
#include <mutex>

namespace geocoin::wallet {

enum class WalletScanStatus { COMPLETE, READ_ERROR, HISTORY_PRUNED, CHAIN_CHANGED, NO_ADDRESSES };

struct WalletScanResult {
    WalletScanStatus status = WalletScanStatus::COMPLETE;
    Height target_height = 0;
    Height next_height = 0; // First height that still needs to be scanned.
    size_t peak_owned_outputs = 0;
    std::string error;
    bool complete() const noexcept { return status == WalletScanStatus::COMPLETE; }
};

struct Hash256Hasher {
    size_t operator()(const Hash256& h) const noexcept {
        // FNV-1a по 32 байтам
        size_t v = 1469598103934665603ull;
        for (byte b : h) {
            v ^= static_cast<size_t>(b);
            v *= 1099511628211ull;
        }
        return v;
    }
};

/**
 * @brief Информация о ключе
 */
struct KeyInfo {
    secure_bytes private_key;       // Приватный ключ (32 байта, secure allocator)
    Address address;                // Адрес кошелька (GEO...)
    std::string label;              // Метка (опционально)
    uint64_t created_at;            // Время создания (Unix timestamp)
    bool used;                      // Использован ли адрес
};

/**
 * @brief Информация о транзакции в кошельке
 *
 * Как в Bitcoin CWalletTx - кэширует дебет/кредит для производительности
 */
struct WalletTransaction {
    core::Transaction tx;
    Hash256 tx_hash;
    Height block_height;            // 0 если в mempool
    uint64_t timestamp;
    int64_t amount;                 // Положительное = получено, отрицательное = отправлено
    uint64_t fee;
    bool is_coinbase;
    std::string memo;               // Локальная заметка кошелька, не часть транзакции/txid.

    // Кэш (как в Bitcoin CWalletTx) - только в памяти, не сохраняется на диск
    mutable bool debit_cached;
    mutable bool credit_cached;
    mutable int64_t debit_cache;
    mutable int64_t credit_cache;

    WalletTransaction()
        : block_height(0)
        , timestamp(0)
        , amount(0)
        , fee(0)
        , is_coinbase(false)
        , debit_cached(false)
        , credit_cached(false)
        , debit_cache(0)
        , credit_cache(0)
    {}

    // Пометить как измененный - сброс кэша (как в Bitcoin)
    void mark_dirty() {
        debit_cached = false;
        credit_cached = false;
        debit_cache = 0;
        credit_cache = 0;
    }
};

/**
 * @brief Простой кошелек GeoCoin (без HD деривации)
 *
 * Управляет приватными ключами и создает транзакции.
 * Каждый ключ независим (не детерминированный).
 * Подходит для Proof-of-Equity где один адрес = один акционер.
 */
class Wallet {
public:
    Wallet();
    ~Wallet();

    /**
     * @brief Инициализировать новый кошелек с приватным ключом
     *
     * @param private_key Приватный ключ (32 байта)
     * @return true при успехе
     */
    bool initialize(const bytes& private_key);

    /**
     * @brief Создать новый случайный ключ
     *
     * @param label Метка для адреса
     * @return Адрес
     */
    Address create_new_key(const std::string& label = "");

    /**
     * @brief Импортировать приватный ключ
     *
     * @param private_key Приватный ключ (32 байта)
     * @param label Метка
     * @return true при успехе
     */
    bool import_private_key(const bytes& private_key, const std::string& label = "");

    /**
     * @brief Получить все адреса кошелька
     *
     * @return Список всех адресов
     */
    std::vector<Address> get_all_addresses() const;

    /**
     * @brief Получить адреса, которыми кошелек может распоряжаться (есть приватные ключи)
     */
    std::vector<Address> get_spendable_addresses() const;

    /**
     * @brief Получить watch-only адреса (например, мультисиг GEOM...), без приватных ключей
     */
    std::vector<Address> get_watch_only_addresses() const;

    /**
     * @brief Получить информацию о ключе
     *
     * @param address Адрес
     * @param info [out] Информация о ключе
     * @return true если найден
     */
    bool get_key_info(const Address& address, KeyInfo& info) const;

    /**
     * @brief Получить приватный ключ
     *
     * @param address Адрес
     * @param private_key [out] Приватный ключ
     * @return true если найден
     */
    bool get_private_key(const Address& address, bytes& private_key) const;

    /**
     * @brief Проверить, есть ли ключ
     *
     * @param address Адрес
     * @return true если есть
     */
    bool has_key(const Address& address) const;

    /**
     * @brief Получить баланс адреса
     *
     * @param address Адрес
     * @param chain_state ChainState для проверки UTXO
     * @return Баланс в angles
     */
    Amount get_balance(const Address& address, std::shared_ptr<state::ChainState> chain_state) const;

    /**
     * @brief Получить общий баланс всех адресов
     *
     * @param chain_state ChainState для проверки UTXO
     * @return Баланс в angles
     */
    Amount get_total_balance(std::shared_ptr<state::ChainState> chain_state) const;

    /**
     * @brief Оценить требуемую комиссию для суммы
     */
    Amount estimate_required_fee(
        Amount amount,
        std::shared_ptr<state::ChainState> chain_state,
        std::span<const core::Transaction> pending = {}
    ) const;

    /**
     * @brief Создать и подписать транзакцию
     *
     * @param to_address Адрес получателя
     * @param amount Сумма в angles
     * @param fee Комиссия в angles
     * @param chain_state ChainState для выбора UTXO
     * @param tx [out] Созданная транзакция
     * @return true при успехе
     */
    bool create_transaction(
        const Address& to_address,
        Amount amount,
        Amount fee,
        std::shared_ptr<state::ChainState> chain_state,
        core::Transaction& tx,
        std::span<const core::Transaction> pending = {}
    );

    /**
     * @brief Создать транзакцию с указанием адреса для сдачи
     *
     * Позволяет отправить сдачу на другой адрес кошелька.
     * Полезно для "внутреннего перевода" между адресами одного кошелька.
     *
     * @param to_address Адрес получателя
     * @param amount Сумма в angles
     * @param fee Комиссия в angles
     * @param change_address Адрес для сдачи (если пустой, используется адрес источника)
     * @param chain_state ChainState для выбора UTXO
     * @param tx [out] Созданная транзакция
     * @return true при успехе
     */
    bool create_transaction_with_change(
        const Address& to_address,
        Amount amount,
        Amount fee,
        const Address& change_address,
        std::shared_ptr<state::ChainState> chain_state,
        core::Transaction& tx,
        std::span<const core::Transaction> pending = {}
    );

    /**
     * @brief Переместить все монеты с одного адреса на другой (внутри кошелька)
     *
     * Полезно для:
     * - Ротации ключей (перевод на новый адрес)
     * - Консолидации UTXO
     * - Миграции после компрометации ключа
     *
     * @param from_address Адрес источника (должен быть в кошельке)
     * @param to_address Адрес назначения (может быть любой)
     * @param fee Комиссия в angles
     * @param chain_state ChainState для выбора UTXO
     * @param tx [out] Созданная транзакция
     * @return true при успехе
     */
    bool move_funds(
        const Address& from_address,
        const Address& to_address,
        Amount fee,
        std::shared_ptr<state::ChainState> chain_state,
        core::Transaction& tx,
        std::span<const core::Transaction> pending = {}
    );

    bool create_nft_create(
        const nft::NFTMetadata& metadata,
        uint32_t royalty_percent,
        Amount fee,
        std::shared_ptr<state::ChainState> chain_state,
        core::Transaction& tx,
        std::span<const core::Transaction> pending = {}
    );

    bool create_nft_transfer(
        const Hash256& nft_id,
        const Address& to,
        Amount fee,
        std::shared_ptr<state::ChainState> chain_state,
        core::Transaction& tx,
        std::span<const core::Transaction> pending = {}
    );

    bool create_nft_burn(
        const Hash256& nft_id,
        Amount fee,
        std::shared_ptr<state::ChainState> chain_state,
        core::Transaction& tx,
        std::span<const core::Transaction> pending = {}
    );

    /**
     * @brief Добавить транзакцию в историю
     *
     * @param wallet_tx Транзакция кошелька
     */
    void add_transaction(const WalletTransaction& wallet_tx);

    /**
     * @brief Получить историю транзакций
     *
     * @param address Адрес (пустой = все адреса)
     * @param limit Максимум транзакций (0 = все после offset)
     * @param offset Пропустить столько подходящих транзакций
     * @return Список транзакций
     */
    std::vector<WalletTransaction> get_transactions(
        const Address& address = "",
        size_t limit = 0,
        size_t offset = 0
    ) const;

    /**
     * @brief Сохранить кошелек в файл
     *
     * @param filename Имя файла
     * @param password Пароль для шифрования AES-256
     * @return true при успехе
     */
    bool save_to_file(
        const std::string& filename,
        const std::string& password
    ) const;

    bool save_to_file(
        const std::string& filename,
        const std::string& password,
        bool preserve_backup
    ) const;

    /** Atomically authenticate the old password and persist a new verifier/ciphertext. */
    bool change_password(
        const std::string& filename,
        const std::string& old_password,
        const std::string& new_password
    );

    /**
     * @brief Загрузить кошелек из файла
     *
     * @param filename Имя файла
     * @param password Пароль для расшифровки
     * @return true при успехе
     */
    bool load_from_file(const std::string& filename, const std::string& password);

    /**
     * @brief Получить основной адрес кошелька
     *
     * @return Основной адрес
     */
    Address get_address() const;

    /**
     * @brief Экспортировать приватный ключ основного адреса
     *
     * @param password Пароль для проверки
     * @return Приватный ключ (32 байта)
     */
    bytes export_private_key(const std::string& password) const;

    /**
     * @brief Запомнить пароль (сохраняется только хеш)
     */
    void remember_password(const std::string& password);

    /**
     * @brief Проверить пароль без сохранения приватных данных
     */
    bool verify_password(const std::string& password) const;

    /**
     * @brief Проверить, зашифрован ли кошелек паролем
     */
    bool has_password_protection() const;

    /**
     * @brief Очистить кэш пароля
     */
    void clear_password_cache();

    /**
     * @brief Очистить кошелек
     */
    void clear();

    /**
     * @brief Получить количество ключей
     *
     * @return Количество
     */
    size_t get_key_count() const;

    /**
     * @brief Сканировать блокчейн и добавить транзакции кошелька
     *
     * @param chain_state ChainState для сканирования
     * @param from_height Начальная высота (0 = с начала)
     */
    WalletScanResult scan_blockchain(std::shared_ptr<state::ChainState> chain_state, Height from_height = 0);

    /**
     * Reconcile confirmed wallet transactions against the active chain after
     * an observer-stream gap. Transactions at or above from_height are first
     * demoted to unconfirmed and then rediscovered from authoritative blocks.
     */
    WalletScanResult reconcile_blockchain(
        std::shared_ptr<state::ChainState> chain_state,
        Height from_height = 0
    );

    /**
     * @brief Получить последнюю отсканированную высоту (как Bitcoin's GetBestBlock)
     *
     * @return Высота последнего обработанного блока
     */
    Height get_last_scanned_height() const;

    /** Hash of the active block that anchors get_last_scanned_height(). */
    Hash256 get_last_scanned_block_hash() const;

    WalletScanResult get_last_scan_result() const;

    /**
     * @brief Обработать транзакцию из блока (callback от ноды)
     *
     * @param tx Транзакция
     * @param block_height Высота блока
     * @param timestamp Время блока
     * @param chain_state ChainState для проверки UTXO
     */
    void sync_transaction(const core::Transaction& tx, Height block_height, uint64_t timestamp, std::shared_ptr<state::ChainState> chain_state);

    /** Mark wallet transactions from a disconnected active block unconfirmed. */
    void sync_disconnected_block(
        const core::Block& block,
        Height disconnected_height,
        Height new_tip_height,
        const Hash256& new_tip_hash = {}
    );

    /**
     * @brief Получить путь к файлу кошелька по умолчанию
     *
     * @return Путь к файлу
     */
    static std::string get_default_wallet_path();
    std::string get_file_path() const;
    bool save(const std::string& password) const;
    enum class SaveStatus { FAILED, COMMITTED_NOT_DURABLE, DURABLE };
    SaveStatus last_save_status() const;

    /**
     * @brief Установить путь к файлу кошелька по умолчанию
     */
    static void set_default_wallet_path(const std::string& path);

    /**
     * @brief Добавить мультиподпись адрес для отслеживания
     *
     * @param config Конфигурация мультиподписи
     * @return true при успехе
     */
    bool add_multisig_address(const wallet::MultisigConfig& config);

    /**
     * @brief Получить все мультиподпись адреса
     *
     * @return Список конфигураций
     */
    std::vector<wallet::MultisigConfig> get_multisig_addresses() const;

    /**
     * @brief Удалить мультиподпись адрес
     *
     * @param address Мультиподпись адрес
     * @return true при успехе
     */
    bool remove_multisig_address(const std::string& address);

    /**
     * @brief Вычислить debit для транзакции (сколько потратили) - как Bitcoin GetDebit
     *
     * @param wtx Транзакция кошелька
     * @param chain_state ChainState для поиска предыдущих транзакций
     * @return Сумма потраченных средств
     */
    int64_t get_debit(WalletTransaction& wtx, std::shared_ptr<state::ChainState> chain_state) const;

    /**
     * @brief Вычислить credit для транзакции (сколько получили) - как Bitcoin GetCredit
     *
     * @param wtx Транзакция кошелька
     * @return Сумма полученных средств
     */
    int64_t get_credit(const WalletTransaction& wtx) const;

    /** Сигналы для уведомления интерфейса (как в Биткоине) */

    /** Транзакция изменилась (добавлена/обновлена) - вызывается при удержании мьютекса */
    ObserverList<Wallet*, const Hash256&> NotifyTransactionChanged;

    /** Статус изменен (зашифрован/заблокирован) */
    ObserverList<Wallet*> NotifyStatusChanged;

    ObserverList<Wallet*, const WalletScanResult&> NotifyScanFinished;
    // Emitted after a batch is committed, outside mutex_, for progress observers.
    ObserverList<Wallet*, Height> NotifyScanProgress;

private:
    mutable std::mutex mutex_;
    mutable std::string file_path_;
    mutable SaveStatus last_save_status_{SaveStatus::FAILED};
    // Serialize history scans and chain events without holding mutex_ during disk reads.
    std::recursive_mutex scan_mutex_;
    WalletScanResult last_scan_result_;

    // Ключи: address -> KeyInfo
    std::map<Address, KeyInfo> keys_;
    // Адрес createwallet / initialize; не меняется при import/getnewaddress.
    Address primary_address_;

    // История транзакций
    std::vector<WalletTransaction> transactions_;
    // Индекс: tx_hash -> индекс в transactions_
    std::unordered_map<Hash256, size_t, Hash256Hasher> tx_index_;

    // Мультиподпись адреса для отслеживания: address -> config
    std::map<std::string, wallet::MultisigConfig> multisig_addresses_;

    // Последняя отсканированная высота блокчейна
    Height last_scanned_height_ = 0;
    Hash256 last_scanned_block_hash_{};

    // Верификатор пароля (PBKDF2): хранится отдельно от шифрования файла.
    // password_hash_ содержит 32-байтный PBKDF2-derived verifier.
    bool has_password_hash_ = false;
    Hash256 password_hash_ = {};
    bytes password_salt_;
    uint32_t password_kdf_iterations_ = 0;

    /**
     * @brief Выбрать UTXO для транзакции
     */
    std::vector<state::UTXO> select_utxos(
        Amount amount,
        Amount fee,
        std::shared_ptr<state::ChainState> chain_state,
        std::map<Address, secure_bytes>* used_keys = nullptr,
        std::span<const core::Transaction> pending = {}
    ) const;

    std::vector<state::UTXO> select_utxos_for_address(
        const Address& address,
        Amount amount,
        Amount fee,
        std::shared_ptr<state::ChainState> chain_state,
        bytes* private_key = nullptr,
        std::span<const core::Transaction> pending = {}
    ) const;

    /**
     * @brief Подписать транзакцию
     */
    bool sign_transaction(
        core::Transaction& tx,
        const std::map<Address, secure_bytes>& keys
    );

    /**
     * @brief Отметить адрес как использованный
     */
    void mark_address_used(const Address& address);

    void rebuild_tx_index_locked();
    int64_t get_debit_locked(WalletTransaction& wtx, std::shared_ptr<state::ChainState> chain_state) const;
    int64_t get_credit_locked(const WalletTransaction& wtx) const;

    bool save_to_file_locked(
        const std::string& filename,
        const std::string& password,
        bool preserve_backup,
        bool replace_password_verifier
    ) const;
};

} // namespace geocoin::wallet
