#include "geocoin/consensus/optimized_miner.hpp"
#include "geocoin/consensus/consensus.hpp"
#include "geocoin/consensus/proof_of_work.hpp"
#include "geocoin/core/block.hpp"
#include "geocoin/consensus/geo_tag.hpp"
#include "geocoin/crypto/geo_area.hpp"
#include "geocoin/types.hpp"
#include "geocoin/utils/bytes.hpp"

#include <atomic>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {

void expect(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

namespace consensus = geocoin::consensus;

} // namespace

namespace geocoin::consensus {
struct OptimizedMinerTestAccess {
    static bool quick_check_target(OptimizedMiner& miner, const Hash256& hash, const uint256& target)
    {
        return miner.quick_check_target(hash, target);
    }

    static bytes compute_midstate(OptimizedMiner& miner, const core::BlockHeader& header)
    {
        return miner.compute_midstate(header);
    }

    static Hash256 optimized_sha256(OptimizedMiner& miner, const bytes& data, const bytes& midstate)
    {
        return miner.optimized_sha256(data, midstate);
    }
};
}

namespace {

geocoin::core::Block make_block()
{
    geocoin::core::Block block;
    block.header.version = 1;
    block.header.timestamp = 1710000000;
    block.header.bits = 0x207fffff;
    block.header.nonce = 0;
    for (size_t i = 0; i < block.header.prev_block_hash.size(); ++i) {
        block.header.prev_block_hash[i] = static_cast<uint8_t>(i + 1);
        block.header.merkle_root[i] = static_cast<uint8_t>(0x80 + i);
        block.header.witness_commitment[i] = static_cast<uint8_t>(0x40 + i);
    }
    return block;
}

void test_backend_selection()
{
    using namespace geocoin::consensus;

    expect(set_mining_sha256_backend("legacy"), "legacy backend is accepted");
    expect(get_mining_sha256_backend() == "legacy", "legacy backend is reported");
    expect(set_mining_sha256_backend("ioke-legacy"), "ioke legacy alias is accepted");
    expect(get_mining_sha256_backend() == "legacy", "ioke legacy alias maps to legacy");
    expect(set_mining_sha256_backend("tonc-legacy"), "tonc legacy alias is accepted");
    expect(get_mining_sha256_backend() == "legacy", "tonc legacy alias maps to legacy");
    expect(set_mining_sha256_backend("portable"), "portable alias is accepted");
    expect(get_mining_sha256_backend() == "legacy", "portable alias maps to legacy");
    expect(set_mining_sha256_backend("openssl"), "openssl backend is accepted");
    expect(get_mining_sha256_backend() == "openssl", "openssl backend is reported");
    expect(set_mining_sha256_backend("native"), "native backend request is accepted");
    expect(get_mining_sha256_backend().find("native") == 0, "native backend is reported with implementation label");
    expect(set_mining_sha256_backend("sha-ni"), "sha-ni alias is accepted");
    expect(get_mining_sha256_backend().find("native") == 0, "sha-ni alias maps to native backend");
    expect(set_mining_sha256_backend("tonc-native"), "tonc native alias is accepted");
    expect(get_mining_sha256_backend().find("native") == 0, "tonc native alias maps to native backend");
    expect(set_mining_sha256_backend("AUTO"), "auto backend accepts uppercase spelling");
    expect(get_mining_sha256_backend().find("auto/") == 0, "auto backend reports effective implementation");
    expect(!set_mining_sha256_backend("definitely-not-a-backend"), "unknown backend is rejected");
    expect(set_mining_sha256_backend("legacy"), "test restores deterministic legacy backend");
}

void test_fast_compare_hash_target()
{
    using namespace geocoin;
    using namespace geocoin::consensus;

    uint256 target;
    target.data[3] = 0x00000000ffffffffULL;
    target.data[2] = 0xffffffffffffffffULL;
    target.data[1] = 0xffffffffffffffffULL;
    target.data[0] = 0xffffffffffffffffULL;

    Hash256 low{};
    low[7] = 0x01;
    expect(fast_compare_hash_target(low.data(), target), "low hash passes target");

    Hash256 high{};
    high[0] = 0x01;
    expect(!fast_compare_hash_target(high.data(), target), "high hash fails target");

    Hash256 equal{};
    equal[4] = 0xff;
    equal[5] = 0xff;
    equal[6] = 0xff;
    equal[7] = 0xff;
    std::memset(equal.data() + 8, 0xff, 24);
    expect(fast_compare_hash_target(equal.data(), target), "hash equal to target passes");
}

void test_optimized_miner_private_helpers()
{
    using namespace geocoin;
    using namespace geocoin::consensus;

    OptimizedMiner miner;
    uint256 target;
    target.data[3] = 0x0100000000000000ULL;

    Hash256 lower{};
    lower[0] = 0x00;
    expect(OptimizedMinerTestAccess::quick_check_target(miner, lower, target),
           "quick target check accepts lower high word");

    Hash256 higher{};
    higher[0] = 0x02;
    expect(!OptimizedMinerTestAccess::quick_check_target(miner, higher, target),
           "quick target check rejects higher high word");

    Hash256 equal{};
    equal[0] = 0x01;
    expect(OptimizedMinerTestAccess::quick_check_target(miner, equal, target),
           "quick target check defers equal high word to full check");

    const auto block = make_block();
    expect(OptimizedMinerTestAccess::compute_midstate(miner, block.header).empty(),
           "placeholder midstate helper returns empty bytes");
    const geocoin::bytes data{'a', 'b', 'c'};
    expect(OptimizedMinerTestAccess::optimized_sha256(miner, data, {}) == geocoin::crypto::sha256(data),
           "placeholder optimized sha256 helper matches normal sha256");
}

bool is_zero_target(const geocoin::uint256& target)
{
    return target.data[0] == 0 && target.data[1] == 0 && target.data[2] == 0 && target.data[3] == 0;
}

void test_proof_of_work_compact_and_reward_edges()
{
    using namespace geocoin;
    using namespace geocoin::consensus;

    ProofOfWork pow;
    (void)pow;

    uint256 tiny = ProofOfWork::bits_to_target(0x01010000);
    expect(tiny.data[0] == 1, "compact exponent <= 3 shifts mantissa down");

    uint256 spanning = ProofOfWork::bits_to_target(0x097fffff);
    expect(spanning.data[0] != 0 && spanning.data[1] != 0,
           "compact target spanning uint64 words is preserved");

    expect(is_zero_target(ProofOfWork::bits_to_target(0x1d80ffff)),
           "negative compact target decodes to zero");
    expect(ProofOfWork::is_valid_target_bits(ProofOfWork::get_initial_bits()),
           "initial target bits are canonical and in range");
    expect(!ProofOfWork::is_valid_target_bits(0),
           "zero compact target is invalid");
    expect(!ProofOfWork::is_valid_target_bits(0x1d80ffff),
           "negative compact target is invalid");
    expect(!ProofOfWork::is_valid_target_bits(0x2300ffff),
           "overflowing compact target is invalid");
    expect(!ProofOfWork::is_valid_target_bits(0x1e00ffff),
           "target easier than pow limit is invalid");
    expect(!ProofOfWork::is_valid_target_bits(0x1d000001),
           "non-canonical compact target is invalid");
    expect(calculate_chainwork(nullptr, 0).is_zero(),
           "invalid target contributes no chainwork");
    expect(ProofOfWork::target_to_bits(uint256{}) == 0, "zero target encodes as zero bits");

    uint256 byte30;
    byte30.data[0] = 0x0000000000000100ULL;
    expect(ProofOfWork::target_to_bits(byte30) == 0x02010000,
           "target_to_bits handles second-to-last byte");

    uint256 byte31;
    byte31.data[0] = 0x0000000000000001ULL;
    expect(ProofOfWork::target_to_bits(byte31) == 0x01010000,
           "target_to_bits handles last byte");

    uint256 normalized;
    normalized.data[3] = 0x8000000000000000ULL;
    expect(ProofOfWork::target_to_bits(normalized) == 0x21008000,
           "target_to_bits normalizes mantissa high bit");

    expect(ProofOfWork::calculate_block_reward(0) == 50 * COIN, "genesis-era reward is initial reward");
    expect(ProofOfWork::calculate_block_reward(ProofOfWork::get_halving_interval()) == 25 * COIN,
           "first halving halves reward");
    expect(ProofOfWork::calculate_block_reward(ProofOfWork::get_halving_interval() * 64) == 0,
           "reward is zero after 64 halvings");

}

void test_proof_of_work_verify_guards()
{
    using namespace geocoin;
    using namespace geocoin::consensus;

    core::BlockHeader header = make_block().header;
    Hash256 prev_hash = header.prev_block_hash;
    const uint32_t prev_timestamp = header.timestamp - 60;
    header.bits = 0x207fffff;

    expect(!ProofOfWork::verify_proof_of_work(header, 0x1d00ffff, prev_timestamp, prev_hash, 1),
           "verify rejects mismatched bits before hashing");

    header.geo_tag = {};
    expect(!ProofOfWork::verify_proof_of_work(header, header.bits, prev_timestamp, prev_hash, 1),
           "verify rejects stale geo tag");

    const auto tag = geo::compute_geo_tag(prev_timestamp, prev_hash);
    header.geo_tag = tag.geo_tag;
    expect(ProofOfWork::compute_pow_hash(header, prev_timestamp, prev_hash, 1).size() == 32,
           "pow hash is always 32 bytes");
}

void test_inline_sha256_transform_known_vector()
{
    using namespace geocoin;
    using namespace geocoin::consensus;

    uint8_t block[64]{};
    block[0] = 'a';
    block[1] = 'b';
    block[2] = 'c';
    block[3] = 0x80;
    block[63] = 24;

    uint32_t state[8] = {
        0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
        0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U,
    };
    inline_sha256_transform(state, block);

    uint8_t digest[32]{};
    for (int i = 0; i < 8; ++i) {
        digest[i * 4 + 0] = static_cast<uint8_t>(state[i] >> 24);
        digest[i * 4 + 1] = static_cast<uint8_t>(state[i] >> 16);
        digest[i * 4 + 2] = static_cast<uint8_t>(state[i] >> 8);
        digest[i * 4 + 3] = static_cast<uint8_t>(state[i]);
    }
    const std::string hex = geocoin::utils::bytes_to_hex(digest, sizeof(digest));
    expect(hex == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
           "inline sha256 transform matches SHA256(abc)");

    inline_sha256_transform(nullptr, block);
    inline_sha256_transform(state, nullptr);
}

void test_mine_block_limits_stop_and_easy_target()
{
    using namespace geocoin;
    using namespace geocoin::consensus;

    expect(set_mining_sha256_backend("legacy"), "legacy backend is accepted before mining test");

    OptimizedMiner miner;
    core::Block block = make_block();
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> hashes{0};
    expect(!miner.mine_block_optimized(block, 0x207fffff, 0, {}, 0, 100, &stop, -1, &hashes),
           "stop flag false exits before work");
    expect(hashes.load() == 0, "stop-before-work does not count hashes");

    stop.store(true);
    block = make_block();
    hashes.store(0);
    expect(!miner.mine_block_optimized(block, 0x01003456, 0, {}, 0, 5, &stop, -1, &hashes),
           "hard target with tiny iteration budget fails predictably");
    expect(hashes.load() == 5, "max iteration budget counts attempted hashes");
    expect(block.header.nonce == 5, "nonce advances by default step");

    block = make_block();
    hashes.store(0);
    expect(!miner.mine_block_optimized(block, 0x01003456, 0, {}, 0, 1, &stop, 0, &hashes),
           "thread zero emits target diagnostics and respects one-iteration budget");
    expect(hashes.load() == 1, "thread zero one-iteration mining counts one hash");

    expect(set_mining_sha256_backend("openssl"), "openssl backend is accepted for mining path");
    block = make_block();
    hashes.store(0);
    expect(!miner.mine_block_optimized(block, 0x01003456, 0, {}, 0, 1, &stop, -1, &hashes),
           "openssl mining path respects one-iteration budget");
    expect(hashes.load() == 1, "openssl mining path counts one hash");
    expect(set_mining_sha256_backend("legacy"), "mining path restores legacy backend");

    block = make_block();
    block.header.nonce = 7;
    hashes.store(0);
    expect(!miner.mine_block_optimized(block, 0x01003456, 0, {}, 0, 3, &stop, -1, &hashes, 4),
           "nonce step is applied under max iteration budget");
    expect(hashes.load() == 3, "stepped mining counts attempted hashes");
    expect(block.header.nonce == 19, "nonce advances by custom step");

    block = make_block();
    hashes.store(0);
    expect(!miner.mine_block_optimized(block, 0x01003456, 0, {}, 0, 2, &stop, -1, &hashes, 0),
           "zero nonce step is normalized to one");
    expect(hashes.load() == 2, "zero-step normalization still counts attempted hashes");
    expect(block.header.nonce == 2, "zero-step normalization advances nonce by one per hash");

    block = make_block();
    block.header.nonce = std::numeric_limits<uint64_t>::max();
    hashes.store(0);
    expect(!miner.mine_block_optimized(block, 0x01003456, 0, {}, 0, 2, &stop, -1, &hashes),
           "nonce overflow stops mining attempt");
    expect(hashes.load() == 1, "nonce overflow flushes attempted hash count");
    expect(block.header.nonce == 0, "nonce wraps before overflow guard returns");

    block = make_block();
    hashes.store(0);
    expect(miner.mine_block_optimized(block, 0x2100ffff, 0, {}, 0, 32, &stop, -1, &hashes),
           "very easy target finds a block quickly");
    expect(block.header.bits == 0x2100ffff, "mined block stores target bits");
    expect(hashes.load() <= 32, "easy mining respects iteration budget");
}

} // namespace

int main()
{
    try {
        test_backend_selection();
        test_fast_compare_hash_target();
        test_optimized_miner_private_helpers();
        test_proof_of_work_compact_and_reward_edges();
        test_proof_of_work_verify_guards();
        test_inline_sha256_transform_known_vector();
        test_mine_block_limits_stop_and_easy_target();
        std::cout << "test_mining_units: ok\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "test_mining_units failed: " << e.what() << "\n";
        return 1;
    }
}
