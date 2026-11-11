#include "geocoin/consensus/consensus.hpp"
#include "geocoin/crypto/geo_area.hpp"
#include "geocoin/crypto/signatures.hpp"
#include "geocoin/nft/geodesy.hpp"
#include "geocoin/nft/nft_validator.hpp"
#include "geocoin/utils/varint.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void expect(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <typename Fn>
void expect_throws(Fn fn, const char* message)
{
    try {
        fn();
    } catch (const std::exception&) {
        return;
    }
    throw std::runtime_error(message);
}

template <typename Fn>
void expect_all_strict_prefixes_throw(
    const geocoin::bytes& encoded,
    Fn parser,
    const char* message
)
{
    for (size_t size = 0; size < encoded.size(); ++size) {
        geocoin::bytes prefix(encoded.begin(), encoded.begin() + static_cast<std::ptrdiff_t>(size));
        try {
            parser(prefix);
        } catch (const std::exception&) {
            continue;
        }
        throw std::runtime_error(
            std::string(message) + " (accepted prefix length " + std::to_string(size) + ")"
        );
    }
}

geocoin::bytes make_key(uint8_t seed)
{
    geocoin::bytes key(32);
    for (size_t i = 0; i < key.size(); ++i) {
        key[i] = static_cast<uint8_t>(seed + i);
    }
    return key;
}

geocoin::Hash256 hash_from_seed(uint8_t seed)
{
    geocoin::Hash256 hash{};
    for (size_t i = 0; i < hash.size(); ++i) {
        hash[i] = static_cast<uint8_t>(seed + i * 17);
    }
    return hash;
}

void append_u64(geocoin::bytes& data, uint64_t value)
{
    for (int i = 0; i < 8; ++i) {
        data.push_back(static_cast<uint8_t>((value >> (i * 8)) & 0xff));
    }
}

geocoin::Address address_from_key(const geocoin::bytes& key)
{
    geocoin::Address address;
    expect(geocoin::crypto::compute_address_from_private_key(key, address), "failed to derive address");
    return address;
}

std::string hex64(char c)
{
    return std::string(64, c);
}

geocoin::nft::NFTMetadata make_metadata(int32_t offset_udeg = 0)
{
    geocoin::nft::NFTMetadata metadata;
    metadata.name = "Plot";
    metadata.description = "Unit test plot";
    metadata.image_uri = "ipfs://plot";
    metadata.content_hash = hex64('a');
    metadata.geometry.polygons.front().exterior = {
        {offset_udeg, offset_udeg},
        {offset_udeg, offset_udeg + 100000},
        {offset_udeg + 100000, offset_udeg + 100000},
        {offset_udeg + 100000, offset_udeg},
    };
    for (auto& point : metadata.geometry.polygons.front().exterior) {
        point.lat_ndeg *= 1000; point.lon_ndeg *= 1000;
    }
    metadata.area_bits = 0;
    metadata.created_at = 1700000000;
    metadata.block_height = 20;
    metadata.block_hash = hash_from_seed(0x20);
    metadata.location_name = "Test location";
    metadata.tags = {"land", "test"};
    metadata.creator = "tester";
    return metadata;
}

uint64_t projected_area_sqcm(const geocoin::nft::NFTMetadata& metadata)
{
    return geocoin::nft::coordinates::area_square_centimetres(metadata.geometry).value_or(0);
}

geocoin::nft::NFTMetadata::CadastralData make_cadastral(
    const geocoin::nft::NFTMetadata& metadata,
    std::string number = "77:01:0001001:1234"
)
{
    geocoin::nft::NFTMetadata::CadastralData cadastral;
    cadastral.cadastral_number = std::move(number);
    cadastral.owner_name = "Owner";
    cadastral.land_category = "Residential";
    cadastral.permitted_use = "Testing";
    cadastral.area_sqcm_official = projected_area_sqcm(metadata);
    cadastral.ownership_document_hash = hash_from_seed(0x31);
    cadastral.technical_plan_hash = hash_from_seed(0x32);
    cadastral.photo_hashes = {hash_from_seed(0x33)};
    cadastral.market_value = geocoin::COIN;
    cadastral.cadastral_value = geocoin::COIN / 2;
    cadastral.last_valuation_date = 1700000000;
    return cadastral;
}

void sign_creation(geocoin::nft::NFTCreationTransaction& tx, const geocoin::bytes& key)
{
    tx.creator_address = address_from_key(key);
    const geocoin::bytes payload = tx.serialize_for_signing();
    tx.signature = geocoin::crypto::sign_recoverable_message(geocoin::crypto::sha256(payload), key);
}

void sign_transfer(geocoin::nft::NFTTransferTransaction& tx, const geocoin::bytes& key)
{
    tx.signature = geocoin::crypto::sign_recoverable_message(
        geocoin::crypto::sha256(tx.serialize_for_signing()), key);
}

void sign_burn(geocoin::nft::NFTBurnTransaction& tx, const geocoin::bytes& key)
{
    tx.signature = geocoin::crypto::sign_recoverable_message(
        geocoin::crypto::sha256(tx.serialize_for_signing()), key);
}

geocoin::nft::NFTCreationTransaction make_creation(
    geocoin::nft::NFTMetadata metadata,
    const geocoin::bytes& key
)
{
    geocoin::nft::NFTCreationTransaction tx;
    tx.metadata = std::move(metadata);
    tx.nft_id = tx.metadata.calculate_hash();
    tx.royalty_percent = 250;
    sign_creation(tx, key);
    return tx;
}

void test_creation_signature_and_metadata_guards()
{
    geocoin::nft::NFTRegistryImpl registry;
    const auto key = make_key(1);

    auto valid = make_creation(make_metadata(), key);
    expect(geocoin::nft::NFTValidator::validate_nft_creation(valid, registry),
           "valid signed NFT creation is accepted");

    auto wrong_signature = valid;
    wrong_signature.signature = geocoin::crypto::sign_recoverable_message(
        geocoin::crypto::sha256(wrong_signature.serialize_for_signing()),
        make_key(2)
    );
    expect(!geocoin::nft::NFTValidator::validate_nft_creation(wrong_signature, registry),
           "signature signer mismatch is rejected");

    auto empty_signature = valid;
    empty_signature.signature.clear();
    expect(!geocoin::nft::NFTValidator::validate_nft_creation(empty_signature, registry),
           "empty NFT creation signature is rejected");

    auto mismatched_id = valid;
    mismatched_id.nft_id = hash_from_seed(0xfe);
    expect(!geocoin::nft::NFTValidator::validate_nft_creation(mismatched_id, registry),
           "NFT id must equal the canonical metadata hash");

    auto excessive_royalty = valid;
    excessive_royalty.royalty_percent = 10001;
    expect(!geocoin::nft::NFTValidator::validate_nft_creation(excessive_royalty, registry),
           "NFT royalty above 100 percent is rejected by consensus validation");

    auto invalid_creator = valid;
    invalid_creator.creator_address = geocoin::Address("not-a-geocoin-address");
    expect(!geocoin::nft::NFTValidator::validate_nft_creation(invalid_creator, registry),
           "invalid NFT creator address is rejected");

    auto oversized_name_metadata = make_metadata();
    oversized_name_metadata.name.assign(geocoin::consensus::params::MAX_NFT_NAME + 1, 'x');
    auto oversized_name = make_creation(oversized_name_metadata, key);
    expect(!geocoin::nft::NFTValidator::validate_nft_creation(oversized_name, registry),
           "oversized NFT name is rejected");

    auto oversized_description_metadata = make_metadata();
    oversized_description_metadata.description.assign(
        geocoin::consensus::params::MAX_NFT_DESCRIPTION + 1, 'd');
    expect(!geocoin::nft::NFTValidator::validate_nft_creation(
               make_creation(oversized_description_metadata, key), registry),
           "oversized NFT description is rejected");

    auto oversized_image_metadata = make_metadata();
    oversized_image_metadata.image_uri.assign(geocoin::consensus::params::MAX_NFT_IMAGE_URI + 1, 'i');
    expect(!geocoin::nft::NFTValidator::validate_nft_creation(
               make_creation(oversized_image_metadata, key), registry),
           "oversized NFT image URI is rejected");

    auto oversized_location_metadata = make_metadata();
    oversized_location_metadata.location_name.assign(
        geocoin::consensus::params::MAX_NFT_LOCATION_NAME + 1, 'l');
    expect(!geocoin::nft::NFTValidator::validate_nft_creation(
               make_creation(oversized_location_metadata, key), registry),
           "oversized NFT location is rejected");

    auto oversized_creator_metadata = make_metadata();
    oversized_creator_metadata.creator.assign(geocoin::consensus::params::MAX_ADDRESS_SIZE + 1, 'c');
    expect(!geocoin::nft::NFTValidator::validate_nft_creation(
               make_creation(oversized_creator_metadata, key), registry),
           "oversized NFT metadata creator is rejected");

    auto too_many_tags_metadata = make_metadata();
    too_many_tags_metadata.tags.assign(geocoin::consensus::params::MAX_NFT_TAGS + 1, "tag");
    auto too_many_tags = make_creation(too_many_tags_metadata, key);
    expect(!geocoin::nft::NFTValidator::validate_nft_creation(too_many_tags, registry),
           "too many NFT tags are rejected");

    auto long_tag_metadata = make_metadata();
    long_tag_metadata.tags = {std::string(geocoin::consensus::params::MAX_NFT_TAG_LEN + 1, 't')};
    auto long_tag = make_creation(long_tag_metadata, key);
    expect(!geocoin::nft::NFTValidator::validate_nft_creation(long_tag, registry),
           "oversized NFT tag is rejected");

    auto duplicate_tags_metadata = make_metadata();
    duplicate_tags_metadata.tags = {"land", "land"};
    expect(!geocoin::nft::NFTValidator::validate_nft_creation(
               make_creation(duplicate_tags_metadata, key), registry),
           "duplicate NFT tags are rejected");

    auto short_content_metadata = make_metadata();
    short_content_metadata.content_hash = std::string(63, 'a');
    auto bad_content_hash = make_creation(short_content_metadata, key);
    expect(!geocoin::nft::NFTValidator::validate_nft_creation(bad_content_hash, registry),
           "short NFT content hash is rejected");

    auto non_hex_content_metadata = make_metadata();
    non_hex_content_metadata.content_hash = std::string(64, 'g');
    bad_content_hash = make_creation(non_hex_content_metadata, key);
    expect(!geocoin::nft::NFTValidator::validate_nft_creation(bad_content_hash, registry),
           "non-hex NFT content hash is rejected");

    auto mixed_hex_metadata = make_metadata(200000);
    mixed_hex_metadata.content_hash =
        "0123456789abcdefABCDEF0123456789abcdefABCDEF0123456789abcdefABCD";
    expect(geocoin::nft::NFTValidator::validate_nft_creation(
               make_creation(mixed_hex_metadata, key), registry),
           "mixed-case hexadecimal NFT content hash is accepted");

    auto unserializable = valid;
    unserializable.metadata.cadastral = make_cadastral(unserializable.metadata);
    unserializable.metadata.cadastral->market_value = -1;
    expect(!geocoin::nft::NFTValidator::validate_nft_creation(unserializable, registry),
           "metadata that cannot be encoded canonically is rejected");

    auto duplicate = valid;
    expect(registry.register_nft(valid), "registry accepts valid NFT");
    expect(!geocoin::nft::NFTValidator::validate_nft_creation(duplicate, registry),
           "duplicate NFT id is rejected");
}

