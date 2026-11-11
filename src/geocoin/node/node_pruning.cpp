#include "geocoin/node/node.hpp"

#include "geocoin/utils/logger.hpp"
#include "geocoin/utils/logging.hpp"

#include <cstdint>
#include <sstream>

using geocoin::utils::LogPrintf;

namespace geocoin::node {

PrunedDataInfo Node::get_pruned_data_info() const {
    PrunedDataInfo info;
    info.enabled = is_pruned_mode();
    info.retention = config_.pruned_block_retention;
    info.tip_height = get_active_height();
    info.first_available_height = 0;

    if (info.enabled && info.retention > 0 && info.tip_height + 1 > info.retention) {
        info.first_available_height = info.tip_height - info.retention + 1;
    }
    return info;
}

bool Node::is_height_pruned_for_api(Height height) const {
    const PrunedDataInfo info = get_pruned_data_info();
    return info.enabled && height < info.first_available_height;
}

bool Node::is_block_pruned_for_api(const Hash256& block_hash, Height* out_height) const {
    if (!is_pruned_mode() || !blockchain_) {
        return false;
    }
    const auto* idx = blockchain_->find_block_index(block_hash);
    if (!idx) {
        return false;
    }
    if (out_height) {
        *out_height = idx->height;
    }
    return is_height_pruned_for_api(idx->height);
}

std::string Node::describe_pruned_api_error(Height requested_height) const {
    const PrunedDataInfo info = get_pruned_data_info();
    std::ostringstream oss;
    oss << "Запрошены исторические данные на высоте " << requested_height
        << ", но эта Pruned-нода обслуживает только последние "
        << info.retention << " блоков"
        << " (доступно с высоты " << info.first_available_height
        << ", текущая высота " << info.tip_height
        << "). Повторите запрос к FullNode или архивной ноде.";
    return oss.str();
}

void Node::prune_old_block_bodies_if_needed() {
    if (!is_pruned_mode() || !config_.pruned_delete_old_blocks || !block_storage_ || !blockchain_) {
        return;
    }

    const PrunedDataInfo info = get_pruned_data_info();
    if (!info.enabled || info.first_available_height == 0) {
        return;
    }

    uint64_t pruned_count = 0, pruned_index_count = 0;
    if (!blockchain_->prune_history(info.retention, config_.pruned_delete_old_indexes,
                                    &pruned_count, &pruned_index_count)) {
        GEO_LOG_WARN("node", "History maintenance did not complete; source data retained where archival failed");
        return;
    }
    if (pruned_count || pruned_index_count) {
        LogPrintf("Pruned %llu block bodies and %llu index records (retention=%llu)\n",
                  static_cast<unsigned long long>(pruned_count),
                  static_cast<unsigned long long>(pruned_index_count),
                  static_cast<unsigned long long>(info.retention));
    }

}

}  // namespace geocoin::node
