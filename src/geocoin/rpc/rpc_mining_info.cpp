#include "geocoin/rpc/rpc_server.hpp"
#include "rpc_mining_helpers.hpp"

#include "geocoin/rpc/rpc_params.hpp"
#include "geocoin/core/blockchain.hpp"
#include "geocoin/network/mempool.hpp"
#include "geocoin/consensus/chainparams.hpp"
#include <algorithm>
#include <cmath>

namespace geocoin::rpc {

void RPCServer::register_mining_info_methods() {
    register_method({
        .name = "getmininginfo",
        .method = [this](const json& params) -> json {
            (void)params;
            auto& blockchain = require_blockchain();

            json result;
            result["blocks"] = blockchain.get_height();
            result["currentblocksize"] = 0;
            result["currentblocktx"] = 0;

            auto* tip = blockchain.get_tip();
            if (tip) {
                result["difficulty"] = bits_to_difficulty(tip->get_bits());
            } else {
                result["difficulty"] = 1.0;
            }

            result["errors"] = "";
            result["networkhashps"] = 0.0;
            result["pooledtx"] = mempool_ ? static_cast<uint64_t>(mempool_->size()) : 0;
            result["chain"] = consensus::Params().network_id();

            if (blockchain.get_height() > 10) {
                try {
                    Height current_height = blockchain.get_height();
                    int blocks_to_check = std::min(120, static_cast<int>(current_height));
                    Height start_height = current_height - blocks_to_check + 1;
                    if (node_services_.info && node_services_.info->is_pruned_mode()) {
                        const auto pruned = node_services_.info->get_pruned_data_info();
                        if (start_height < pruned.first_available_height) {
                            start_height = pruned.first_available_height;
                            if (current_height >= start_height) {
                                blocks_to_check = static_cast<int>(current_height - start_height + 1);
                            } else {
                                blocks_to_check = 0;
                            }
                            result["warnings"] = "networkhashps limited by pruned node history window";
                        }
                    }

                    core::Block start_block, end_block;
                    if (blocks_to_check > 0 &&
                        blockchain.get_block_by_height(start_height, start_block) &&
                        blockchain.get_block_by_height(current_height, end_block)) {
                        if (end_block.header.timestamp > start_block.header.timestamp) {
                            const uint64_t time_diff =
                                end_block.header.timestamp - start_block.header.timestamp;
                            double avg_difficulty = bits_to_difficulty(end_block.header.bits);
                            double hashrate = (blocks_to_check * avg_difficulty * 4294967296.0) / time_diff;
                            result["networkhashps"] = hashrate;
                        }
                    }
                } catch (...) {
                }
            }

            if (node_services_.info) {
                const auto status = node_services_.info->get_status();
                result["generate"] = status.mining;
                result["genproclimit"] = static_cast<int>(status.mining_threads);
                result["hashrate"] = status.hashrate;
            } else {
                result["generate"] = false;
                result["genproclimit"] = 0;
                result["hashrate"] = 0.0;
            }

            return result;
        },
        .category = "mining",
        .help = "getmininginfo\n\nВозвращает объект с информацией о майнинге.\n\nРезультат:\n{\n  \"blocks\": nnn,             (numeric) текущее количество блоков\n  \"currentblocksize\": nnn,   (numeric) размер последнего блока\n  \"currentblocktx\": nnn,     (numeric) количество транзакций в последнем блоке\n  \"difficulty\": xxx.xxxxx    (numeric) текущая сложность\n  \"errors\": \"...\"            (string) текущие ошибки\n  \"networkhashps\": nnn,      (numeric) хешрейт сети\n  \"pooledtx\": n              (numeric) размер mempool\n  \"chain\": \"xxxx\",           (string) текущая сеть\n}\n",
        .requires_wallet = false,
        .required_role = RPCAuthRole::MINER
    });

    register_method({
        .name = "getmempoolinfo",
        .method = [this](const json& params) -> json {
            (void)params;
            auto& mempool = require_mempool();

            const auto stats = mempool.get_stats();
            json result;
            result["size"] = stats.tx_count;
            result["bytes"] = stats.total_size;
            result["usage"] = stats.total_size;
            result["maxmempool"] = mempool.max_size_limit();
            result["total_fee_angles"] = stats.total_fees;
            result["min_fee_per_byte"] = stats.min_fee_per_byte;
            result["max_fee_per_byte"] = stats.max_fee_per_byte;
            result["avg_fee_per_byte"] = stats.avg_fee_per_byte;
            result["transactions_updated"] = mempool.get_transactions_updated();
            return result;
        },
        .category = "mempool",
        .help = "getmempoolinfo\n\nВозвращает read-only статистику mempool.\n",
        .requires_wallet = false,
        .required_role = RPCAuthRole::MINER,
        .capabilities = {"readonly", "miner"}
    });

    register_method({
        .name = "getnetworkhashps",
        .method = [this](const json& params) -> json {
            auto& blockchain = require_blockchain();

            int blocks = optional_param<int>(params, 0, 120);
            int height_param = optional_param<int>(params, 1, -1);

            Height current_height = blockchain.get_height();
            if (height_param >= 0) {
                current_height = static_cast<Height>(height_param);
            }
            require_height_available_for_mining_rpc(node_services_.info.get(), current_height);

            if (current_height < 1 || blocks < 1) {
                return 0.0;
            }

            if (blocks > static_cast<int>(current_height)) {
                blocks = current_height;
            }

            Height start_height = current_height - blocks + 1;
            if (start_height < 1) {
                start_height = 1;
            }
            require_height_available_for_mining_rpc(node_services_.info.get(), start_height);

            core::Block end_block;
            if (!blockchain.get_block_by_height(current_height, end_block)) {
                return 0.0;
            }

            core::Block start_block;
            uint64_t time_diff = 0;
            Height effective_start = start_height;
            int effective_blocks = blocks;

            while (true) {
                require_height_available_for_mining_rpc(node_services_.info.get(), effective_start);
                if (!blockchain.get_block_by_height(effective_start, start_block)) {
                    return 0.0;
                }

                if (end_block.header.timestamp > start_block.header.timestamp) {
                    time_diff = end_block.header.timestamp - start_block.header.timestamp;
                    break;
                }

                if (effective_start <= 1) {
                    return 0.0;
                }

                effective_start -= 1;
                effective_blocks += 1;

                if (effective_blocks > 5000) {
                    return 0.0;
                }
            }

            double diff = bits_to_difficulty(end_block.header.bits);
            if (!(diff > 0.0) || std::isnan(diff) || std::isinf(diff)) {
                diff = 1.0;
            }

            double hashrate = (static_cast<double>(effective_blocks) * diff * 4294967296.0) / static_cast<double>(time_diff);
            if (std::isnan(hashrate) || std::isinf(hashrate) || hashrate < 0.0) {
                return 0.0;
            }

            return hashrate;
        },
        .category = "mining",
        .help = "getnetworkhashps ( blocks height )\n\nВозвращает оценку хешрейта сети в хешах в секунду на основе chainwork.\n\nАргументы:\n1. blocks     (numeric, optional, default=120) Количество блоков для оценки\n2. height     (numeric, optional, default=-1) Высота для оценки (-1 = текущая)\n\nРезультат:\nx             (numeric) Хеши в секунду\n",
        .requires_wallet = false,
        .required_role = RPCAuthRole::MINER,
        .param_names = {"blocks", "height"}
    });
}

} // namespace geocoin::rpc