void test_polygon_and_coordinate_helpers()
{
    geocoin::nft::NFTRegistryImpl registry;
    const auto key = make_key(11);

    auto too_few_metadata = make_metadata();
    too_few_metadata.geometry.polygons.front().exterior.resize(2);
    auto too_few_points = make_creation(too_few_metadata, key);
    expect(!geocoin::nft::NFTValidator::validate_nft_creation(too_few_points, registry),
           "polygon with fewer than three points is rejected");

    auto duplicate_vertex_metadata = make_metadata();
    duplicate_vertex_metadata.geometry.polygons.front().exterior[2] = duplicate_vertex_metadata.geometry.polygons.front().exterior[1];
    auto duplicate_vertex = make_creation(duplicate_vertex_metadata, key);
    expect(!geocoin::nft::NFTValidator::validate_nft_creation(duplicate_vertex, registry),
           "polygon with duplicate vertices is rejected");

    auto collinear_metadata = make_metadata();
    // A meridian is a WGS84 geodesic; a diagonal in lat/lon need not be.
    collinear_metadata.geometry.polygons.front().exterior = {{0LL, 0LL}, {1000000LL, 0LL}, {2000000LL, 0LL}};
    auto collinear = make_creation(collinear_metadata, key);
    expect(!geocoin::nft::NFTValidator::validate_nft_creation(collinear, registry),
           "zero-area polygon is rejected");

    auto out_of_range_metadata = make_metadata();
    out_of_range_metadata.geometry.polygons.front().exterior[0].lat_ndeg = 90000001000LL;
    auto out_of_range = make_creation(out_of_range_metadata, key);
    expect(!geocoin::nft::NFTValidator::validate_nft_creation(out_of_range, registry),
           "out-of-range latitude is rejected");

    auto low_latitude_metadata = make_metadata();
    low_latitude_metadata.geometry.polygons.front().exterior[0].lat_ndeg = -90000001000LL;
    expect(!geocoin::nft::NFTValidator::validate_nft_creation(
               make_creation(low_latitude_metadata, key), registry),
           "latitude below the valid range is rejected");

    auto high_longitude_metadata = make_metadata();
    high_longitude_metadata.geometry.polygons.front().exterior[0].lon_ndeg = 180000001000LL;
    expect(!geocoin::nft::NFTValidator::validate_nft_creation(
               make_creation(high_longitude_metadata, key), registry),
           "longitude above the valid range is rejected");

    auto low_longitude_metadata = make_metadata();
    low_longitude_metadata.geometry.polygons.front().exterior[0].lon_ndeg = -180000001000LL;
    expect(!geocoin::nft::NFTValidator::validate_nft_creation(
               make_creation(low_longitude_metadata, key), registry),
           "longitude below the valid range is rejected");

    auto too_many_points_metadata = make_metadata();
    too_many_points_metadata.geometry.polygons.front().exterior.clear();
    for (size_t i = 0; i <= geocoin::consensus::params::MAX_NFT_POINTS; ++i) {
        too_many_points_metadata.geometry.polygons.front().exterior.push_back({
            static_cast<int32_t>(i),
            static_cast<int32_t>(i * i)
        });
    }
    expect(!geocoin::nft::NFTValidator::validate_nft_creation(
               make_creation(too_many_points_metadata, key), registry),
           "polygon above the point-count limit is rejected");

    auto crossing_metadata = make_metadata();
    crossing_metadata.geometry.polygons.front().exterior = {
        {0LL, 0LL}, {100000000LL, 100000000LL}, {0LL, 100000000LL}, {100000000LL, 0LL}, {150000000LL, 50000000LL}
    };
    expect(!geocoin::nft::NFTValidator::validate_nft_creation(
               make_creation(crossing_metadata, key), registry),
           "self-intersecting polygon with non-zero shoelace area is rejected");

    auto t_junction_metadata = make_metadata();
    t_junction_metadata.geometry.polygons.front().exterior = {
        {0LL, 0LL}, {100000000LL, 0LL}, {100000000LL, 100000000LL}, {50000000LL, 0LL}, {0LL, 100000000LL}
    };
    expect(!geocoin::nft::NFTValidator::validate_nft_creation(
               make_creation(t_junction_metadata, key), registry),
           "self-touching geodesic polygon is rejected");

    const auto polygon = make_metadata().geometry.polygons.front().exterior;
    expect(geocoin::nft::coordinates::point_in_polygon(polygon, 50000000LL, 50000000LL),
           "point inside polygon is detected");
    expect(!geocoin::nft::coordinates::point_in_polygon(polygon, 250000000LL, 250000000LL),
           "point outside polygon is detected");
    expect(geocoin::nft::coordinates::point_in_polygon(polygon, 0LL, 50000000LL),
           "point on a polygon edge is inside");
    expect(geocoin::nft::coordinates::point_in_polygon(polygon, 0LL, 0LL),
           "polygon vertex is inside");
    expect(!geocoin::nft::coordinates::point_in_polygon(
               std::vector<geocoin::nft::NFTMetadata::GeoPoint>{{0LL, 0LL}, {1000LL, 1000LL}}, 0, 0),
           "point lookup rejects a short polygon");
    expect(geocoin::nft::coordinates::calculate_distance_m(0, 0, 100000000, 0) > 10000,
           "distance helper returns a positive meter distance");

    const auto overlapping = make_metadata(50000).geometry.polygons.front().exterior;
    const auto separated = make_metadata(300000).geometry.polygons.front().exterior;
    expect(geocoin::nft::coordinates::polygons_intersect(polygon, overlapping),
           "overlapping polygons intersect");
    expect(!geocoin::nft::coordinates::polygons_intersect(polygon, separated),
           "separated polygons do not intersect");
    expect(!geocoin::nft::coordinates::polygons_intersect(
               polygon,
               std::vector<geocoin::nft::NFTMetadata::GeoPoint>{{0LL, 100000000LL}, {0LL, 200000000LL}, {100000000LL, 200000000LL}, {100000000LL, 100000000LL}}),
           "neighbouring parcels can share a geodesic boundary");
    expect(geocoin::nft::coordinates::polygons_intersect(
               std::vector<geocoin::nft::NFTMetadata::GeoPoint>{{25000000LL, 25000000LL}, {25000000LL, 75000000LL}, {75000000LL, 50000000LL}},
               polygon),
           "polygon contained by another polygon intersects");
    expect(geocoin::nft::coordinates::polygons_intersect(
               polygon,
               std::vector<geocoin::nft::NFTMetadata::GeoPoint>{{25000000LL, 25000000LL}, {25000000LL, 75000000LL}, {75000000LL, 50000000LL}}),
           "polygon containing another polygon intersects");
    expect(!geocoin::nft::coordinates::polygons_intersect(
               std::vector<geocoin::nft::NFTMetadata::GeoPoint>{{0LL, 0LL}, {1000LL, 1000LL}}, polygon),
           "intersection lookup rejects a short first polygon");
    expect(!geocoin::nft::coordinates::polygons_intersect(
               polygon, std::vector<geocoin::nft::NFTMetadata::GeoPoint>{{0LL, 0LL}, {1000LL, 1000LL}}),
           "intersection lookup rejects a short second polygon");
}

