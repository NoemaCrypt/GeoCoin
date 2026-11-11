#include "geocoin/network/messages.hpp"
#include "geocoin/network/services.hpp"
#include <array>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace geocoin;

namespace {
int failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

Hash256 filled_hash(byte seed) {
    Hash256 hash{};
    for (size_t i = 0; i < hash.size(); ++i) {
        hash[i] = static_cast<byte>(seed + static_cast<byte>(i));
    }
    return hash;
}

void write_le32(bytes& out, uint32_t value) {
    out.push_back(static_cast<byte>(value & 0xff));
    out.push_back(static_cast<byte>((value >> 8) & 0xff));
    out.push_back(static_cast<byte>((value >> 16) & 0xff));
    out.push_back(static_cast<byte>((value >> 24) & 0xff));
}

void write_be64(bytes& out, uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8) {
        out.push_back(static_cast<byte>((value >> shift) & 0xff));
    }
}

void append_hash(bytes& out, const Hash256& hash) {
    out.insert(out.end(), hash.begin(), hash.end());
}

void set_le32(bytes& out, size_t offset, uint32_t value) {
    out.at(offset) = static_cast<byte>(value & 0xff);
    out.at(offset + 1) = static_cast<byte>((value >> 8) & 0xff);
    out.at(offset + 2) = static_cast<byte>((value >> 16) & 0xff);
    out.at(offset + 3) = static_cast<byte>((value >> 24) & 0xff);
}

template <typename Fn>
void check_throws(Fn&& fn, const char* message) {
    try {
        fn();
    } catch (const std::exception&) {
        return;
    }
    check(false, message);
}

void test_service_flags() {
    const uint64_t pruned_services = network::build_local_services(false);
    check(network::has_service(pruned_services, network::ServiceFlags::NODE_NETWORK), "pruned services include network");
    check(network::has_service(pruned_services, network::ServiceFlags::NODE_WITNESS), "pruned services include witness");
    check(network::has_service(pruned_services, network::ServiceFlags::NODE_NETWORK_LIMITED), "pruned services include limited");
    check(!network::has_service(pruned_services, network::ServiceFlags::NODE_ARCHIVE), "pruned services exclude archive");

    const uint64_t archive_services = network::build_local_services(true);
    check(network::has_service(archive_services, network::ServiceFlags::NODE_NETWORK), "archive services include network");
    check(network::has_service(archive_services, network::ServiceFlags::NODE_WITNESS), "archive services include witness");
    check(network::has_service(archive_services, network::ServiceFlags::NODE_ARCHIVE), "archive services include archive");
    check(!network::has_service(archive_services, network::ServiceFlags::NODE_NETWORK_LIMITED), "archive services exclude limited");
}

void test_unknown_message_type_is_rejected() {
    check(!network::is_known_message_type(static_cast<network::MessageType>(0xff)),
        "unknown message type must not be accepted");

    bytes raw_header{
        0x00, 0x00, 0x67, 0xd3, // mainnet magic, little-endian
        0xff,                   // unknown message type
        0x00, 0x00, 0x00, 0x00, // zero payload length
        0x00, 0x00, 0x00, 0x00  // checksum bytes
    };

    network::MessageHeader header;
    bytes payload;
    check(!network::parse_message(raw_header, header, payload),
        "parse_message must reject unknown message type");
}

void test_message_framing_policy() {
    const bytes payload{0x01, 0x02, 0x03};
    bytes framed = network::create_message(network::MessageType::PING, payload, network::MessageHeader::MAGIC_MAINNET);
    network::MessageHeader header;
    bytes parsed_payload;
    check(network::parse_message(framed, header, parsed_payload), "valid framed message parses");
    check(parsed_payload == payload, "valid framed message preserves payload");

    bytes wrong_magic = framed;
    wrong_magic[0] ^= 0xff;
    check(!network::parse_message(wrong_magic, header, parsed_payload), "wrong network magic must be rejected");

    bytes bad_checksum = framed;
    bad_checksum[9] ^= 0xff;
    check(!network::parse_message(bad_checksum, header, parsed_payload), "bad message checksum must be rejected");

    bytes truncated_payload = framed;
    truncated_payload.pop_back();
    check(!network::parse_message(truncated_payload, header, parsed_payload),
          "declared payload length larger than available bytes must be rejected");

    bytes mismatched_length = framed;
    set_le32(mismatched_length, 5, static_cast<uint32_t>(payload.size() + 1));
    check(!network::parse_message(mismatched_length, header, parsed_payload),
          "tampered payload length must be rejected before checksum acceptance");

    network::MessageHeader oversized_header;
    oversized_header.magic = network::MessageHeader::MAGIC_MAINNET;
    oversized_header.type = network::MessageType::PING;
    oversized_header.length = (4 * 1024 * 1024) + 1;
    oversized_header.checksum.fill(0);
    check(!network::parse_message(oversized_header.serialize(), header, parsed_payload),
          "oversized declared payload length must be rejected while parsing");

    check_throws(
        [] {
            network::create_message(
                network::MessageType::PING,
                bytes((4 * 1024 * 1024) + 1, 0x00),
                network::MessageHeader::MAGIC_MAINNET
            );
        },
        "oversized message payload must be rejected while framing"
    );
}

