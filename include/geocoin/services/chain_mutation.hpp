#pragma once

#include "geocoin/consensus/validation.hpp"
#include "geocoin/core/block.hpp"
#include "geocoin/core/transaction.hpp"
#include "geocoin/types.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace geocoin::services {

enum class SubmissionOutcome {
    ACCEPTED,
    REJECTED,
    UNAVAILABLE,
    INTERNAL_ERROR
};

struct ChainBlockChange {
    Height height{0};
    Hash256 hash{};
    core::Block block;
    bool body_available{false};
};

/** Immutable description of one active-chain transition. */
struct ChainTransition {
    uint64_t sequence{0};
    Hash256 old_tip_hash{};
    Height old_height{0};
    Hash256 new_tip_hash{};
    Height new_height{0};
    Height fork_height{0};
    std::vector<ChainBlockChange> disconnected; // old tip -> fork
    std::vector<ChainBlockChange> connected;    // fork -> new tip
    size_t approximate_bytes{0};

    bool is_reorg() const noexcept {
        return !disconnected.empty();
    }
};

enum class ChainMutationEventKind {
    TRANSACTION_ACCEPTED,
    ACTIVE_CHAIN_CHANGED,
    /** One or more committed events were coalesced; observers must reconcile. */
    RESCAN_REQUIRED
};

struct ChainEventGap {
    uint64_t first_missing_sequence{0};
    uint64_t last_missing_sequence{0};
    bool chain_transition_lost{false};
    Height rescan_from_height{0};
};

/**
 * FIFO event emitted by the single writer after the corresponding mutation is
 * committed. Consumers receive value objects and never inspect mutable chain
 * internals to reconstruct what happened.
 */
struct ChainMutationEvent {
    uint64_t sequence{0};
    ChainMutationEventKind kind{ChainMutationEventKind::TRANSACTION_ACCEPTED};
    core::Transaction transaction;
    uint64_t observed_timestamp{0};
    std::string source_peer;
    ChainTransition transition;
    ChainEventGap gap;
    size_t approximate_bytes{0};
};

struct BlockSubmissionResult {
    SubmissionOutcome outcome{SubmissionOutcome::UNAVAILABLE};
    bool active_tip_changed{false};
    bool block_is_active{false};
    Height block_height{0};
    Height active_height{0};
    Hash256 active_tip_hash{};
    consensus::ValidationResult validation_result{consensus::ValidationResult::ERROR_STATE};
    consensus::RejectCode reject_code{consensus::RejectCode::INVALID};
    std::string reject_reason;
    std::string debug_message;
    uint64_t event_sequence{0};

    bool is_accepted() const noexcept {
        return outcome == SubmissionOutcome::ACCEPTED;
    }
};

struct ChainTipActivationResult {
    SubmissionOutcome outcome{SubmissionOutcome::UNAVAILABLE};
    Hash256 requested_tip_hash{};
    bool active_tip_changed{false};
    Height active_height{0};
    Hash256 active_tip_hash{};
    consensus::ValidationResult validation_result{consensus::ValidationResult::ERROR_STATE};
    consensus::RejectCode reject_code{consensus::RejectCode::INVALID};
    std::string reject_reason;
    std::string debug_message;
    uint64_t event_sequence{0};

    bool is_accepted() const noexcept {
        return outcome == SubmissionOutcome::ACCEPTED;
    }
};

struct TransactionSubmissionResult {
    SubmissionOutcome outcome{SubmissionOutcome::UNAVAILABLE};
    Hash256 txid{};
    std::string reject_reason;
    std::string debug_message;
    uint64_t event_sequence{0};

    bool is_accepted() const noexcept {
        return outcome == SubmissionOutcome::ACCEPTED;
    }
};

/**
 * Narrow mutation port shared by RPC, P2P, mining and GUI adapters.
 *
 * Read-only consumers deliberately use separate query APIs.  Implementations
 * must serialize mutations so Blockchain, ChainState and Mempool cannot be
 * changed concurrently by unrelated frontend threads.
 */
class ChainMutationService {
public:
    virtual ~ChainMutationService() = default;

    virtual BlockSubmissionResult process_block(const core::Block& block) = 0;
    virtual ChainTipActivationResult activate_tip(const Hash256& tip_hash) = 0;
    virtual TransactionSubmissionResult admit_transaction(const core::Transaction& tx) = 0;
};

} // namespace geocoin::services