void test_cadastral_guards()
{
    geocoin::nft::NFTRegistryImpl registry;
    const auto key = make_key(21);

    auto metadata = make_metadata();
    metadata.cadastral = make_cadastral(metadata);
    auto valid = make_creation(metadata, key);
    expect(geocoin::nft::NFTValidator::validate_nft_creation(valid, registry),
           "valid cadastral NFT creation is accepted");
    expect(registry.register_nft(valid), "registry stores cadastral NFT");

    auto missing_number_metadata = make_metadata(300000);
    missing_number_metadata.cadastral = make_cadastral(missing_number_metadata, "");
    auto missing_number = make_creation(missing_number_metadata, key);
    expect(!geocoin::nft::NFTValidator::validate_nft_creation(missing_number, registry),
           "missing cadastral number is rejected");

    auto missing_owner_metadata = make_metadata(300000);
    missing_owner_metadata.cadastral = make_cadastral(missing_owner_metadata, "77:01:0001001:9999");
    missing_owner_metadata.cadastral->owner_name.clear();
    auto missing_owner = make_creation(missing_owner_metadata, key);
    expect(!geocoin::nft::NFTValidator::validate_nft_creation(missing_owner, registry),
           "missing cadastral owner is rejected");

    auto long_number_metadata = make_metadata(300000);
    long_number_metadata.cadastral = make_cadastral(long_number_metadata, "77:01:0001001:9910");
    long_number_metadata.cadastral->cadastral_number.assign(
        geocoin::consensus::params::MAX_CADASTRAL_NUMBER + 1, 'n');
    expect(!geocoin::nft::NFTValidator::validate_nft_creation(
               make_creation(long_number_metadata, key), registry),
           "oversized cadastral number is rejected");

    auto long_owner_metadata = make_metadata(300000);
    long_owner_metadata.cadastral = make_cadastral(long_owner_metadata, "77:01:0001001:9911");
    long_owner_metadata.cadastral->owner_name.assign(
        geocoin::consensus::params::MAX_CADASTRAL_OWNER_NAME + 1, 'o');
    expect(!geocoin::nft::NFTValidator::validate_nft_creation(
               make_creation(long_owner_metadata, key), registry),
           "oversized cadastral owner name is rejected");

    auto long_category_metadata = make_metadata(300000);
    long_category_metadata.cadastral = make_cadastral(long_category_metadata, "77:01:0001001:9912");
    long_category_metadata.cadastral->land_category.assign(
        geocoin::consensus::params::MAX_LAND_CATEGORY + 1, 'c');
    expect(!geocoin::nft::NFTValidator::validate_nft_creation(
               make_creation(long_category_metadata, key), registry),
           "oversized land category is rejected");

    auto long_use_metadata = make_metadata(300000);
    long_use_metadata.cadastral = make_cadastral(long_use_metadata, "77:01:0001001:9913");
    long_use_metadata.cadastral->permitted_use.assign(
        geocoin::consensus::params::MAX_PERMITTED_USE + 1, 'u');
    expect(!geocoin::nft::NFTValidator::validate_nft_creation(
               make_creation(long_use_metadata, key), registry),
           "oversized permitted use is rejected");

    auto too_many_photos_metadata = make_metadata(300000);
    too_many_photos_metadata.cadastral = make_cadastral(too_many_photos_metadata, "77:01:0001001:9914");
    too_many_photos_metadata.cadastral->photo_hashes.assign(
        geocoin::consensus::params::MAX_PHOTO_HASHES + 1, hash_from_seed(0x40));
    expect(!geocoin::nft::NFTValidator::validate_nft_creation(
               make_creation(too_many_photos_metadata, key), registry),
           "too many cadastral photo hashes are rejected");

    auto too_many_restrictions_metadata = make_metadata(300000);
    too_many_restrictions_metadata.cadastral = make_cadastral(
        too_many_restrictions_metadata, "77:01:0001001:9915");
    too_many_restrictions_metadata.cadastral->restrictions.assign(
        geocoin::consensus::params::MAX_RESTRICTIONS + 1, "entry");
    expect(!geocoin::nft::NFTValidator::validate_nft_creation(
               make_creation(too_many_restrictions_metadata, key), registry),
           "too many cadastral restrictions are rejected");

    auto long_restriction_metadata = make_metadata(300000);
    long_restriction_metadata.cadastral = make_cadastral(
        long_restriction_metadata, "77:01:0001001:9916");
    long_restriction_metadata.cadastral->restrictions = {
        std::string(geocoin::consensus::params::MAX_RESTRICTION_LEN + 1, 'r')
    };
    expect(!geocoin::nft::NFTValidator::validate_nft_creation(
               make_creation(long_restriction_metadata, key), registry),
           "oversized cadastral restriction is rejected");

    auto zero_area_metadata = make_metadata(300000);
    zero_area_metadata.cadastral = make_cadastral(zero_area_metadata, "77:01:0001001:9998");
    zero_area_metadata.cadastral->area_sqcm_official = 0;
    auto zero_area = make_creation(zero_area_metadata, key);
    expect(!geocoin::nft::NFTValidator::validate_nft_creation(zero_area, registry),
           "zero official cadastral area is rejected");

    auto projected_zero_metadata = make_metadata();
    projected_zero_metadata.geometry.polygons.front().exterior = {{0, 0}, {0, 1}, {1, 1}, {1, 0}};
    projected_zero_metadata.cadastral = make_cadastral(
        projected_zero_metadata, "77:01:0001001:9995");
    projected_zero_metadata.cadastral->area_sqcm_official = 1;
    expect(!geocoin::nft::NFTValidator::validate_nft_creation(
               make_creation(projected_zero_metadata, key), registry),
           "polygon below the geometric area resolution is rejected");

    auto mismatched_area_metadata = make_metadata(300000);
    mismatched_area_metadata.cadastral = make_cadastral(mismatched_area_metadata, "77:01:0001001:9997");
    mismatched_area_metadata.cadastral->area_sqcm_official *= 2;
    auto mismatched_area = make_creation(mismatched_area_metadata, key);
    expect(!geocoin::nft::NFTValidator::validate_nft_creation(mismatched_area, registry),
           "mismatched official cadastral area is rejected");

    auto negative_market = valid;
    negative_market.metadata.cadastral->market_value = -1;
    expect(!geocoin::nft::NFTValidator::validate_nft_creation(negative_market, registry),
           "negative cadastral market value is rejected");

    auto negative_cadastral_value = valid;
    negative_cadastral_value.metadata.cadastral->cadastral_value = -1;
    expect(!geocoin::nft::NFTValidator::validate_nft_creation(negative_cadastral_value, registry),
           "negative cadastral assessed value is rejected");

    auto missing_building_count_metadata = make_metadata(300000);
    missing_building_count_metadata.cadastral = make_cadastral(
        missing_building_count_metadata, "77:01:0001001:9917");
    missing_building_count_metadata.cadastral->has_buildings = true;
    missing_building_count_metadata.cadastral->building_count = 0;
    expect(!geocoin::nft::NFTValidator::validate_nft_creation(
               make_creation(missing_building_count_metadata, key), registry),
           "building flag without a building count is rejected");

    auto unexpected_building_count_metadata = make_metadata(300000);
    unexpected_building_count_metadata.cadastral = make_cadastral(
        unexpected_building_count_metadata, "77:01:0001001:9918");
    unexpected_building_count_metadata.cadastral->has_buildings = false;
    unexpected_building_count_metadata.cadastral->building_count = 1;
    expect(!geocoin::nft::NFTValidator::validate_nft_creation(
               make_creation(unexpected_building_count_metadata, key), registry),
           "building count without the building flag is rejected");

    auto duplicate_number_metadata = make_metadata(300000);
    duplicate_number_metadata.cadastral = make_cadastral(duplicate_number_metadata);
    auto duplicate_number = make_creation(duplicate_number_metadata, key);
    expect(!geocoin::nft::NFTValidator::validate_nft_creation(duplicate_number, registry),
           "duplicate cadastral number is rejected");

    auto intersecting_metadata = make_metadata(50000);
    intersecting_metadata.cadastral = make_cadastral(intersecting_metadata, "77:01:0001001:9996");
    auto intersecting = make_creation(intersecting_metadata, key);
    expect(!geocoin::nft::NFTValidator::validate_nft_creation(intersecting, registry),
           "intersecting cadastral polygon is rejected");

    geocoin::nft::NFTRegistryImpl regular_registry;
    auto regular_geo_nft = make_creation(make_metadata(700000), make_key(22));
    expect(regular_registry.register_nft(regular_geo_nft),
           "registry stores an ordinary geo NFT for cadastral isolation");
    auto cadastral_same_geometry = regular_geo_nft.metadata;
    cadastral_same_geometry.cadastral = make_cadastral(
        cadastral_same_geometry, "77:01:0001001:9994");
    expect(geocoin::nft::NFTValidator::validate_nft_creation(
               make_creation(cadastral_same_geometry, key), regular_registry),
           "ordinary geo NFTs do not reserve cadastral plot geometry");
}

