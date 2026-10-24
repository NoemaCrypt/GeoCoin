#pragma once

#include "geocoin/types.hpp"
#include "geocoin/core/block.hpp"
#include <memory>

namespace geocoin::chain {

/**
 * Статус блока в индексе
 * Аналог BlockStatus из Bitcoin
 */
enum class BlockStatus : uint32_t {
    UNKNOWN = 0,

    // Блок прошел базовую проверку (PoW, merkle root, timestamp)
    VALID_HEADER = 1,

    // Все родительские блоки найдены, сложность совпадает
    VALID_TREE = 2,

    // Транзакции валидны, нет дубликатов
    VALID_TRANSACTIONS = 3,

    // UTXO проверены, нет двойной траты
    VALID_CHAIN = 4,

    // Подписи проверены
    VALID_SCRIPTS = 5,

    // Все проверки пройдены
    VALID_MASK = VALID_HEADER | VALID_TREE | VALID_TRANSACTIONS | VALID_CHAIN | VALID_SCRIPTS,

    // Блок сохранен на диск
    HAVE_DATA = 8,

    // Undo данные сохранены
    HAVE_UNDO = 16,

    HAVE_MASK = HAVE_DATA | HAVE_UNDO,

    // Блок провалил валидацию
    FAILED_VALID = 32,

    // Потомок провалившего блока
    FAILED_CHILD = 64,

    FAILED_MASK = FAILED_VALID | FAILED_CHILD
};

inline BlockStatus operator|(BlockStatus a, BlockStatus b) {
    return static_cast<BlockStatus>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

inline BlockStatus operator&(BlockStatus a, BlockStatus b) {
    return static_cast<BlockStatus>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
}

inline BlockStatus& operator|=(BlockStatus& a, BlockStatus b) {
    a = a | b;
    return a;
}

inline bool has_status(BlockStatus status, BlockStatus flag) {
    return (static_cast<uint32_t>(status) & static_cast<uint32_t>(flag)) != 0;
}

/**
 * Позиция блока на диске
 * Аналог CDiskBlockPos из Bitcoin
 */
struct DiskBlockPos {
    int32_t file = -1;      // Номер файла (blk00000.dat, blk00001.dat, ...)
    uint32_t pos = 0;       // Позиция в файле

    DiskBlockPos() = default;
    DiskBlockPos(int32_t file_, uint32_t pos_) : file(file_), pos(pos_) {}

    bool is_null() const { return file == -1; }
    void set_null() { file = -1; pos = 0; }

    bool operator==(const DiskBlockPos& other) const {
        return file == other.file && pos == other.pos;
    }

    bool operator!=(const DiskBlockPos& other) const {
        return !(*this == other);
    }
};

/**
 * Индекс блока - легковесная структура с метаданными
 * Аналог CBlockIndex из Bitcoin
 *
 * Хранится в памяти для быстрого доступа.
 * Полный блок загружается с диска только при необходимости.
 */
class BlockIndex {
public:
    // Хеш блока (ключ в BlockMap)
    Hash256 hash;

    // Указатель на предыдущий блок
    BlockIndex* prev = nullptr;

    // Указатель на блок для skip (для быстрого поиска)
    BlockIndex* skip = nullptr;

    // Высота блока (Genesis = 0)
    Height height = 0;

    // Позиция блока на диске
    DiskBlockPos block_pos;

    // Позиция undo данных на диске
    DiskBlockPos undo_pos;

    // Общая работа цепи до этого блока (включительно)
    // Используется для выбора лучшей цепи
    uint256 chain_work;

    // Количество транзакций в блоке
    uint32_t tx_count = 0;

    // Количество транзакций в цепи до этого блока (включительно)
    // Используется для оценки прогресса синхронизации
    uint64_t chain_tx = 0;

    // Статус блока (валидация, наличие данных)
    BlockStatus status = BlockStatus::UNKNOWN;

    // Заголовок блока (для быстрого доступа без загрузки с диска)
    core::BlockHeader header;

    // Sequence ID для определения порядка получения блоков
    uint32_t sequence_id = 0;

    BlockIndex() = default;
    explicit BlockIndex(const core::BlockHeader& header_);

    // Получить заголовок блока
    core::BlockHeader get_block_header() const { return header; }

    // Получить хеш блока
    Hash256 get_block_hash() const { return hash; }

    // Получить позицию блока на диске
    DiskBlockPos get_block_pos() const { return block_pos; }

    // Получить позицию undo данных на диске
    DiskBlockPos get_undo_pos() const { return undo_pos; }

    // Проверить, есть ли данные блока на диске
    bool has_data() const {
        return has_status(status, BlockStatus::HAVE_DATA);
    }

    // Проверить, есть ли undo данные на диске
    bool has_undo() const {
        return has_status(status, BlockStatus::HAVE_UNDO);
    }

    // Проверить, валиден ли блок
    bool is_valid(BlockStatus up_to = BlockStatus::VALID_TRANSACTIONS) const {
        if (is_failed()) {
            return false;
        }
        const uint32_t current =
            static_cast<uint32_t>(status) & static_cast<uint32_t>(BlockStatus::VALID_MASK);
        const uint32_t required =
            static_cast<uint32_t>(up_to) & static_cast<uint32_t>(BlockStatus::VALID_MASK);
        return current >= required;
    }

    // Проверить, провалил ли блок валидацию
    bool is_failed() const {
        return has_status(status, BlockStatus::FAILED_MASK);
    }

    // Получить предка на заданной высоте
    BlockIndex* get_ancestor(Height height);
    const BlockIndex* get_ancestor(Height height) const;

    // Построить skip указатель для быстрого поиска
    void build_skip();

    // Вычислить медианное время последних 11 блоков
    Timestamp get_median_time_past() const;

    // Получить время блока
    Timestamp get_block_time() const { return header.timestamp; }

    // Получить сложность блока
    uint32_t get_bits() const { return header.bits; }

    // Получить nonce блока
    uint64_t get_nonce() const { return header.nonce; }

    // Получить merkle root
    Hash256 get_merkle_root() const { return header.merkle_root; }

    // Получить prev_block_hash
    Hash256 get_prev_block_hash() const { return header.prev_block_hash; }

    // Получить версию блока
    uint32_t get_version() const { return header.version; }

    // Сравнение для сортировки по chainwork
    bool operator<(const BlockIndex& other) const;

    // Строковое представление для отладки
    std::string to_string() const;
};

/**
 * Компаратор для сортировки BlockIndex по chainwork
 * Используется для выбора лучшей цепи
 */
struct BlockIndexWorkComparator {
    bool operator()(const BlockIndex* a, const BlockIndex* b) const;
};

} // namespace geocoin::chain
