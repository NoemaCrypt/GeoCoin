#pragma once

#include "geocoin/state/state.hpp"

#include <string_view>

namespace geocoin::state {

struct CardOverlay {
    std::map<std::string, CardProgramEntry> programs;
    std::map<std::string, CardInstanceEntry> cards;
    std::set<std::string> pubkeys;
    std::map<std::string, size_t> active_counts;
};

// Keeps registry and input reads for one transition under the state lock.
class CardReadView {
public:
    CardReadView(const ChainState& state, const CardOverlay* overlay = nullptr)
        : state_(state), lock_(state.mutex_), overlay_(overlay) {}
    bool get_program(const Hash256& id, CardProgramEntry& out) const;
    bool get_card(const Hash256& uid, CardInstanceEntry& out) const;
    bool has_pubkey(const Hash256& hash) const;
    size_t active_card_count(const Hash256& program) const;
    bool get_utxo(const Hash256& hash, uint32_t vout, UTXO& out) const {
        return state_.get_utxo(hash, vout, out);
    }
private:
    const ChainState& state_;
    std::unique_lock<std::recursive_mutex> lock_;
    const CardOverlay* overlay_;
};

struct CardTransition {
    std::optional<CardProgramEntry> program;
    std::optional<CardInstanceEntry> card;
};

// Registry and input-owner authority rules; witness ownership is verified
// separately by ChainState and need not be repeated during mining selection.
bool validate_card_transition(const CardReadView& state, const core::Transaction& tx,
                              std::string_view* error = nullptr);

// Calculates effects of an already validated transaction. ChainState calls
// this after spending the inputs, so it must not re-read their authority.
bool prepare_card_effects(const CardReadView& state, const core::Transaction& tx,
                          Height height, CardTransition& out);

} // namespace geocoin::state