void test_cadastral_lookup_without_full_registry_copy()
{
    using namespace geocoin::nft;
    class Registry : public NFTRegistryImpl {
    public:
        std::vector<geocoin::Hash256> get_all_nfts() const override {
            throw std::runtime_error("cadastral validation must not copy the full NFT list");
        }
        NFTMetadata get_metadata(const geocoin::Hash256&) const override {
            throw std::runtime_error("cadastral validation must read indexed metadata by reference");
        }
    } registry;
    const auto key = make_key(29);
    auto ordinary = make_creation(make_metadata(700000), key);
    expect(registry.register_nft(ordinary), "seed ordinary NFT outside cadastral index");
    auto data = make_metadata();
    data.cadastral = make_cadastral(data, "cad-index-one");
    auto existing = make_creation(data, key);
    expect(registry.register_nft(existing), "seed indexed cadastral NFT");
    auto second_data = make_metadata(300000);
    second_data.cadastral = make_cadastral(second_data, "cad-index-two");
    auto second = make_creation(second_data, key);
    expect(registry.register_nft(second), "seed second indexed cadastral NFT");

    size_t visits = 0;
    expect(registry.for_each_cadastral([&](const auto&, const auto& entry) {
        ++visits;
        expect(entry.cadastral.has_value(), "visitor sees only cadastral records");
        return true;
    }) && visits == 2, "cadastral traversal excludes ordinary NFTs");
    visits = 0;
    expect(!registry.for_each_cadastral([&](const auto&, const auto&) { ++visits; return false; }) && visits == 1,
           "cadastral traversal stops at the first conflict");

    auto replacement_data = data;
    replacement_data.name += " replacement";
    auto replacement = make_creation(replacement_data, key);
    expect(!NFTValidator::validate_nft_creation(replacement, registry), "indexed cadastral slot is occupied");
    expect(registry.burn_nft(existing.nft_id), "remove first cadastral record");
    expect(NFTValidator::validate_nft_creation(replacement, registry), "burn releases the indexed slot");
    expect(registry.register_nft(existing), "restore first cadastral record as undo does");
    expect(!NFTValidator::validate_nft_creation(replacement, registry), "restoration reserves the slot again");

    NFTRegistryImpl copy = std::move(registry);
    expect(!NFTValidator::validate_nft_creation(replacement, copy), "registry move preserves derived cadastral index");
    static_cast<NFTRegistryImpl&>(registry) = NFTRegistryImpl{};
    expect(NFTValidator::validate_nft_creation(replacement, registry), "registry reset clears cadastral reservations");
}

void test_pol_disabled_and_transfer_burn()
{
    geocoin::nft::NFTRegistryImpl registry;
    const auto owner_key = make_key(31);
    const auto recipient_key = make_key(32);
    const geocoin::Address owner = address_from_key(owner_key);
    const geocoin::Address recipient = address_from_key(recipient_key);

    auto creation = make_creation(make_metadata(), owner_key);
    auto with_pol = creation;
    with_pol.pol_data = geocoin::nft::ProofOfLocationData{
        50000000,
        50000000,
        creation.metadata.created_at * 1000ULL,
        1000,
        static_cast<uint8_t>(geocoin::nft::PoLSourceType::GPS_NATIVE)
    };
    expect(!geocoin::nft::NFTValidator::validate_nft_creation(with_pol, registry),
           "PoL creation is rejected while PoL consensus flag is disabled");

    geocoin::nft::NFTTransferTransaction missing_transfer;
    missing_transfer.nft_id = hash_from_seed(0x99);
    missing_transfer.from = owner;
    missing_transfer.to = recipient;
    expect(!geocoin::nft::NFTValidator::validate_nft_transfer(missing_transfer, registry),
           "transfer of missing NFT is rejected");

    expect(registry.register_nft(creation), "registry stores NFT for transfer tests");
    geocoin::nft::NFTTransferTransaction wrong_owner_transfer;
    wrong_owner_transfer.nft_id = creation.nft_id;
    wrong_owner_transfer.from = recipient;
    wrong_owner_transfer.to = owner;
    expect(!geocoin::nft::NFTValidator::validate_nft_transfer(wrong_owner_transfer, registry),
           "transfer by non-owner is rejected");

    geocoin::nft::NFTTransferTransaction valid_transfer;
    valid_transfer.nft_id = creation.nft_id;
    valid_transfer.from = owner;
    valid_transfer.to = recipient;
    valid_transfer.fee = geocoin::consensus::params::MIN_TX_FEE;
    sign_transfer(valid_transfer, owner_key);
    expect(geocoin::nft::NFTValidator::validate_nft_transfer(valid_transfer, registry),
           "transfer by owner is accepted");

    auto empty_signature_transfer = valid_transfer;
    empty_signature_transfer.signature.clear();
    expect(!geocoin::nft::NFTValidator::validate_nft_transfer(
               empty_signature_transfer, registry),
           "transfer with an empty owner signature is rejected");

    auto wrong_signature_transfer = valid_transfer;
    sign_transfer(wrong_signature_transfer, recipient_key);
    expect(!geocoin::nft::NFTValidator::validate_nft_transfer(
               wrong_signature_transfer, registry),
           "transfer signed by another address is rejected");

    auto changed_recipient_transfer = valid_transfer;
    changed_recipient_transfer.to = address_from_key(make_key(33));
    expect(!geocoin::nft::NFTValidator::validate_nft_transfer(
               changed_recipient_transfer, registry),
           "transfer signature binds the recipient");

    auto changed_fee_transfer = valid_transfer;
    ++changed_fee_transfer.fee;
    expect(!geocoin::nft::NFTValidator::validate_nft_transfer(
               changed_fee_transfer, registry),
           "transfer signature binds the fee");

    auto invalid_recipient_transfer = valid_transfer;
    invalid_recipient_transfer.to = geocoin::Address("not-a-geocoin-address");
    expect(!geocoin::nft::NFTValidator::validate_nft_transfer(invalid_recipient_transfer, registry),
           "transfer to an invalid NFT owner address is rejected");

    auto invalid_sender_transfer = valid_transfer;
    invalid_sender_transfer.from = geocoin::Address("not-a-geocoin-address");
    expect(!geocoin::nft::NFTValidator::validate_nft_transfer(invalid_sender_transfer, registry),
           "transfer from an invalid NFT owner address is rejected");

    auto negative_fee_transfer = valid_transfer;
    negative_fee_transfer.fee = -1;
    expect(!geocoin::nft::NFTValidator::validate_nft_transfer(negative_fee_transfer, registry),
           "negative NFT transfer fee is rejected");
    expect(registry.transfer_nft(creation.nft_id, recipient), "registry transfers NFT");

    geocoin::nft::NFTBurnTransaction missing_burn;
    missing_burn.nft_id = hash_from_seed(0x98);
    missing_burn.owner = recipient;
    expect(!geocoin::nft::NFTValidator::validate_nft_burn(missing_burn, registry),
           "burn of missing NFT is rejected");

    geocoin::nft::NFTBurnTransaction wrong_owner_burn;
    wrong_owner_burn.nft_id = creation.nft_id;
    wrong_owner_burn.owner = owner;
    expect(!geocoin::nft::NFTValidator::validate_nft_burn(wrong_owner_burn, registry),
           "burn by non-owner is rejected");

    geocoin::nft::NFTBurnTransaction valid_burn;
    valid_burn.nft_id = creation.nft_id;
    valid_burn.owner = recipient;
    valid_burn.fee = geocoin::consensus::params::MIN_TX_FEE;
    sign_burn(valid_burn, recipient_key);
    expect(geocoin::nft::NFTValidator::validate_nft_burn(valid_burn, registry),
           "burn by owner is accepted");

    auto empty_signature_burn = valid_burn;
    empty_signature_burn.signature.clear();
    expect(!geocoin::nft::NFTValidator::validate_nft_burn(empty_signature_burn, registry),
           "burn with an empty owner signature is rejected");

    auto wrong_signature_burn = valid_burn;
    sign_burn(wrong_signature_burn, owner_key);
    expect(!geocoin::nft::NFTValidator::validate_nft_burn(wrong_signature_burn, registry),
           "burn signed by another address is rejected");

    auto changed_fee_burn = valid_burn;
    ++changed_fee_burn.fee;
    expect(!geocoin::nft::NFTValidator::validate_nft_burn(changed_fee_burn, registry),
           "burn signature binds the fee");

    auto negative_fee_burn = valid_burn;
    negative_fee_burn.fee = -1;
    expect(!geocoin::nft::NFTValidator::validate_nft_burn(negative_fee_burn, registry),
           "negative NFT burn fee is rejected");

    auto invalid_owner_burn = valid_burn;
    invalid_owner_burn.owner = geocoin::Address("not-a-geocoin-address");
    expect(!geocoin::nft::NFTValidator::validate_nft_burn(invalid_owner_burn, registry),
           "burn with an invalid owner address is rejected");
    expect(registry.burn_nft(creation.nft_id), "registry burns NFT");
    expect(!registry.exists(creation.nft_id), "burned NFT is removed from registry");
}