void test_inventory_and_address_limits() {
    bytes too_many_inv_items;
    write_le32(too_many_inv_items, 50001);
    check_throws(
        [&too_many_inv_items] {
            (void)network::InvMessage::deserialize(too_many_inv_items);
        },
        "INV deserialize must reject excessive item count before allocation"
    );

    network::InvMessage inv;
    inv.items.resize(50001);
    check_throws(
        [&inv] {
            (void)inv.serialize();
        },
        "INV serialize must reject excessive item count"
    );

    network::InvMessage duplicate_inv;
    network::InvMessage::InvItem duplicate_item;
    duplicate_item.type = network::InvMessage::InvType::BLOCK;
    duplicate_item.hash.fill(0x42);
    duplicate_inv.items = {duplicate_item, duplicate_item};
    check_throws(
        [&duplicate_inv] { (void)duplicate_inv.serialize(); },
        "INV serialize must reject duplicate inventory amplification"
    );

    bytes too_many_addresses;
    write_le32(too_many_addresses, 1001);
    check_throws(
        [&too_many_addresses] {
            (void)network::AddrMessage::deserialize(too_many_addresses);
        },
        "ADDR deserialize must reject excessive address count before allocation"
    );

    network::AddrMessage addr;
    addr.addresses.resize(1001);
    check_throws(
        [&addr] {
            (void)addr.serialize();
        },
        "ADDR serialize must reject excessive address count"
    );
}

void test_header_locator_limits() {
    network::HeadersMessage too_many_headers;
    too_many_headers.headers.resize(network::kMaxHeadersPerMessage + 1);
    check_throws(
        [&too_many_headers] {
            (void)too_many_headers.serialize();
        },
        "HEADERS serialize must reject excessive header count"
    );

    bytes excessive_header_count;
    write_le32(excessive_header_count, network::kMaxHeadersPerMessage + 1);
    check_throws(
        [&excessive_header_count] {
            (void)network::HeadersMessage::deserialize(excessive_header_count);
        },
        "HEADERS deserialize must reject excessive header count before allocation"
    );

    network::GetBlocksMessage too_many_locators;
    too_many_locators.version = 1;
    too_many_locators.locator_hashes.resize(network::kMaxHeadersPerMessage + 1);
    check_throws(
        [&too_many_locators] {
            (void)too_many_locators.serialize();
        },
        "GETBLOCKS serialize must reject excessive locator count"
    );

    bytes excessive_locator_count;
    write_le32(excessive_locator_count, 1);
    write_le32(excessive_locator_count, network::kMaxHeadersPerMessage + 1);
    check_throws(
        [&excessive_locator_count] {
            (void)network::GetBlocksMessage::deserialize(excessive_locator_count);
        },
        "GETBLOCKS deserialize must reject excessive locator count before allocation"
    );
}

