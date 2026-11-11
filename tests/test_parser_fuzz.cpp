#include "geocoin/core/transaction.hpp"
#include "geocoin/core/block.hpp"
#include "geocoin/consensus/consensus.hpp"
#include "geocoin/network/messages.hpp"
#include "geocoin/rpc/rpc_dispatch.hpp"
#include "geocoin/rpc/rpc_protocol.hpp"
#include "geocoin/utils/varint.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <new>
#include <string>
#include <vector>

namespace {
std::atomic<bool> track_allocations{false};
std::atomic<size_t> largest_allocation{0};
} // namespace

void* operator new(std::size_t size) {
    if (track_allocations.load(std::memory_order_relaxed)) {
        size_t seen = largest_allocation.load(std::memory_order_relaxed);
        while (size > seen &&
               !largest_allocation.compare_exchange_weak(seen, size, std::memory_order_relaxed)) {
        }
    }
    if (void* memory = std::malloc(size == 0 ? 1 : size)) {
        return memory;
    }
    throw std::bad_alloc();
}

void operator delete(void* memory) noexcept {
    std::free(memory);
}

void operator delete(void* memory, std::size_t) noexcept {
    std::free(memory);
}

using namespace geocoin;

namespace {

int failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

class XorShift64 {
public:
    explicit XorShift64(uint64_t seed) : state_(seed) {}

    uint64_t next() {
        uint64_t x = state_;
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        state_ = x;
        return x;
    }