void test_proof_of_location_rules()
{
    const auto metadata = make_metadata();
    geocoin::nft::ProofOfLocationData pol{
        50000000,
        50000000,
        metadata.created_at * 1000ULL,
        1,
        static_cast<uint8_t>(geocoin::nft::PoLSourceType::GPS_NATIVE)
    };

    for (uint8_t source = static_cast<uint8_t>(geocoin::nft::PoLSourceType::GPS_NATIVE);
         source <= static_cast<uint8_t>(geocoin::nft::PoLSourceType::HYBRID);
         ++source) {
        pol.source_type = source;
        expect(geocoin::nft::NFTValidator::validate_proof_of_location(pol, metadata),
               "every defined PoL source is accepted for valid data");
    }

    auto invalid_source = pol;
    invalid_source.source_type = static_cast<uint8_t>(geocoin::nft::PoLSourceType::HYBRID) + 1;
    expect(!geocoin::nft::NFTValidator::validate_proof_of_location(invalid_source, metadata),
           "undefined PoL source is rejected");

    auto low_latitude = pol;
    low_latitude.latitude_ndeg = -90000001000LL;
    expect(!geocoin::nft::NFTValidator::validate_proof_of_location(low_latitude, metadata),
           "PoL latitude below its range is rejected");
    auto high_latitude = pol;
    high_latitude.latitude_ndeg = 90000001000LL;
    expect(!geocoin::nft::NFTValidator::validate_proof_of_location(high_latitude, metadata),
           "PoL latitude above its range is rejected");
    auto low_longitude = pol;
    low_longitude.longitude_ndeg = -180000001000LL;
    expect(!geocoin::nft::NFTValidator::validate_proof_of_location(low_longitude, metadata),
           "PoL longitude below its range is rejected");
    auto high_longitude = pol;
    high_longitude.longitude_ndeg = 180000001000LL;
    expect(!geocoin::nft::NFTValidator::validate_proof_of_location(high_longitude, metadata),
           "PoL longitude above its range is rejected");

    auto zero_accuracy = pol;
    zero_accuracy.gps_accuracy_cm = 0;
    expect(!geocoin::nft::NFTValidator::validate_proof_of_location(zero_accuracy, metadata),
           "zero PoL accuracy is rejected by the complete rule set");
    auto excessive_accuracy = pol;
    excessive_accuracy.gps_accuracy_cm = geocoin::consensus::params::MAX_NFT_POL_ACCURACY_CM + 1;
    expect(!geocoin::nft::NFTValidator::validate_proof_of_location(excessive_accuracy, metadata),
           "PoL accuracy above the consensus limit is rejected");
    auto maximum_accuracy = pol;
    maximum_accuracy.gps_accuracy_cm = geocoin::consensus::params::MAX_NFT_POL_ACCURACY_CM;
    expect(geocoin::nft::NFTValidator::validate_proof_of_location(maximum_accuracy, metadata),
           "PoL accuracy at the exact consensus limit is accepted");

    auto empty_polygon = metadata;
    empty_polygon.geometry.polygons.front().exterior.clear();
    expect(!geocoin::nft::NFTValidator::validate_proof_of_location(pol, empty_polygon),
           "PoL location rejects empty target geometry");

    auto close_to_vertex = pol;
    close_to_vertex.latitude_ndeg = -50000LL;
    close_to_vertex.longitude_ndeg = 0LL;
    close_to_vertex.gps_accuracy_cm = 100;
    expect(geocoin::nft::NFTValidator::validate_proof_of_location(close_to_vertex, metadata),
           "PoL point within the accuracy buffer of a vertex is accepted");

    auto far_away = pol;
    far_away.latitude_ndeg = 1000000000LL;
    far_away.longitude_ndeg = 1000000000LL;
    expect(!geocoin::nft::NFTValidator::validate_proof_of_location(far_away, metadata),
           "PoL point outside the polygon and its buffer is rejected");

    auto future = pol;
    future.timestamp_ms = metadata.created_at * 1000ULL + 1;
    expect(!geocoin::nft::NFTValidator::validate_proof_of_location(future, metadata),
           "PoL timestamp one millisecond in the future is rejected");
    auto maximum_age = pol;
    maximum_age.timestamp_ms = metadata.created_at * 1000ULL - 3'600'000ULL;
    expect(geocoin::nft::NFTValidator::validate_proof_of_location(maximum_age, metadata),
           "PoL timestamp at the exact age limit is accepted");
    auto too_old = maximum_age;
    --too_old.timestamp_ms;
    expect(!geocoin::nft::NFTValidator::validate_proof_of_location(too_old, metadata),
           "PoL timestamp one millisecond beyond the age limit is rejected");

    auto overflowing_time = metadata;
    overflowing_time.created_at = (std::numeric_limits<uint64_t>::max)() / 1000ULL + 1;
    expect(!geocoin::nft::NFTValidator::validate_proof_of_location(pol, overflowing_time),
           "PoL reference timestamp multiplication overflow is rejected");
}

void test_registry_indexes_without_sorted_assumption()
{
    geocoin::nft::NFTRegistryImpl registry;

    auto high_meta = make_metadata();
    high_meta.tags = {"land"};
    auto high = make_creation(high_meta, make_key(41));
    high.nft_id = hash_from_seed(0xf0);

    auto low_meta = make_metadata(300000);
    low_meta.tags = {"land", "rare"};
    auto low = make_creation(low_meta, make_key(42));
    low.nft_id = hash_from_seed(0x01);

    expect(registry.register_nft(high), "registry stores first tagged NFT");
    expect(registry.register_nft(low), "registry stores second tagged NFT");

    const auto land = registry.find_by_tags({"land"});
    expect(land.size() == 2, "single-tag lookup returns both NFTs");

    const auto land_and_rare = registry.find_by_tags({"land", "rare"});
    expect(land_and_rare.size() == 1 && land_and_rare[0] == low.nft_id,
           "multi-tag lookup works without sorted Hash256 indexes");

    expect(registry.get_owned_nfts(low.creator_address).size() == 1,
           "owner index tracks tagged NFT");
    expect(registry.find_by_location(350000000LL, 350000000LL, 0).size() == 1,
           "location lookup finds point inside polygon");
    expect(registry.find_by_location(300000000LL, 300000000LL, 1).size() == 1,
           "location lookup finds nearby polygon vertex with radius");
    expect(registry.find_by_tags({"missing"}).empty(),
           "missing tag lookup returns empty");
    expect(registry.find_by_tags({"land", "missing"}).empty(),
           "missing secondary tag clears intersection");
    expect(registry.find_by_tags({}).empty(),
           "empty tag lookup returns empty");

    auto all = registry.get_all_nfts();
    expect(all.size() == 2, "registry returns all stored NFT ids");

    auto short_polygon = make_creation(make_metadata(600000), make_key(43));
    short_polygon.metadata.tags = {"short-poly"};
    short_polygon.metadata.geometry.polygons.front().exterior.resize(2);
    expect(registry.register_nft(short_polygon), "registry can store short polygon metadata");
    expect(registry.find_by_location(600000000LL, 600000000LL, 1000).empty(),
           "location lookup skips malformed short polygons");
}