void test_unsigned_integer_decoding() {
    network::MessageHeader header;
    header.magic = 0x806700ffU;
    header.type = network::MessageType::PING;
    header.length = 0;
    header.checksum.fill(0);
    const network::MessageHeader parsed_header = network::MessageHeader::deserialize(header.serialize());
    check(parsed_header.magic == header.magic, "MessageHeader must decode high-bit uint32 fields as unsigned");

    network::VersionMessage version;
    version.version = 0x80000001U;
    version.services = 0x8000000000000001ULL;
    version.timestamp = 0x8000000000000002ULL;
    version.user_agent = "degree-test";
    version.height = 0x8000000200000003ULL;
    version.auth_address = Address("GEO1KrNQJ6C6qYNH4N2goZPU2eDAfiPvknY2x");
    const network::VersionMessage parsed_version = network::VersionMessage::deserialize(version.serialize());
    check(parsed_version.version == version.version, "VersionMessage must decode high-bit uint32 version");
    check(parsed_version.height == version.height, "VersionMessage must decode full uint64 height");

    network::AddrMessage addr;
    addr.addresses.push_back(network::AddrMessage::NetAddr{
        0x8000000000000003ULL,
        "127.0.0.1",
        0x80ffU,
        0x8000000000000004ULL
    });
    const network::AddrMessage parsed_addr = network::AddrMessage::deserialize(addr.serialize());
    check(parsed_addr.addresses.size() == 1, "AddrMessage high-bit port roundtrip count");
    if (!parsed_addr.addresses.empty()) {
        check(parsed_addr.addresses.front().port == 0x80ffU, "AddrMessage must decode high-bit uint16 port");
    }
}

void test_auth_message_size_limits() {
    network::ChallengeMessage invalid_challenge;
    invalid_challenge.challenge.value.resize(31);
    invalid_challenge.challenge.session_id.resize(16);
    check_throws(
        [&invalid_challenge] {
            (void)invalid_challenge.serialize();
        },
        "CHALLENGE serialize must require exact 32-byte challenge value"
    );

    bytes bad_challenge_payload;
    write_le32(bad_challenge_payload, 33);
    bad_challenge_payload.resize(4 + 33 + 8 + 16, 0x11);
    check_throws(
        [&bad_challenge_payload] {
            (void)network::ChallengeMessage::deserialize(bad_challenge_payload);
        },
        "CHALLENGE deserialize must reject non-32-byte challenge values before auth"
    );

    network::ResponseMessage oversized_response;
    oversized_response.response.proof.resize(4097);
    check_throws(
        [&oversized_response] {
            (void)oversized_response.serialize();
        },
        "RESPONSE serialize must reject oversized auth proof"
    );

    bytes bad_response_payload;
    write_le32(bad_response_payload, 4097);
    bad_response_payload.resize(4 + 4097, 0x22);
    check_throws(
        [&bad_response_payload] {
            (void)network::ResponseMessage::deserialize(bad_response_payload);
        },
        "RESPONSE deserialize must reject oversized auth proof before crypto verification"
    );
}

