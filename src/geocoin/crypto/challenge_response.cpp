#include "geocoin/crypto/challenge_response.hpp"
#include "geocoin/crypto/geo_area.hpp"
#include "geocoin/crypto/signatures.hpp"

#include <openssl/rand.h>
#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <chrono>

namespace geocoin::crypto {

namespace {

constexpr size_t kSessionIdSize = 16;
constexpr size_t kChallengeValueSize = 32;
constexpr size_t kChannelBindingSize = 32;

Hash256 hash_challenge(const ChallengeData& challenge) {
    if (challenge.value.size() != kChallengeValueSize) {
        throw std::invalid_argument("Challenge value must be 32 bytes");
    }
    if (challenge.session_id.size() != kSessionIdSize) {
        throw std::invalid_argument("Invalid session_id size");
    }

    bytes data;
    data.reserve(24 + kChallengeValueSize + 8 + kSessionIdSize + kChannelBindingSize);

    static constexpr char kDomain[] = "GEOCOIN-FALCON-AUTH-v2";
    data.insert(data.end(), kDomain, kDomain + sizeof(kDomain) - 1);
    data.insert(data.end(), challenge.value.begin(), challenge.value.end());

    for (int i = 0; i < 8; ++i) {
        data.push_back(static_cast<byte>((challenge.timestamp >> (i * 8)) & 0xFF));
    }

    data.insert(data.end(), challenge.session_id.begin(), challenge.session_id.end());
    data.insert(
        data.end(), challenge.channel_binding.begin(), challenge.channel_binding.end());
    return sha256(data);
}

}

ChallengeData create_challenge(const Hash256& channel_binding) {
    ChallengeData challenge;
    challenge.value.resize(kChallengeValueSize);
    challenge.session_id.resize(kSessionIdSize);

    if (RAND_bytes(challenge.value.data(), static_cast<int>(challenge.value.size())) != 1) {
        throw std::runtime_error("Failed to generate challenge value");
    }
    if (RAND_bytes(challenge.session_id.data(), static_cast<int>(challenge.session_id.size())) != 1) {
        throw std::runtime_error("Failed to generate session_id");
    }

    challenge.timestamp = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count()
    );
    challenge.channel_binding = channel_binding;

    challenge.nonce = challenge.session_id;
    challenge.rounds = {challenge.session_id, challenge.session_id, challenge.session_id};
    return challenge;
}

ChallengeResponse respond_to_challenge(
    const bytes& private_key,
    const ChallengeData& challenge
) {
    if (private_key.size() != 32) {
        throw std::invalid_argument("Private key must be 32 bytes");
    }

    Hash256 msg32 = hash_challenge(challenge);

    ChallengeResponse response;
    response.proof = sign_recoverable_message(msg32, private_key);
    response.responses = {response.proof, response.proof, response.proof};
    return response;
}

bool verify_challenge_response(
    const ChallengeResponse& response,
    const Address& expected_address,
    const ChallengeData& challenge
) {
    if (response.proof.empty() || expected_address.empty()) {
        return false;
    }

    Hash256 msg32;
    try {
        msg32 = hash_challenge(challenge);
    } catch (...) {
        return false;
    }

    Address recovered;
    if (!recover_signer_address_from_signed_message(msg32, response.proof, recovered)) {
        return false;
    }

    return recovered == expected_address;
}

MultiRoundAuth::MultiRoundAuth(size_t num_rounds)
    : num_rounds_(num_rounds)
    , current_round_(0)
    , completed_(false)
{
    if (num_rounds_ < 1 || num_rounds_ > 10) {
        throw std::invalid_argument("Number of rounds must be between 1 and 10");
    }
}

ChallengeData MultiRoundAuth::start_round() {
    if (completed_) {
        throw std::runtime_error("Authentication already completed");
    }

    if (current_round_ >= num_rounds_) {
        throw std::runtime_error("All rounds completed");
    }
    if (challenges_.size() != responses_.size()) {
        throw std::runtime_error("Previous authentication round is still pending");
    }

    ChallengeData challenge = create_challenge();
    challenges_.push_back(challenge);
    current_round_++;

    return challenge;
}