void test_registry_index_integrity_across_updates()
{
    geocoin::nft::NFTRegistryImpl registry;
    const auto owner_key = make_key(44);
    const geocoin::Address owner = address_from_key(owner_key);
    const geocoin::Address recipient = address_from_key(make_key(45));

    auto metadata_a = make_metadata(1'000'000);
    metadata_a.name = "Index A";
    metadata_a.tags = {"common", "first"};
    auto nft_a = make_creation(metadata_a, owner_key);

    auto metadata_b = make_metadata(1'300'000);
    metadata_b.name = "Index B";
    metadata_b.tags = {"common", "middle"};
    auto nft_b = make_creation(metadata_b, owner_key);

    auto metadata_c = make_metadata(1'600'000);
    metadata_c.name = "Index C";
    metadata_c.tags = {"common", "last"};
    auto nft_c = make_creation(metadata_c, owner_key);

    expect(registry.register_nft(nft_a), "registry stores first index-integrity NFT");
    expect(registry.register_nft(nft_b), "registry stores middle index-integrity NFT");
    expect(registry.register_nft(nft_c), "registry stores last index-integrity NFT");

    expect(registry.transfer_nft(nft_a.nft_id, recipient), "registry transfers first indexed NFT");
    const auto owner_after_transfer = registry.get_owned_nfts(owner);
    expect(owner_after_transfer.size() == 2 &&
               std::find(owner_after_transfer.begin(), owner_after_transfer.end(), nft_b.nft_id) != owner_after_transfer.end() &&
               std::find(owner_after_transfer.begin(), owner_after_transfer.end(), nft_c.nft_id) != owner_after_transfer.end(),
           "transferring one NFT preserves every later owner-index entry");
    expect(registry.get_owned_nfts(recipient).size() == 1,
           "recipient owner index receives exactly one NFT");

    expect(registry.transfer_nft(nft_c.nft_id, owner), "same-owner NFT transfer succeeds");
    const auto owner_after_same_owner_transfer = registry.get_owned_nfts(owner);
    expect(std::count(owner_after_same_owner_transfer.begin(), owner_after_same_owner_transfer.end(), nft_c.nft_id) == 1,
           "same-owner transfer does not duplicate the owner index");

    expect(registry.burn_nft(nft_b.nft_id), "registry burns middle indexed NFT");
    const auto owner_after_burn = registry.get_owned_nfts(owner);
    expect(owner_after_burn.size() == 1 && owner_after_burn.front() == nft_c.nft_id,
           "burning one NFT preserves later owner-index entries");

    const auto common_after_burn = registry.find_by_tags({"common"});
    expect(common_after_burn.size() == 2 &&
               std::find(common_after_burn.begin(), common_after_burn.end(), nft_a.nft_id) != common_after_burn.end() &&
               std::find(common_after_burn.begin(), common_after_burn.end(), nft_c.nft_id) != common_after_burn.end(),
           "burning one NFT preserves unrelated tag-index entries");

    auto duplicate_tag_metadata = make_metadata(1'900'000);
    duplicate_tag_metadata.name = "Duplicate tag index";
    duplicate_tag_metadata.tags = {"duplicate", "duplicate"};
    auto duplicate_tag_nft = make_creation(duplicate_tag_metadata, owner_key);
    expect(registry.register_nft(duplicate_tag_nft), "registry defensively stores duplicate-tag metadata");
    const auto duplicate_tag_matches = registry.find_by_tags({"duplicate"});
    expect(duplicate_tag_matches.size() == 1 && duplicate_tag_matches.front() == duplicate_tag_nft.nft_id,
           "duplicate metadata tags do not duplicate query results");

    expect(registry.find_by_location(
               -80000000000LL, -170000000000LL, (std::numeric_limits<uint32_t>::max)()).size() >= 3,
           "maximum location radius remains an unsigned distance");
    expect(registry.find_by_location(
               90000001000LL, 0LL, (std::numeric_limits<uint32_t>::max)()).empty(),
           "invalid location is rejected even with the maximum radius");
}