void test_message_deserialize_strictness() {
    check_throws(
        [] {
            (void)network::create_message(
                static_cast<network::MessageType>(0xfe),
                bytes(),
                network::MessageHeader::MAGIC_MAINNET
            );
        },
        "create_message must reject unknown message types"
    );

    network::MessageHeader parsed_header;
    bytes parsed_payload;
    check(!network::parse_message(bytes{0x01, 0x02}, parsed_header, parsed_payload),
          "parse_message rejects frames shorter than header");

    network::VersionMessage version;
    version.version = 1;
    version.services = network::build_local_services(true);
    version.timestamp = 123;
    version.user_agent = "degree/strict";
    version.height = 9;
    version.auth_address = Address("GEO1KrNQJ6C6qYNH4N2goZPU2eDAfiPvknY2x");
    bytes version_payload = version.serialize();
    version_payload.push_back(0x00);
    check_throws(
        [&version_payload] {
            (void)network::VersionMessage::deserialize(version_payload);
        },
        "VERSION deserialize rejects trailing bytes"
    );
    version.user_agent.assign(4097, 'x');
    check_throws(
        [&version] {
            (void)version.serialize();
        },
        "VERSION serialize rejects oversized user-agent strings"
    );

    network::ChallengeMessage challenge;
    challenge.challenge.value.assign(32, 0x11);
    challenge.challenge.session_id.assign(15, 0x22);
    check_throws(
        [&challenge] {
            (void)challenge.serialize();
        },
        "CHALLENGE serialize must require exact 16-byte session id"
    );
    bytes short_challenge;
    write_le32(short_challenge, 32);
    short_challenge.resize(4 + 32 + 7, 0x33);
    check_throws(
        [&short_challenge] {
            (void)network::ChallengeMessage::deserialize(short_challenge);
        },
        "CHALLENGE deserialize rejects missing timestamp/session bytes"
    );
    network::ChallengeMessage full_challenge;
    full_challenge.challenge.value.assign(32, 0x21);
    full_challenge.challenge.session_id.assign(16, 0x22);
    full_challenge.challenge.timestamp = 123456;
    bytes trailing_challenge = full_challenge.serialize();
    trailing_challenge.push_back(0x23);
    check_throws(
        [&trailing_challenge] {
            (void)network::ChallengeMessage::deserialize(trailing_challenge);
        },
        "CHALLENGE deserialize rejects trailing bytes"
    );

    network::ResponseMessage empty_response;
    check_throws(
        [&empty_response] {
            (void)empty_response.serialize();
        },
        "RESPONSE serialize rejects empty proof"
    );
    bytes short_response;
    write_le32(short_response, 8);
    short_response.resize(4 + 3, 0x44);
    check_throws(
        [&short_response] {
            (void)network::ResponseMessage::deserialize(short_response);
        },
        "RESPONSE deserialize rejects proof underflow"
    );
    network::ResponseMessage full_response;
    full_response.response.proof.assign(8, 0x31);
    bytes trailing_response = full_response.serialize();
    trailing_response.push_back(0x32);
    check_throws(
        [&trailing_response] {
            (void)network::ResponseMessage::deserialize(trailing_response);
        },
        "RESPONSE deserialize rejects trailing bytes"
    );

    bytes inv_trailing;
    write_le32(inv_trailing, 0);
    inv_trailing.push_back(0xff);
    check_throws(
        [&inv_trailing] {
            (void)network::InvMessage::deserialize(inv_trailing);
        },
        "INV deserialize rejects trailing bytes"
    );
    bytes inv_truncated;
    write_le32(inv_truncated, 1);
    write_le32(inv_truncated, static_cast<uint32_t>(network::InvMessage::InvType::TX));
    inv_truncated.resize(inv_truncated.size() + 10, 0xaa);
    check_throws(
        [&inv_truncated] {
            (void)network::InvMessage::deserialize(inv_truncated);
        },
        "INV deserialize rejects truncated inventory hash"
    );

    bytes getblocks_truncated;
    write_le32(getblocks_truncated, 1);
    write_le32(getblocks_truncated, 0);
    getblocks_truncated.resize(getblocks_truncated.size() + 31, 0x55);
    check_throws(
        [&getblocks_truncated] {
            (void)network::GetBlocksMessage::deserialize(getblocks_truncated);
        },
        "GETBLOCKS deserialize rejects truncated stop hash"
    );
    bytes getblocks_one_locator;
    write_le32(getblocks_one_locator, 1);
    write_le32(getblocks_one_locator, 1);
    append_hash(getblocks_one_locator, filled_hash(0x51));
    append_hash(getblocks_one_locator, filled_hash(0x61));
    const network::GetBlocksMessage parsed_getblocks =
        network::GetBlocksMessage::deserialize(getblocks_one_locator);
    check(parsed_getblocks.locator_hashes.size() == 1, "GETBLOCKS deserialize reads locator hashes");
    if (!parsed_getblocks.locator_hashes.empty()) {
        check(parsed_getblocks.locator_hashes.front() == filled_hash(0x51),
              "GETBLOCKS locator hash bytes roundtrip");
    }
    check(parsed_getblocks.stop_hash == filled_hash(0x61), "GETBLOCKS stop hash bytes roundtrip");

    bytes headers_bad_size;
    write_le32(headers_bad_size, 1);
    headers_bad_size.resize(headers_bad_size.size() + 91, 0x66);
    check_throws(
        [&headers_bad_size] {
            (void)network::HeadersMessage::deserialize(headers_bad_size);
        },
        "HEADERS deserialize rejects invalid byte count"
    );
    bytes legacy_headers;
    write_le32(legacy_headers, 1);
    write_le32(legacy_headers, 3);
    write_le32(legacy_headers, 999);
    append_hash(legacy_headers, filled_hash(0x71));
    append_hash(legacy_headers, filled_hash(0x81));
    write_le32(legacy_headers, 0x1d00ffff);
    write_be64(legacy_headers, 0x0102030405060708ULL);
    write_be64(legacy_headers, 0x1112131415161718ULL);
    check_throws(
        [&legacy_headers] {
            (void)network::HeadersMessage::deserialize(legacy_headers);
        },
        "HEADERS rejects obsolete encoding without witness commitment"
    );

    network::AddrMessage addr;
    addr.addresses.push_back({network::build_local_services(false), std::string(4097, '1'), 3030, 1});
    check_throws(
        [&addr] {
            (void)addr.serialize();
        },
        "ADDR serialize rejects oversized IP string"
    );
    bytes addr_trailing;
    write_le32(addr_trailing, 0);
    addr_trailing.push_back(0x77);
    check_throws(
        [&addr_trailing] {
            (void)network::AddrMessage::deserialize(addr_trailing);
        },
        "ADDR deserialize rejects trailing bytes"
    );

    network::RejectMessage reject;
    reject.reason.assign(257, 'r');
    check_throws(
        [&reject] {
            (void)reject.serialize();
        },
        "REJECT serialize rejects oversized reason"
    );
    reject.reason = "bad";
    bytes reject_payload = reject.serialize();
    reject_payload.push_back(0x88);
    check_throws(
        [&reject_payload] {
            (void)network::RejectMessage::deserialize(reject_payload);
        },
        "REJECT deserialize rejects trailing bytes"
    );
    bytes long_reject_reason;
    long_reject_reason.push_back(static_cast<byte>(network::MessageType::TX));
    long_reject_reason.push_back(1);
    write_le32(long_reject_reason, 257);
    long_reject_reason.resize(long_reject_reason.size() + 257, 'r');
    append_hash(long_reject_reason, filled_hash(0x91));
    check_throws(
        [&long_reject_reason] {
            (void)network::RejectMessage::deserialize(long_reject_reason);
        },
        "REJECT deserialize rejects oversized reason"
    );

    const core::Transaction coinbase = core::Transaction::create_coinbase(
        7,
        Address("GEOmessage-tx-roundtrip"),
        50 * COIN
    );
    network::TxMessage tx_message;
    tx_message.tx = coinbase;
    const network::TxMessage parsed_tx = network::TxMessage::deserialize(tx_message.serialize());
    check(parsed_tx.tx.get_hash() == coinbase.get_hash(), "TX message deserialize roundtrips transaction");

    core::Block block;
    block.header.version = 1;
    block.header.timestamp = 1234;
    block.header.bits = 0x1d00ffff;
    block.header.nonce = 42;
    block.transactions.push_back(coinbase);
    block.header.merkle_root = block.calculate_merkle_root();
    block.header.witness_commitment = block.calculate_witness_commitment();
    network::BlockMessage block_message;
    block_message.block = block;
    const network::BlockMessage parsed_block =
        network::BlockMessage::deserialize(block_message.serialize());
    check(parsed_block.block.get_hash() == block.get_hash(), "BLOCK message deserialize roundtrips block");
}
} // namespace

