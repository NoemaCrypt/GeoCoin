#include "geocoin/network/messages.hpp"

#include <cstdlib>
#include <exception>
#include <new>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    using namespace geocoin;
    if (size == 0) return 0;
    const bytes payload(data + 1, data + size);
    if (data[0] % 7 == 0) {
        // The framing API promises failure without throwing.
        network::MessageHeader header;
        bytes body;
        if (network::parse_message(payload, header, body) &&
            (!network::is_known_message_type(header.type) || body.size() != header.length)) std::abort();
        return 0;
    }
    try {
        switch (data[0] % 7) {
        case 1: (void)network::VersionMessage::deserialize(payload); break;
        case 2: (void)network::AddrMessage::deserialize(payload); break;
        case 3: (void)network::InvMessage::deserialize(payload); break;
        case 4: (void)network::HeadersMessage::deserialize(payload); break;
        case 5: (void)network::ChallengeMessage::deserialize(payload); break;
        case 6: (void)network::ResponseMessage::deserialize(payload); break;
        }
    } catch (const std::bad_alloc&) {
        throw;
    } catch (const std::exception&) {
    }
    return 0;
}