void test_nft_serialization_roundtrips_and_bounds()
{
    const geocoin::nft::ProofOfLocationData pol{
        12345678,
        -23456789,
        1700000000123ULL,
        150,
        static_cast<uint8_t>(geocoin::nft::PoLSourceType::HYBRID)
    };
    const auto pol_bytes = pol.serialize();
    expect(pol_bytes.size() == 29, "PoL serialization uses the full 29-byte layout");
    const auto pol_roundtrip = geocoin::nft::ProofOfLocationData::deserialize(pol_bytes);
    expect(pol_roundtrip.latitude_ndeg == pol.latitude_ndeg &&
               pol_roundtrip.longitude_ndeg == pol.longitude_ndeg &&
               pol_roundtrip.timestamp_ms == pol.timestamp_ms &&
               pol_roundtrip.gps_accuracy_cm == pol.gps_accuracy_cm &&
               pol_roundtrip.source_type == pol.source_type,
           "PoL data roundtrips through serialization");

    const geocoin::nft::ProofOfLocationData extreme_pol{
        (std::numeric_limits<int64_t>::min)(),
        (std::numeric_limits<int64_t>::max)(),
        (std::numeric_limits<uint64_t>::max)(),
        (std::numeric_limits<uint32_t>::max)(),
        0xff
    };
    const auto extreme_pol_roundtrip = geocoin::nft::ProofOfLocationData::deserialize(
        extreme_pol.serialize());
    expect(extreme_pol_roundtrip.latitude_ndeg == extreme_pol.latitude_ndeg &&
               extreme_pol_roundtrip.longitude_ndeg == extreme_pol.longitude_ndeg &&
               extreme_pol_roundtrip.timestamp_ms == extreme_pol.timestamp_ms &&
               extreme_pol_roundtrip.gps_accuracy_cm == extreme_pol.gps_accuracy_cm &&
               extreme_pol_roundtrip.source_type == extreme_pol.source_type,
           "PoL integer extrema roundtrip without signedness changes");

    auto high_bit_u32_pol = pol;
    high_bit_u32_pol.gps_accuracy_cm = 0xf0000001U;
    const auto high_bit_u32_roundtrip = geocoin::nft::ProofOfLocationData::deserialize(
        high_bit_u32_pol.serialize()
    );
    expect(high_bit_u32_roundtrip.gps_accuracy_cm == high_bit_u32_pol.gps_accuracy_cm,
           "PoL uint32 decoding is unsigned for high-bit wire values");
    expect(pol.is_valid_accuracy(), "valid PoL accuracy is accepted");

    auto invalid_pol = pol;
    invalid_pol.gps_accuracy_cm = 0;
    expect(!invalid_pol.is_valid_accuracy(), "zero PoL accuracy is rejected");
    invalid_pol.gps_accuracy_cm = 1000001;
    expect(!invalid_pol.is_valid_accuracy(), "oversized PoL accuracy is rejected");

    expect_throws([] {
        geocoin::nft::ProofOfLocationData::deserialize(geocoin::bytes(20, 0));
    }, "short PoL payload throws");
    auto trailing_pol = pol_bytes;
    trailing_pol.push_back(0);
    expect_throws([&] {
        geocoin::nft::ProofOfLocationData::deserialize(trailing_pol);
    }, "PoL payload with trailing bytes throws");

    auto metadata = make_metadata();
    metadata.cadastral = make_cadastral(metadata);
    metadata.cadastral->photo_hashes.push_back(hash_from_seed(0x34));
    metadata.cadastral->restrictions = {"lease", "easement"};
    metadata.cadastral->has_buildings = true;
    metadata.cadastral->building_count = 3;

    const auto metadata_bytes = metadata.serialize();
    const auto metadata_roundtrip = geocoin::nft::NFTMetadata::deserialize(metadata_bytes);
    expect(metadata_roundtrip.name == metadata.name &&
               metadata_roundtrip.description == metadata.description &&
               metadata_roundtrip.image_uri == metadata.image_uri &&
               metadata_roundtrip.content_hash == metadata.content_hash &&
               metadata_roundtrip.geometry.polygons.front().exterior.size() == metadata.geometry.polygons.front().exterior.size() &&
               metadata_roundtrip.tags == metadata.tags &&
               metadata_roundtrip.creator == metadata.creator &&
               metadata_roundtrip.cadastral.has_value(),
           "NFT metadata with cadastral data roundtrips");
    expect(metadata_roundtrip.cadastral->photo_hashes.size() == 2 &&
               metadata_roundtrip.cadastral->restrictions.size() == 2 &&
               metadata_roundtrip.cadastral->has_buildings &&
               metadata_roundtrip.cadastral->building_count == 3,
           "NFT cadastral nested fields roundtrip");
    expect_all_strict_prefixes_throw(
        metadata_bytes,
        [](const geocoin::bytes& value) { (void)geocoin::nft::NFTMetadata::deserialize(value); },
        "every truncated cadastral metadata prefix must be rejected"
    );

    auto no_cadastral = make_metadata(700000);
    const auto no_cadastral_roundtrip = geocoin::nft::NFTMetadata::deserialize(no_cadastral.serialize());
    expect(!no_cadastral_roundtrip.cadastral.has_value(),
           "NFT metadata without cadastral data roundtrips");

    auto missing_cadastral_flag = no_cadastral.serialize();
    missing_cadastral_flag.pop_back();
    expect_throws([&] {
        (void)geocoin::nft::NFTMetadata::deserialize(missing_cadastral_flag);
    }, "NFT metadata without its cadastral flag throws");

    auto noncanonical_name_length = no_cadastral.serialize();
    expect(noncanonical_name_length.front() < 0xfd, "test metadata name uses one-byte length");
    const geocoin::byte name_length = noncanonical_name_length.front();
    noncanonical_name_length.erase(noncanonical_name_length.begin());
    noncanonical_name_length.insert(
        noncanonical_name_length.begin(), {0xfd, name_length, 0x00});
    expect_throws([&] {
        (void)geocoin::nft::NFTMetadata::deserialize(noncanonical_name_length);
    }, "non-minimal NFT metadata length encoding throws");

    auto maximum_metadata = make_metadata(800000);
    maximum_metadata.name.assign(geocoin::consensus::params::MAX_NFT_NAME, 'n');
    maximum_metadata.description.assign(geocoin::consensus::params::MAX_NFT_DESCRIPTION, 'd');
    maximum_metadata.image_uri.assign(geocoin::consensus::params::MAX_NFT_IMAGE_URI, 'i');
    maximum_metadata.location_name.assign(geocoin::consensus::params::MAX_NFT_LOCATION_NAME, 'l');
    maximum_metadata.creator.assign(geocoin::consensus::params::MAX_ADDRESS_SIZE, 'c');
    maximum_metadata.tags.assign(
        geocoin::consensus::params::MAX_NFT_TAGS,
        std::string(geocoin::consensus::params::MAX_NFT_TAG_LEN, 't'));
    maximum_metadata.geometry.polygons.front().exterior.clear();
    for (size_t i = 0; i < geocoin::consensus::params::MAX_NFT_POINTS; ++i) {
        const double angle = 6.283185307179586 * i / geocoin::consensus::params::MAX_NFT_POINTS;
        maximum_metadata.geometry.polygons.front().exterior.push_back({
            static_cast<int64_t>(std::llround(100000000.0 * std::sin(angle))),
            static_cast<int64_t>(std::llround(100000000.0 * std::cos(angle)))});
    }
    maximum_metadata.cadastral = make_cadastral(maximum_metadata);
    maximum_metadata.cadastral->cadastral_number.assign(
        geocoin::consensus::params::MAX_CADASTRAL_NUMBER, 'n');
    maximum_metadata.cadastral->owner_name.assign(
        geocoin::consensus::params::MAX_CADASTRAL_OWNER_NAME, 'o');
    maximum_metadata.cadastral->land_category.assign(
        geocoin::consensus::params::MAX_LAND_CATEGORY, 'l');
    maximum_metadata.cadastral->permitted_use.assign(
        geocoin::consensus::params::MAX_PERMITTED_USE, 'u');
    maximum_metadata.cadastral->photo_hashes.assign(
        geocoin::consensus::params::MAX_PHOTO_HASHES, hash_from_seed(0x35));
    maximum_metadata.cadastral->restrictions.assign(
        geocoin::consensus::params::MAX_RESTRICTIONS,
        std::string(geocoin::consensus::params::MAX_RESTRICTION_LEN, 'r'));
    const auto maximum_roundtrip = geocoin::nft::NFTMetadata::deserialize(maximum_metadata.serialize());
    expect(maximum_roundtrip.name.size() == geocoin::consensus::params::MAX_NFT_NAME &&
               maximum_roundtrip.tags.size() == geocoin::consensus::params::MAX_NFT_TAGS &&
               maximum_roundtrip.geometry.polygons.front().exterior.size() == geocoin::consensus::params::MAX_NFT_POINTS &&
               maximum_roundtrip.cadastral->photo_hashes.size() == geocoin::consensus::params::MAX_PHOTO_HASHES &&
               maximum_roundtrip.cadastral->restrictions.size() == geocoin::consensus::params::MAX_RESTRICTIONS,
           "all exact NFT metadata limits roundtrip");

    auto oversized_name = make_metadata();
    oversized_name.name.assign(geocoin::consensus::params::MAX_NFT_NAME + 1, 'x');
    expect_throws([&] {
        geocoin::nft::NFTMetadata::deserialize(oversized_name.serialize());
    }, "oversized serialized NFT name throws");

    auto too_few_points = make_metadata();
    too_few_points.geometry.polygons.front().exterior.resize(2);
    expect_throws([&] {
        geocoin::nft::NFTMetadata::deserialize(too_few_points.serialize());
    }, "serialized NFT metadata with too few points throws");

    auto too_many_points = make_metadata();
    too_many_points.geometry.polygons.front().exterior.assign(geocoin::consensus::params::MAX_NFT_POINTS + 1, {0LL, 0LL});
    expect_throws([&] {
        geocoin::nft::NFTMetadata::deserialize(too_many_points.serialize());
    }, "serialized NFT metadata with too many points throws");

    auto too_many_tags = make_metadata();
    too_many_tags.tags.assign(geocoin::consensus::params::MAX_NFT_TAGS + 1, "tag");
    expect_throws([&] {
        geocoin::nft::NFTMetadata::deserialize(too_many_tags.serialize());
    }, "serialized NFT metadata with too many tags throws");

    auto too_many_photos = metadata;
    too_many_photos.cadastral->photo_hashes.assign(
        geocoin::consensus::params::MAX_PHOTO_HASHES + 1, hash_from_seed(0x36));
    expect_throws([&] {
        (void)geocoin::nft::NFTMetadata::deserialize(too_many_photos.serialize());
    }, "serialized NFT metadata with too many photo hashes throws");

    auto too_many_restrictions = metadata;
    too_many_restrictions.cadastral->restrictions.assign(
        geocoin::consensus::params::MAX_RESTRICTIONS + 1, "restriction");
    expect_throws([&] {
        (void)geocoin::nft::NFTMetadata::deserialize(too_many_restrictions.serialize());
    }, "serialized NFT metadata with too many restrictions throws");

    auto oversized_restriction = metadata;
    oversized_restriction.cadastral->restrictions = {
        std::string(geocoin::consensus::params::MAX_RESTRICTION_LEN + 1, 'r')
    };
    expect_throws([&] {
        (void)geocoin::nft::NFTMetadata::deserialize(oversized_restriction.serialize());
    }, "serialized NFT metadata with an oversized restriction throws");

    auto truncated = make_metadata().serialize();
    truncated.resize(1);
    expect_throws([&] {
        geocoin::nft::NFTMetadata::deserialize(truncated);
    }, "truncated NFT metadata throws");

    auto bad_cadastral_flag = make_metadata().serialize();
    bad_cadastral_flag.back() = 2;
    expect_throws([&] {
        geocoin::nft::NFTMetadata::deserialize(bad_cadastral_flag);
    }, "invalid NFT cadastral flag throws");

    auto trailing_metadata = metadata_bytes;
    trailing_metadata.push_back(0);
    expect_throws([&] {
        geocoin::nft::NFTMetadata::deserialize(trailing_metadata);
    }, "NFT metadata with trailing bytes throws");

    auto negative_cadastral = metadata;
    negative_cadastral.cadastral->market_value = -1;
    expect_throws([&] {
        (void)negative_cadastral.serialize();
    }, "negative in-memory NFT market value cannot be serialized");

    auto negative_cadastral_value = metadata;
    negative_cadastral_value.cadastral->cadastral_value = -1;
    expect_throws([&] {
        (void)negative_cadastral_value.serialize();
    }, "negative in-memory NFT cadastral value cannot be serialized");

    auto invalid_market_metadata = metadata;
    invalid_market_metadata.cadastral->market_value = 123456789;
    auto invalid_market_bytes = invalid_market_metadata.serialize();
    geocoin::bytes market_pattern;
    append_u64(market_pattern, 123456789);
    const auto market_it = std::search(
        invalid_market_bytes.begin(), invalid_market_bytes.end(),
        market_pattern.begin(), market_pattern.end()
    );
    expect(market_it != invalid_market_bytes.end(), "test market value encoding is present");
    std::fill(market_it, market_it + 8, 0xff);
    expect_throws([&] {
        geocoin::nft::NFTMetadata::deserialize(invalid_market_bytes);
    }, "out-of-range serialized NFT market value throws");

    auto invalid_cadastral_value_metadata = metadata;
    invalid_cadastral_value_metadata.cadastral->cadastral_value = 987654321;
    auto invalid_cadastral_value_bytes = invalid_cadastral_value_metadata.serialize();
    geocoin::bytes cadastral_value_pattern;
    append_u64(cadastral_value_pattern, 987654321);
    const auto cadastral_value_it = std::search(
        invalid_cadastral_value_bytes.begin(), invalid_cadastral_value_bytes.end(),
        cadastral_value_pattern.begin(), cadastral_value_pattern.end()
    );
    expect(cadastral_value_it != invalid_cadastral_value_bytes.end(),
           "test cadastral value encoding is present");
    std::fill(cadastral_value_it, cadastral_value_it + 8, 0xff);
    expect_throws([&] {
        (void)geocoin::nft::NFTMetadata::deserialize(invalid_cadastral_value_bytes);
    }, "out-of-range serialized NFT cadastral value throws");

    auto invalid_buildings_flag = metadata_bytes;
    invalid_buildings_flag[invalid_buildings_flag.size() - 5] = 2;
    expect_throws([&] {
        geocoin::nft::NFTMetadata::deserialize(invalid_buildings_flag);
    }, "non-canonical NFT has_buildings flag throws");

    auto creation = make_creation(make_metadata(900000), make_key(51));
    const auto creation_roundtrip = geocoin::nft::NFTCreationTransaction::deserialize(creation.serialize());
    expect(creation_roundtrip.nft_id == creation.nft_id &&
               creation_roundtrip.creator_address.str() == creation.creator_address.str() &&
               creation_roundtrip.royalty_percent == creation.royalty_percent &&
               creation_roundtrip.signature == creation.signature &&
               !creation_roundtrip.pol_data.has_value(),
           "NFT creation payload without PoL roundtrips");
    expect_all_strict_prefixes_throw(
        creation.serialize(),
        [](const geocoin::bytes& value) {
            (void)geocoin::nft::NFTCreationTransaction::deserialize(value);
        },
        "every truncated NFT creation prefix must be rejected"
    );

    auto with_pol = creation;
    with_pol.pol_data = pol;
    sign_creation(with_pol, make_key(51));
    const auto pol_creation_roundtrip = geocoin::nft::NFTCreationTransaction::deserialize(with_pol.serialize());
    expect(pol_creation_roundtrip.pol_data.has_value() &&
               pol_creation_roundtrip.pol_data->timestamp_ms == pol.timestamp_ms &&
               pol_creation_roundtrip.signature == with_pol.signature,
           "NFT creation payload with PoL roundtrips");

    expect_throws([] {
        geocoin::nft::NFTCreationTransaction::deserialize(geocoin::bytes(31, 0));
    }, "short NFT creation payload throws");
    auto trailing_creation = creation.serialize();
    trailing_creation.push_back(0);
    expect_throws([&] {
        geocoin::nft::NFTCreationTransaction::deserialize(trailing_creation);
    }, "NFT creation payload with trailing bytes throws");

    auto invalid_pol_flag = creation.serialize();
    size_t pol_flag_offset = 32 + creation.metadata.serialize().size();
    pol_flag_offset += geocoin::utils::encode_varint(creation.creator_address.str().size()).size();
    pol_flag_offset += creation.creator_address.str().size() + 4;
    expect(pol_flag_offset < invalid_pol_flag.size(), "test PoL flag offset is in bounds");
    invalid_pol_flag[pol_flag_offset] = 2;
    expect_throws([&] {
        geocoin::nft::NFTCreationTransaction::deserialize(invalid_pol_flag);
    }, "non-canonical NFT PoL flag throws");

    auto short_pol_payload = with_pol.serialize();
    size_t pol_end = 32 + with_pol.metadata.serialize().size();
    pol_end += geocoin::utils::encode_varint(with_pol.creator_address.str().size()).size();
    pol_end += with_pol.creator_address.str().size();
    pol_end += 4;
    pol_end += 1;
    short_pol_payload.resize(pol_end + 20);
    expect_throws([&] {
        geocoin::nft::NFTCreationTransaction::deserialize(short_pol_payload);
    }, "NFT creation payload with short PoL bytes throws");

    auto oversized_creation_signature = creation;
    oversized_creation_signature.signature.assign(
        geocoin::consensus::params::MAX_SIGNATURE_SIZE + 1, 0x44);
    expect_throws([&] {
        (void)geocoin::nft::NFTCreationTransaction::deserialize(
            oversized_creation_signature.serialize());
    }, "oversized NFT creation signature throws");

    geocoin::nft::NFTTransferTransaction transfer;
    transfer.nft_id = hash_from_seed(0x61);
    transfer.from = address_from_key(make_key(61));
    transfer.to = address_from_key(make_key(62));
    transfer.signature = {1, 2, 3, 4};
    transfer.fee = geocoin::consensus::params::MIN_TX_FEE;
    const auto transfer_roundtrip = geocoin::nft::NFTTransferTransaction::deserialize(transfer.serialize());
    expect(transfer_roundtrip.nft_id == transfer.nft_id &&
               transfer_roundtrip.from.str() == transfer.from.str() &&
               transfer_roundtrip.to.str() == transfer.to.str() &&
               transfer_roundtrip.signature == transfer.signature &&
               transfer_roundtrip.fee == transfer.fee,
           "NFT transfer payload roundtrips");
    expect_all_strict_prefixes_throw(
        transfer.serialize(),
        [](const geocoin::bytes& value) {
            (void)geocoin::nft::NFTTransferTransaction::deserialize(value);
        },
        "every truncated NFT transfer prefix must be rejected"
    );
    expect(!transfer.serialize_for_signing().empty(), "NFT transfer signing payload is non-empty");
    expect_throws([] {
        geocoin::nft::NFTTransferTransaction::deserialize(geocoin::bytes(31, 0));
    }, "short NFT transfer payload throws");
    auto trailing_transfer = transfer.serialize();
    trailing_transfer.push_back(0);
    expect_throws([&] {
        geocoin::nft::NFTTransferTransaction::deserialize(trailing_transfer);
    }, "NFT transfer payload with trailing bytes throws");

    auto oversized_sig_transfer = transfer;
    oversized_sig_transfer.signature.assign(geocoin::consensus::params::MAX_SIGNATURE_SIZE + 1, 0x55);
    expect_throws([&] {
        geocoin::nft::NFTTransferTransaction::deserialize(oversized_sig_transfer.serialize());
    }, "oversized NFT transfer signature throws");

    auto negative_transfer = transfer;
    negative_transfer.fee = -1;
    expect_throws([&] {
        (void)negative_transfer.serialize();
    }, "negative NFT transfer fee cannot be serialized");
    expect_throws([&] {
        (void)negative_transfer.serialize_for_signing();
    }, "negative NFT transfer fee cannot be included in a signing payload");
    auto invalid_transfer_fee = transfer.serialize();
    std::fill(invalid_transfer_fee.end() - 8, invalid_transfer_fee.end(), 0xff);
    expect_throws([&] {
        geocoin::nft::NFTTransferTransaction::deserialize(invalid_transfer_fee);
    }, "out-of-range serialized NFT transfer fee throws");

    auto maximum_transfer = transfer;
    maximum_transfer.fee = (std::numeric_limits<geocoin::Amount>::max)();
    expect(geocoin::nft::NFTTransferTransaction::deserialize(maximum_transfer.serialize()).fee ==
               maximum_transfer.fee,
           "maximum valid NFT transfer fee roundtrips");

    geocoin::nft::NFTBurnTransaction burn;
    burn.nft_id = hash_from_seed(0x71);
    burn.owner = transfer.from;
    burn.signature = {9, 8, 7};
    burn.fee = geocoin::consensus::params::MIN_TX_FEE * 2;
    const auto burn_roundtrip = geocoin::nft::NFTBurnTransaction::deserialize(burn.serialize());
    expect(burn_roundtrip.nft_id == burn.nft_id &&
               burn_roundtrip.owner.str() == burn.owner.str() &&
               burn_roundtrip.signature == burn.signature &&
               burn_roundtrip.fee == burn.fee,
           "NFT burn payload roundtrips");
    expect_all_strict_prefixes_throw(
        burn.serialize(),
        [](const geocoin::bytes& value) {
            (void)geocoin::nft::NFTBurnTransaction::deserialize(value);
        },
        "every truncated NFT burn prefix must be rejected"
    );
    expect(!burn.serialize_for_signing().empty(), "NFT burn signing payload is non-empty");
    expect_throws([] {
        geocoin::nft::NFTBurnTransaction::deserialize(geocoin::bytes(31, 0));
    }, "short NFT burn payload throws");
    auto trailing_burn = burn.serialize();
    trailing_burn.push_back(0);
    expect_throws([&] {
        geocoin::nft::NFTBurnTransaction::deserialize(trailing_burn);
    }, "NFT burn payload with trailing bytes throws");

    auto negative_burn = burn;
    negative_burn.fee = -1;
    expect_throws([&] {
        (void)negative_burn.serialize();
    }, "negative NFT burn fee cannot be serialized");
    expect_throws([&] {
        (void)negative_burn.serialize_for_signing();
    }, "negative NFT burn fee cannot be signed");
    auto invalid_burn_fee = burn.serialize();
    std::fill(invalid_burn_fee.end() - 8, invalid_burn_fee.end(), 0xff);
    expect_throws([&] {
        geocoin::nft::NFTBurnTransaction::deserialize(invalid_burn_fee);
    }, "out-of-range serialized NFT burn fee throws");

    geocoin::nft::NFTRegistryImpl registry;
    expect(!registry.exists(creation.nft_id), "new registry does not contain NFT");
    expect(registry.get_owner(creation.nft_id).str().empty(), "missing owner lookup returns empty address");
    expect(registry.get_metadata(creation.nft_id).name.empty(), "missing metadata lookup returns default metadata");
    expect(registry.get_owned_nfts(creation.creator_address).empty(), "missing owner index returns empty list");
    expect(!registry.transfer_nft(creation.nft_id, transfer.to), "registry rejects transfer of missing NFT");
    expect(!registry.burn_nft(creation.nft_id), "registry rejects burn of missing NFT");
    expect(registry.register_nft(creation), "registry stores NFT for duplicate checks");
    expect(!registry.register_nft(creation), "registry rejects duplicate NFT registration");
}

} // namespace

int main()
{
    try {
        test_creation_signature_and_metadata_guards();
        test_polygon_and_coordinate_helpers();
        test_cadastral_guards();
        test_cadastral_lookup_without_full_registry_copy();
        test_pol_disabled_and_transfer_burn();
        test_proof_of_location_rules();
        test_registry_indexes_without_sorted_assumption();
        test_registry_index_integrity_across_updates();
        test_nft_serialization_roundtrips_and_bounds();
        std::cout << "NFT validator unit tests passed\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "NFT validator unit tests failed: " << ex.what() << "\n";
        return 1;
    }
}