bool MultiRoundAuth::verify_round(
    const ChallengeResponse& response,
    const Address& expected_address
) {
    if (challenges_.empty() || responses_.size() >= challenges_.size()) {
        return false;
    }

    const auto& challenge = challenges_.back();
    bool valid = verify_challenge_response(response, expected_address, challenge);

    if (valid) {
        responses_.push_back(response);
    }

    return valid;
}

bool MultiRoundAuth::is_complete() const {
    return current_round_ >= num_rounds_ && responses_.size() == num_rounds_;
}

bool MultiRoundAuth::finalize() {
    if (!is_complete()) {
        return false;
    }

    completed_ = true;
    return true;
}

void MultiRoundAuth::reset() {
    current_round_ = 0;
    completed_ = false;
    challenges_.clear();
    responses_.clear();
}

double MultiRoundAuth::get_security_bits() const {
    return 128.0;
}

bytes serialize_challenge(const ChallengeData& challenge) {
    if (challenge.session_id.size() != kSessionIdSize) {
        throw std::invalid_argument("Invalid session_id size");
    }
    if (challenge.value.size() != kChallengeValueSize) {
        throw std::invalid_argument("Challenge value must be 32 bytes");
    }

    bytes result;
    result.reserve(kChallengeValueSize + 8 + kSessionIdSize + kChannelBindingSize);

    result.insert(result.end(), challenge.value.begin(), challenge.value.end());

    for (int i = 0; i < 8; ++i) {
        result.push_back(static_cast<byte>((challenge.timestamp >> (i * 8)) & 0xFF));
    }

    result.insert(result.end(), challenge.session_id.begin(), challenge.session_id.end());
    result.insert(
        result.end(), challenge.channel_binding.begin(), challenge.channel_binding.end());
    return result;
}

ChallengeData deserialize_challenge(const bytes& data) {
    if (data.size() != kChallengeValueSize + 8 + kSessionIdSize + kChannelBindingSize) {
        throw std::invalid_argument("Invalid challenge data size");
    }

    ChallengeData challenge;
    challenge.value.assign(data.begin(), data.begin() + kChallengeValueSize);

    challenge.timestamp = 0;
    for (int i = 0; i < 8; ++i) {
        challenge.timestamp |= static_cast<uint64_t>(data[kChallengeValueSize + i]) << (i * 8);
    }

    challenge.session_id.assign(
        data.begin() + kChallengeValueSize + 8,
        data.begin() + kChallengeValueSize + 8 + kSessionIdSize
    );
    std::copy_n(
        data.begin() + kChallengeValueSize + 8 + kSessionIdSize,
        kChannelBindingSize,
        challenge.channel_binding.begin()
    );
    challenge.nonce = challenge.session_id;
    challenge.rounds = {challenge.session_id, challenge.session_id, challenge.session_id};
    return challenge;
}

bytes serialize_response(const ChallengeResponse& response) {
    bytes result;
    result.reserve(4 + response.proof.size());

    uint32_t proof_len = static_cast<uint32_t>(response.proof.size());
    for (int i = 0; i < 4; ++i) {
        result.push_back(static_cast<byte>((proof_len >> (i * 8)) & 0xFF));
    }

    result.insert(result.end(), response.proof.begin(), response.proof.end());
    return result;
}

ChallengeResponse deserialize_response(const bytes& data) {
    if (data.size() < 4) {
        throw std::invalid_argument("Invalid response data size");
    }

    ChallengeResponse response;
    size_t offset = 0;

    uint32_t proof_len = 0;
    for (int i = 0; i < 4; ++i) {
        proof_len |= static_cast<uint32_t>(data[offset++]) << (i * 8);
    }

    if (offset + proof_len != data.size() || proof_len == 0) {
        throw std::invalid_argument("Invalid response proof size");
    }

    response.proof.assign(data.begin() + offset, data.end());
    response.responses = {response.proof, response.proof, response.proof};
    return response;
}

} // namespace geocoin::crypto
