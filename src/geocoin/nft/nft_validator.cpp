#include "geocoin/nft/nft_validator.hpp"
#include "geocoin/crypto/addresses.hpp"
#include "geocoin/crypto/geo_area.hpp"
#include "geocoin/crypto/signatures.hpp"
#include "geocoin/consensus/consensus.hpp"
#include "geocoin/nft/geodesy.hpp"
#include "geocoin/geography/wgs84.hpp"
#include "geocoin/utils/logger.hpp"
#include "geocoin/utils/int128.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

namespace geocoin::nft {

namespace {
using WideInteger = int128_t;

bool is_hex_sha256(const std::string& s) {
    if (s.size() != 64) {
        return false;
    }
    for (char c : s) {
        const bool is_digit = (c >= '0' && c <= '9');
        const bool is_lower = (c >= 'a' && c <= 'f');
        const bool is_upper = (c >= 'A' && c <= 'F');
        if (!is_digit && !is_lower && !is_upper) {
            return false;
        }
    }
    return true;
}

bool validate_cadastral_geometry(const NFTMetadata& metadata) {
    if (!metadata.cadastral) {
        return true;
    }

    const auto& cadastral = *metadata.cadastral;
    if (cadastral.cadastral_number.empty()) {
        GEO_LOG_ERROR("nft", "Cadastral number is required");
        return false;
    }
    if (cadastral.owner_name.empty()) {
        GEO_LOG_ERROR("nft", "Owner name is required");
        return false;
    }
    if (cadastral.area_sqcm_official == 0) {
        GEO_LOG_ERROR("nft", "Official cadastral area must be > 0");
        return false;
    }

    // Official areas are serialized as square centimetres. Quantize the
    // derived WGS84 area to the same unit before the exact integer comparison.
    // The 10% allowance is GeoCoin policy, not a cadastral survey standard.
    const auto measured_sqcm = coordinates::area_square_centimetres(metadata.geometry).value_or(0);
    if (measured_sqcm == 0) {
        GEO_LOG_ERROR("nft", "WGS84 geodesic area is zero or invalid");
        return false;
    }
    const WideInteger measured = measured_sqcm;
    const WideInteger official = cadastral.area_sqcm_official;
    const WideInteger difference = measured > official ? measured - official : official - measured;
    if (difference * 100 > official * consensus::params::NFT_CADASTRAL_AREA_TOLERANCE_PERCENT) {
        GEO_LOG_ERROR("nft", "Official cadastral area mismatch");
        return false;
    }
    return true;
}

bool cadastral_slot_available(
    const NFTMetadata& metadata,
    const NFTRegistryView& registry
) {
    if (!metadata.cadastral) {
        return true;
    }

    const coordinates::PreparedGeometry prepared(metadata.geometry);
    return registry.for_each_cadastral_candidate(metadata, [&](const Hash256& id, const NFTMetadata& existing) {
        return existing.cadastral->cadastral_number != metadata.cadastral->cadastral_number &&
               !prepared.overlaps(*registry.prepared_geometry(id));
    });
}
} // namespace

bool metadata_within_consensus_limits(const NFTMetadata& metadata) {
    if (metadata.name.size() > consensus::params::MAX_NFT_NAME ||
        metadata.description.size() > consensus::params::MAX_NFT_DESCRIPTION ||
        metadata.image_uri.size() > consensus::params::MAX_NFT_IMAGE_URI ||
        metadata.location_name.size() > consensus::params::MAX_NFT_LOCATION_NAME ||
        metadata.creator.size() > consensus::params::MAX_ADDRESS_SIZE) {
        GEO_LOG_ERROR("nft", "NFT metadata fields exceed size limits");
        return false;
    }
    if (metadata.tags.size() > consensus::params::MAX_NFT_TAGS) {
        GEO_LOG_ERROR("nft", "Too many NFT tags");
        return false;
    }
    if (std::any_of(metadata.tags.begin(), metadata.tags.end(), [](const std::string& tag) {
            return tag.size() > consensus::params::MAX_NFT_TAG_LEN;
        })) {
        GEO_LOG_ERROR("nft", "NFT tag exceeds size limit");
        return false;
    }
    if (std::set<std::string>(metadata.tags.begin(), metadata.tags.end()).size() !=
        metadata.tags.size()) {
        GEO_LOG_ERROR("nft", "NFT tags must be unique");
        return false;
    }

    if (!metadata.cadastral) {
        return true;
    }

    const auto& cadastral = *metadata.cadastral;
    if (cadastral.cadastral_number.size() > consensus::params::MAX_CADASTRAL_NUMBER ||
        cadastral.owner_name.size() > consensus::params::MAX_CADASTRAL_OWNER_NAME ||
        cadastral.land_category.size() > consensus::params::MAX_LAND_CATEGORY ||
        cadastral.permitted_use.size() > consensus::params::MAX_PERMITTED_USE ||
        cadastral.photo_hashes.size() > consensus::params::MAX_PHOTO_HASHES ||
        cadastral.restrictions.size() > consensus::params::MAX_RESTRICTIONS ||
        std::any_of(
            cadastral.restrictions.begin(),
            cadastral.restrictions.end(),
            [](const std::string& restriction) {
                return restriction.size() > consensus::params::MAX_RESTRICTION_LEN;
            })) {
        GEO_LOG_ERROR("nft", "Cadastral metadata fields exceed size limits");
        return false;
    }
    if (cadastral.market_value < 0 || cadastral.cadastral_value < 0) {
        GEO_LOG_ERROR("nft", "Cadastral values must be non-negative");
        return false;
    }
    if (cadastral.has_buildings != (cadastral.building_count > 0)) {
        GEO_LOG_ERROR("nft", "Cadastral building fields are inconsistent");
        return false;
    }
    return true;
}

// ============================================================================
// Реализация NFTValidator
// ============================================================================

bool NFTValidator::validate_creation_state(const NFTCreationTransaction& tx, const NFTRegistryView& registry) {
    return !registry.exists(tx.nft_id) && cadastral_slot_available(tx.metadata, registry);
}

bool NFTValidator::validate_transfer_state(const NFTTransferTransaction& tx, const NFTRegistryView& registry) {
    return registry.exists(tx.nft_id) && registry.get_owner(tx.nft_id) == tx.from;
}

bool NFTValidator::validate_burn_state(const NFTBurnTransaction& tx, const NFTRegistryView& registry) {
    return registry.exists(tx.nft_id) && registry.get_owner(tx.nft_id) == tx.owner;
}

bool NFTValidator::validate_nft_creation(
    const NFTCreationTransaction& nft_tx,
    const NFTRegistryView& registry
) {
    if (!crypto::verify_address(nft_tx.creator_address)) {
        GEO_LOG_ERROR("nft", "NFT creator address is invalid");
        return false;
    }
    if (nft_tx.royalty_percent > 10000) {
        GEO_LOG_ERROR("nft", "Royalty percent exceeds 100%");
        return false;
    }
    if (!metadata_within_consensus_limits(nft_tx.metadata)) {
        return false;
    }

    // Все условия, при которых каноническая сериализация метаданных может быть
    // отклонена, проверены выше. NFT id должен соответствовать ровно этим
    // байтам, без альтернативного представления.
    if (nft_tx.nft_id != nft_tx.metadata.calculate_hash()) {
        GEO_LOG_ERROR("nft", "NFT id does not match the canonical metadata hash");
        return false;
    }

    // 2. Проверить что координаты образуют валидный полигон
    if (!is_valid_polygon(nft_tx.metadata)) {
        GEO_LOG_ERROR("nft", "Invalid polygon coordinates");
        return false;
    }

    // 2.1 Проверить криптографическую привязку к контенту
    if (!is_hex_sha256(nft_tx.metadata.content_hash)) {
        GEO_LOG_ERROR("nft", "Invalid content_hash (expected 64 hex chars SHA-256)");
        return false;
    }

    if (!validate_cadastral_geometry(nft_tx.metadata)) {
        return false;
    }

    // 3. Проверить что создатель заплатил комиссию
    // (уже проверена на уровне TxInput/TxOutput в основном валидаторе)

    if (!validate_creation_state(nft_tx, registry)) {
        GEO_LOG_ERROR("nft", "NFT id or cadastral slot is unavailable");
        return false;
    }

    // 4. КЛЮЧЕВОЕ: Если требуется Proof-of-Location
    if (nft_tx.pol_data.has_value()) {
        if constexpr (!consensus::params::ENABLE_NFT_POL) {
            GEO_LOG_ERROR("nft", "PoL is disabled by consensus");
            return false;
        } else {
            if (!validate_proof_of_location(nft_tx.pol_data.value(), nft_tx.metadata)) {
                GEO_LOG_ERROR("nft", "Invalid Proof-of-Location data");
                return false;
            }
        }
    }

    // Один канонический payload связывает создателя, royalty, метаданные и
    // опциональные данные местоположения. Проверка одинакова для обоих режимов.
    const bytes payload = nft_tx.serialize_for_signing();
    const Hash256 msg32 = crypto::sha256(payload);
    Address signer;
    if (!crypto::recover_signer_address_from_signed_message(msg32, nft_tx.signature, signer)) {
        GEO_LOG_ERROR("nft", "Invalid NFT creation signature");
        return false;
    }
    if (signer != nft_tx.creator_address) {
        GEO_LOG_ERROR("nft", "NFT creation signature signer mismatch");
        return false;
    }

    GEO_LOG_INFO("nft", "NFT creation validation passed");
    return true;
}

bool NFTValidator::validate_nft_transfer(
    const NFTTransferTransaction& tx,
    const NFTRegistryView& registry
) {
    if (tx.fee < 0 || !crypto::verify_address(tx.from) ||
        !crypto::verify_address(tx.to)) {
        GEO_LOG_ERROR("nft", "NFT transfer contains an invalid fee or address");
        return false;
    }

    if (!validate_transfer_state(tx, registry)) {
        GEO_LOG_ERROR("nft", "NFT is missing or sender is not its owner");
        return false;
    }

    const Hash256 msg32 = crypto::sha256(tx.serialize_for_signing());
    Address signer;
    if (!crypto::recover_signer_address_from_signed_message(msg32, tx.signature, signer) ||
        signer != tx.from) {
        GEO_LOG_ERROR("nft", "Invalid NFT transfer signature");
        return false;
    }

    GEO_LOG_INFO("nft", "NFT transfer validation passed");
    return true;
}

bool NFTValidator::validate_nft_burn(
    const NFTBurnTransaction& tx,
    const NFTRegistryView& registry
) {
    if (tx.fee < 0 || !crypto::verify_address(tx.owner)) {
        GEO_LOG_ERROR("nft", "NFT burn contains an invalid fee or owner address");
        return false;
    }

    if (!validate_burn_state(tx, registry)) {
        GEO_LOG_ERROR("nft", "NFT is missing or burn owner does not match");
        return false;
    }

    const Hash256 msg32 = crypto::sha256(tx.serialize_for_signing());
    Address signer;
    if (!crypto::recover_signer_address_from_signed_message(msg32, tx.signature, signer) ||
        signer != tx.owner) {
        GEO_LOG_ERROR("nft", "Invalid NFT burn signature");
        return false;
    }

    GEO_LOG_INFO("nft", "NFT burn validation passed");
    return true;
}

bool NFTValidator::validate_proof_of_location(
    const ProofOfLocationData& pol_data,
    const NFTMetadata& metadata
) {
    constexpr int64_t kLatitudeMin = geography::kMinLatitude;
    constexpr int64_t kLatitudeMax = geography::kMaxLatitude;
    constexpr int64_t kLongitudeMin = geography::kMinLongitude;
    constexpr int64_t kLongitudeMax = geography::kMaxLongitude;

    if (pol_data.source_type > static_cast<uint8_t>(PoLSourceType::HYBRID)) {
        GEO_LOG_ERROR("nft", "Invalid PoL source_type");
        return false;
    }
    if (pol_data.latitude_ndeg < kLatitudeMin || pol_data.latitude_ndeg > kLatitudeMax ||
        pol_data.longitude_ndeg < kLongitudeMin || pol_data.longitude_ndeg > kLongitudeMax) {
        GEO_LOG_ERROR("nft", "PoL coordinates are out of range");
        return false;
    }
    if (!validate_pol_accuracy(pol_data)) {
        return false;
    }
    if (!validate_pol_location(pol_data, metadata)) {
        return false;
    }
    if (metadata.created_at > (std::numeric_limits<uint64_t>::max)() / 1000ULL) {
        GEO_LOG_ERROR("nft", "NFT creation timestamp is out of range");
        return false;
    }
    return validate_pol_timestamp(pol_data, metadata.created_at * 1000ULL);
}

bool NFTValidator::is_valid_polygon(const NFTMetadata& metadata) {
    const auto result = coordinates::validate_geometry(coordinates::canonicalize_geometry(metadata.geometry));
    if (!result) {
        GEO_LOG_ERROR("nft", result.issue->code << ": " << result.issue->message);
        return false;
    }
    return true;
}

bool NFTValidator::validate_pol_location(
    const ProofOfLocationData& pol_data,
    const NFTMetadata& metadata
) {
    const double maximum = pol_data.gps_accuracy_cm / 100.0 + 10;
    const auto distance = coordinates::PreparedGeometry(metadata.geometry).distance_m(
        {pol_data.latitude_ndeg, pol_data.longitude_ndeg}, maximum);
    return distance && *distance <= maximum;
}

bool NFTValidator::validate_pol_timestamp(
    const ProofOfLocationData& pol_data,
    uint64_t reference_time_ms
) {
    // Проверка на будущее относительно детерминированного reference timestamp.
    if (pol_data.timestamp_ms > reference_time_ms) {
        GEO_LOG_ERROR("nft", "GPS timestamp is in the future");
        return false;
    }

    const uint64_t time_elapsed_ms = reference_time_ms - pol_data.timestamp_ms;
    const uint64_t MAX_AGE_MS = 3600000;  // 1 час

    if (time_elapsed_ms > MAX_AGE_MS) {
        GEO_LOG_ERROR("nft", "GPS timestamp too old");
        return false;
    }

    return true;
}

bool NFTValidator::validate_pol_accuracy(const ProofOfLocationData& pol_data) {
    // По умолчанию не более 100м (10000 см)
    const uint32_t MAX_ACCURACY_CM = consensus::params::MAX_NFT_POL_ACCURACY_CM;

    if (pol_data.gps_accuracy_cm == 0 || pol_data.gps_accuracy_cm > MAX_ACCURACY_CM) {
        GEO_LOG_WARN("nft", "GPS accuracy too low");
        return false;
    }

    return true;
}

// ============================================================================
// Реализация namespace coordinates
// ============================================================================

namespace coordinates {

int32_t calculate_distance_m(
    int64_t lat1_ndeg, int64_t lon1_ndeg, int64_t lat2_ndeg, int64_t lon2_ndeg
) {
    const auto distance = geodesic_distance_m({lat1_ndeg, lon1_ndeg}, {lat2_ndeg, lon2_ndeg});
    // Preserve the integer caller contract without converting invalid inputs
    // into a nearby point. All valid WGS84 distances fit comfortably in int32.
    return distance ? static_cast<int32_t>(std::llround(*distance)) :
        std::numeric_limits<int32_t>::max();
}

bool point_in_polygon(
    const std::vector<NFTMetadata::GeoPoint>& polygon,
    int64_t lat_point_ndeg, int64_t lon_point_ndeg
) {
    return locate_point(polygon, {lat_point_ndeg, lon_point_ndeg}) != PointLocation::outside;
}

bool polygons_intersect(
    const std::vector<NFTMetadata::GeoPoint>& p1,
    const std::vector<NFTMetadata::GeoPoint>& p2
) {
    return polygon_interiors_overlap(p1, p2);
}

} // namespace coordinates

} // namespace geocoin::nft