int main() {
    test_service_flags();
    test_unknown_message_type_is_rejected();
    test_message_framing_policy();
    test_inventory_and_address_limits();
    test_header_locator_limits();
    test_unsigned_integer_decoding();
    test_auth_message_size_limits();
    test_message_deserialize_strictness();

    core::BlockHeader header;
    header.version = 7;
    header.timestamp = 1234567890;
    header.prev_block_hash = filled_hash(0x10);
    header.merkle_root = filled_hash(0x40);
    header.bits = 0x1d00ffff;
    header.geo_tag = 0x0102030405060708ULL;
    header.witness_commitment = filled_hash(0x80);
    header.nonce = 0x1112131415161718ULL;

    network::HeadersMessage msg;
    msg.headers.push_back(header);

    const bytes serialized = msg.serialize();
    check(serialized.size() == 4 + header.serialize().size(), "HEADERS uses canonical BlockHeader size");

    const network::HeadersMessage roundtrip = network::HeadersMessage::deserialize(serialized);
    check(roundtrip.headers.size() == 1, "HEADERS roundtrip count");
    if (!roundtrip.headers.empty()) {
        const auto& parsed = roundtrip.headers.front();
        check(parsed.witness_commitment == header.witness_commitment, "HEADERS preserves witness_commitment");
        check(parsed.get_hash() == header.get_hash(), "HEADERS preserves canonical header hash");
        check(parsed.serialize() == header.serialize(), "HEADERS preserves canonical header bytes");
    }

    if (failures == 0) {
        std::cout << "Headers message tests passed.\n";
        return 0;
    }

    std::cerr << failures << " test(s) failed.\n";
    return 1;
}