    uint8_t next_byte() {
        return static_cast<uint8_t>(next() & 0xffu);
    }

private:
    uint64_t state_;
};

bytes random_bytes(XorShift64& rng, size_t max_size) {
    const size_t size = static_cast<size_t>(rng.next() % (max_size + 1));
    bytes out(size);
    for (auto& byte : out) {
        byte = rng.next_byte();
    }
    return out;
}

std::string random_ascii(XorShift64& rng, size_t max_size) {
    const size_t size = static_cast<size_t>(rng.next() % (max_size + 1));
    std::string out;
    out.reserve(size);
    for (size_t i = 0; i < size; ++i) {
        out.push_back(static_cast<char>(32 + (rng.next_byte() % 95)));
    }
    return out;
}

network::MessageType random_known_message_type(XorShift64& rng) {
    static constexpr std::array<network::MessageType, 15> kTypes{{
        network::MessageType::VERSION,
        network::MessageType::VERACK,
        network::MessageType::ADDR,
        network::MessageType::INV,
        network::MessageType::GETDATA,
        network::MessageType::BLOCK,
        network::MessageType::TX,
        network::MessageType::GETBLOCKS,
        network::MessageType::GETHEADERS,
        network::MessageType::HEADERS,
        network::MessageType::CHALLENGE,
        network::MessageType::RESPONSE,
        network::MessageType::PING,
        network::MessageType::PONG,
        network::MessageType::REJECT,
    }};
    return kTypes[static_cast<size_t>(rng.next() % kTypes.size())];
}

template <typename Fn>
void must_not_escape(Fn&& fn, const char* message) {
    try {
        fn();
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << message << ": " << e.what() << "\n";
        ++failures;
    } catch (...) {
        std::cerr << "FAIL: " << message << ": non-standard exception\n";
        ++failures;
    }
}

template <typename Parser>
void try_parser(Parser&& parser, const bytes& data) {
    try {
        parser(data);
    } catch (const std::exception&) {
    }
}

void fuzz_p2p_framing() {
    XorShift64 rng(0x67656f636f696e01ULL);
    for (size_t i = 0; i < 2500; ++i) {
        bytes data = random_bytes(rng, 768);
        must_not_escape([&] {
            network::MessageHeader header;
            bytes payload;
            const bool ok = network::parse_message(data, header, payload);
            if (ok) {
                check(network::is_known_message_type(header.type), "parsed message type must be known");
                check(payload.size() == header.length, "parsed payload length must match header length");
            }
        }, "parse_message must not throw");

        try {
            (void)network::MessageHeader::deserialize(data);
        } catch (const std::exception&) {
        }
    }

    for (size_t i = 0; i < 500; ++i) {
        const bytes payload = random_bytes(rng, 256);
        const auto type = random_known_message_type(rng);
        bytes framed = network::create_message(type, payload, network::MessageHeader::MAGIC_MAINNET);

        network::MessageHeader header;
        bytes parsed_payload;
        check(network::parse_message(framed, header, parsed_payload), "valid framed message must parse");
        check(header.type == type, "valid framed message type roundtrip");
        check(parsed_payload == payload, "valid framed payload roundtrip");

        if (!framed.empty()) {
            framed[static_cast<size_t>(rng.next() % framed.size())] ^= rng.next_byte();
            must_not_escape([&] {
                network::MessageHeader mutated_header;
                bytes mutated_payload;
                (void)network::parse_message(framed, mutated_header, mutated_payload);
            }, "mutated parse_message must not throw");
        }
    }
}

void fuzz_p2p_payload_parsers() {
    XorShift64 rng(0x67656f636f696e02ULL);
    for (size_t i = 0; i < 1500; ++i) {
        const bytes data = random_bytes(rng, 512);
        try_parser([](const bytes& v) { (void)network::VersionMessage::deserialize(v); }, data);
        try_parser([](const bytes& v) { (void)network::AddrMessage::deserialize(v); }, data);
        try_parser([](const bytes& v) { (void)network::InvMessage::deserialize(v); }, data);
        try_parser([](const bytes& v) { (void)network::GetBlocksMessage::deserialize(v); }, data);
        try_parser([](const bytes& v) { (void)network::HeadersMessage::deserialize(v); }, data);
        try_parser([](const bytes& v) { (void)network::RejectMessage::deserialize(v); }, data);
        try_parser([](const bytes& v) { (void)network::ChallengeMessage::deserialize(v); }, data);
        try_parser([](const bytes& v) { (void)network::ResponseMessage::deserialize(v); }, data);
    }
}

void fuzz_rpc_basic_auth() {
    XorShift64 rng(0x67656f636f696e03ULL);
    for (size_t i = 0; i < 2000; ++i) {
        std::string user;
        std::string password;
        const std::string candidate = random_ascii(rng, 256);
        must_not_escape([&] {
            (void)rpc::parse_basic_auth(candidate, user, password);
        }, "parse_basic_auth must not throw");
    }
}

void fuzz_rpc_json_parser() {
    XorShift64 rng(0x67656f636f696e05ULL);
    for (size_t i = 0; i < 2500; ++i) {
        const std::string candidate = random_ascii(rng, 1024);
        must_not_escape([&] {
            try {
                (void)rpc::parse_json_rpc_payload(candidate);
            } catch (const rpc::RPCException&) {
                // Syntax and policy rejection are the parser's public result.
            }
        }, "strict RPC JSON parser must only return or throw RPCException");
    }
}

void fuzz_raw_transaction_parser() {
    XorShift64 rng(0x67656f636f696e04ULL);
    for (size_t i = 0; i < 1000; ++i) {
        const bytes data = random_bytes(rng, 384);
        try {
            (void)core::Transaction::canonical_hash_from_serialized(data);
        } catch (const std::exception&) {
        }

        try {
            size_t offset = 0;
            (void)core::Transaction::deserialize(data, offset);
            check(offset <= data.size(), "transaction parser offset must not exceed input size");
        } catch (const std::exception&) {
        }
    }
}

void test_block_transaction_count_allocation_guard() {
    core::BlockHeader header;
    bytes encoded = header.serialize();
    const bytes count = utils::encode_varint(
        consensus::params::MAX_BLOCK_TRANSACTIONS + 1
    );
    encoded.insert(encoded.end(), count.begin(), count.end());

    bool rejected = false;
    try {
        (void)core::Block::deserialize(encoded);
    } catch (const std::exception&) {
        rejected = true;
    }
    check(rejected, "block parser rejects an allocation-amplifying transaction count");
}

// A permitted transaction count with no transaction bytes behind it must not
// size the transaction buffer: each Transaction object is kilobytes in memory.
void test_block_unbacked_transaction_count_is_not_reserved() {
    core::BlockHeader header;
    bytes encoded = header.serialize();
    const bytes count = utils::encode_varint(consensus::params::MAX_BLOCK_TRANSACTIONS);
    encoded.insert(encoded.end(), count.begin(), count.end());

    largest_allocation.store(0);
    track_allocations.store(true);
    bool rejected = false;
    try {
        (void)core::Block::deserialize(encoded);
    } catch (const std::exception&) {
        rejected = true;
    }
    track_allocations.store(false);
    check(rejected, "block parser rejects a transaction count without transaction bytes");
    check(largest_allocation.load() < consensus::params::MAX_BLOCK_SIZE,
          "an unbacked transaction count does not size the transaction buffer");
}

void test_block_size_guard() {
    const bytes oversized(consensus::params::MAX_BLOCK_SIZE + 1, 0);

    bool rejected = false;
    try {
        (void)core::Block::deserialize(oversized);
    } catch (const std::exception&) {
        rejected = true;
    }
    check(rejected, "block parser rejects input over the consensus size limit");
}

} // namespace

int main() {
    fuzz_p2p_framing();
    fuzz_p2p_payload_parsers();
    fuzz_rpc_basic_auth();
    fuzz_rpc_json_parser();
    fuzz_raw_transaction_parser();
    test_block_transaction_count_allocation_guard();
    test_block_unbacked_transaction_count_is_not_reserved();
    test_block_size_guard();

    if (failures == 0) {
        std::cout << "Parser fuzz smoke tests passed.\n";
        return 0;
    }

    std::cerr << failures << " parser fuzz failure(s).\n";
    return 1;
}
