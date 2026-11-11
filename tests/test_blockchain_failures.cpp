#include "chain_test_fixture.hpp"

#include <iostream>

using namespace geocoin;
using test::expect;

namespace {

class FailingStorage final : public state::MemoryStorage {
public:
    bool fail_commit = false;
    bool fail_journal = false;
    std::string fail_metadata_prefix;

    bool commit_batch() override {
        if (fail_commit) {
            fail_commit = false;
            return false;
        }
        return MemoryStorage::commit_batch();
    }

    bool put_metadata(const std::string& key, const bytes& value) override {
        if (fail_journal && key.find("journal") != std::string::npos) {
            fail_journal = false;
            return false;
        }
        if (!fail_metadata_prefix.empty() && key.rfind(fail_metadata_prefix, 0) == 0) {
            fail_metadata_prefix.clear();
            return false;
        }
        return MemoryStorage::put_metadata(key, value);
    }
};

void test_unavailable_body_is_retryable(bool forced) {
    test::TestChain fixture;
    const auto block = fixture.make_block();
    auto* index = fixture.accept(block);
    fixture.blocks->close();

    consensus::ValidationState failure;
    const bool accepted = forced
        ? fixture.chain->switch_to_tip(block.get_hash(), failure)
        : fixture.chain->activate_best_chain(failure);
    expect(!accepted && failure.is_error(), "missing body is a local error");
    expect(!index->is_failed(), "local read failure must not invalidate the candidate");
    expect(fixture.chain->get_height() == 0, "failed connect preserves old tip");

    expect(fixture.blocks->open((fixture.directory / "blocks").string()), "storage reopens");
    fixture.append(block);
}

void test_state_write_failure_is_retryable(bool journal) {
    auto backend = std::make_unique<FailingStorage>();
    auto* faults = backend.get();
    test::TestChain fixture(std::move(backend));
    const auto old_tip = fixture.state->get_tip_hash();
    const auto block = fixture.make_block();
    if (journal) faults->fail_journal = true;
    else faults->fail_commit = true;

    consensus::ValidationState failure;
    expect(!fixture.chain->process_new_block(block, failure), "injected write failure rejects this attempt");
    expect(failure.is_error(), "write failure is not a consensus violation");
    expect(!fixture.chain->find_block_index(block.get_hash())->is_failed(), "write failure keeps candidate retryable");
    expect(fixture.state->get_tip_hash() == old_tip && fixture.chain->get_height() == 0,
           "write failure rolls state and active chain back together");
    fixture.append(block);
}

void test_invalid_reward_is_not_retryable(bool forced) {
    test::TestChain fixture;
    const auto block = fixture.make_block(nullptr, COIN);
    auto* index = fixture.accept(block);
    consensus::ValidationState failure;
    const bool accepted = forced
        ? fixture.chain->switch_to_tip(block.get_hash(), failure)
        : fixture.chain->activate_best_chain(failure);
    expect(!accepted && failure.is_invalid(), "excess reward is a consensus rejection");
    expect(index->is_failed(), "invalid block must be removed from fork choice");
    expect(!fixture.state->get_storage()->has_metadata("chainstate_journal"),
           "pre-mutation consensus rejection leaves no recovery journal");

    consensus::ValidationState retry;
    expect(!fixture.chain->process_new_block(block, retry) &&
               retry.get_reject_reason() == "duplicate-invalid",
           "known invalid candidate stays rejected");
    fixture.append(fixture.make_block());
}

void test_required_active_index_write_is_retryable() {
    auto backend = std::make_unique<FailingStorage>();
    auto* faults = backend.get();
    test::TestChain fixture(std::move(backend));
    const auto old_tip = fixture.state->get_tip_hash();
    const auto block = fixture.make_block();
    faults->fail_metadata_prefix = "atx:";

    consensus::ValidationState failure;
    expect(!fixture.chain->process_new_block(block, failure),
           "active transaction marker write failure rejects this attempt");
    expect(failure.is_error(), "active transaction marker failure is a local storage error");
    expect(!fixture.chain->find_block_index(block.get_hash())->is_failed(),
           "active transaction marker failure keeps the candidate retryable");
    expect(fixture.state->get_tip_hash() == old_tip && fixture.chain->get_height() == 0,
           "active transaction marker failure restores state and active chain");
    fixture.append(block);
}

} // namespace

int main() {
    try {
        consensus::SelectParams(consensus::Network::LOADNET);
        test_unavailable_body_is_retryable(false);
        test_unavailable_body_is_retryable(true);
        test_state_write_failure_is_retryable(false);
        test_state_write_failure_is_retryable(true);
        test_invalid_reward_is_not_retryable(false);
        test_invalid_reward_is_not_retryable(true);
        test_required_active_index_write_is_retryable();
        std::cout << "Blockchain failure recovery tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
