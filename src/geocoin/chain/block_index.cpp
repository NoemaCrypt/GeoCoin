#include "geocoin/chain/block_index.hpp"
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <limits>

namespace geocoin::chain {

// Вспомогательные функции (forward declarations)
static Height get_skip_height(Height height);
static Height invert_lowest_one(Height n);

BlockIndex::BlockIndex(const core::BlockHeader& header_)
    : header(header_)
{
    hash = header.get_hash();
    // Height = uint64_t, поэтому -1 здесь превращается в UINT64_MAX.
    // Используем явный sentinel "не вычислена".
    height = std::numeric_limits<Height>::max();
}

BlockIndex* BlockIndex::get_ancestor(Height target_height) {
    if (target_height > height || target_height < 0) {
        return nullptr;
    }

    BlockIndex* index = this;
    Height height_walk = height;

    while (height_walk > target_height) {
        // Используем skip для быстрого перехода
        Height height_skip = get_skip_height(height_walk);
        Height height_skip_prev = get_skip_height(height_walk - 1);

        if (index->skip != nullptr &&
            (height_skip == target_height ||
             (height_skip > target_height && !(height_skip_prev < target_height)))) {
            // Можем использовать skip
            index = index->skip;
            height_walk = height_skip;
        } else {
            // Идем по prev
            if (!index->prev) {
                // Цепь повреждена - нет предыдущего блока
                return nullptr;
            }
            index = index->prev;
            height_walk--;
        }
    }

    return index;
}

const BlockIndex* BlockIndex::get_ancestor(Height target_height) const {
    return const_cast<BlockIndex*>(this)->get_ancestor(target_height);
}

void BlockIndex::build_skip() {
    if (prev) {
        // КРИТИЧНО: Проверяем что get_ancestor вернула непустой результат
        BlockIndex* ancestor = prev->get_ancestor(get_skip_height(height));
        if (ancestor) {
            skip = ancestor;
        } else {
            skip = prev;  // Fallback: если нет, используем prev
        }
    }
}

Timestamp BlockIndex::get_median_time_past() const {
    std::vector<Timestamp> times;
    times.reserve(11);

    const BlockIndex* index = this;
    for (int i = 0; i < 11 && index; i++) {
        times.push_back(index->get_block_time());
        index = index->prev;
    }

    // КРИТИЧНО: Проверяем на пустой вектор
    if (times.empty()) {
        return 0;  // Fallback: вернуть нулевое время
    }

    std::sort(times.begin(), times.end());
    return times[times.size() / 2];
}

bool BlockIndex::operator<(const BlockIndex& other) const {
    // Сначала сравниваем chainwork
    if (chain_work < other.chain_work) return true;
    if (chain_work > other.chain_work) return false;

    // Затем по sequence_id (кто раньше получен)
    if (sequence_id < other.sequence_id) return true;
    if (sequence_id > other.sequence_id) return false;

    // Затем по указателю (для детерминизма)
    return this < &other;
}

std::string BlockIndex::to_string() const {
    std::stringstream ss;
    ss << "BlockIndex(height=" << height
       << ", hash=";

    for (int i = 0; i < 8; i++) {
        ss << std::hex << std::setw(2) << std::setfill('0') << (int)hash[i];
    }
    ss << "...";

    ss << std::dec << ", status=" << static_cast<uint32_t>(status)
       << ", chainwork=";

    // Выводим только старшие 64 бита chainwork
    ss << std::hex << chain_work.data[3] << std::dec
       << ")";

    return ss.str();
}

bool BlockIndexWorkComparator::operator()(const BlockIndex* a, const BlockIndex* b) const {
    if (a == b) {
        return false;
    }

    // Сначала по chainwork (больше = "меньше" в терминах set, чтобы идти первыми)
    if (a->chain_work > b->chain_work) return true;
    if (a->chain_work < b->chain_work) return false;

    // Затем по sequence_id (кто раньше получен — выше приоритет)
    if (a->sequence_id < b->sequence_id) return true;
    if (a->sequence_id > b->sequence_id) return false;

    // Затем по адресу указателя (детерминизм)
    return a < b;
}

// Инвертировать младший установленный бит
static Height invert_lowest_one(Height n) {
    return n & (n - 1);
}

// Вспомогательная функция для вычисления высоты skip
// Аналог GetSkipHeight из Bitcoin
static Height get_skip_height(Height height) {
    if (height < 2) {
        return 0;
    }

    // Определяем, на сколько блоков назад прыгать
    // Используем степени двойки для эффективного поиска
    return (height & 1) ? invert_lowest_one(invert_lowest_one(height - 1)) + 1 : invert_lowest_one(height);
}

} // namespace geocoin::chain
