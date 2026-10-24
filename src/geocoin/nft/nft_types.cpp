#include "geocoin/nft/nft_types.hpp"
#include "geometry_index.hpp"
#include "geocoin/utils/logger.hpp"
#include "geocoin/consensus/consensus.hpp"
#include "geocoin/nft/nft_validator.hpp"
#include "geocoin/nft/geodesy.hpp"
#include "geocoin/geography/wgs84.hpp"
#include "geocoin/utils/varint.hpp"
#include "geocoin/crypto/geo_area.hpp"
#include <algorithm>
#include <bit>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace geocoin::nft {

// ============================================================================
// Вспомогательные функции для сериализации
// Используем формат little-endian + varint.
// ============================================================================

/// Записать uint32 в little-endian формате
static void write_uint32_le(bytes& buf, uint32_t val) {
    buf.push_back((val >> 0) & 0xFF);
    buf.push_back((val >> 8) & 0xFF);
    buf.push_back((val >> 16) & 0xFF);
    buf.push_back((val >> 24) & 0xFF);
}

/// Записать uint64 в little-endian формате
static void write_uint64_le(bytes& buf, uint64_t val) {
    for (int i = 0; i < 8; i++) {
        buf.push_back((val >> (i * 8)) & 0xFF);
    }
}

/// Записать int32 в little-endian формате
static void write_int32_le(bytes& buf, int32_t val) {
    write_uint32_le(buf, static_cast<uint32_t>(val));
}

/// Записать переменную длину (Bitcoin CompactSize формат)
static void write_varint(bytes& buf, uint64_t val) {
    auto varint = geocoin::utils::encode_varint(val);
    buf.insert(buf.end(), varint.begin(), varint.end());
}

/// Записать строку (varint длина + данные)
static void write_string(bytes& buf, const std::string& str) {
    write_varint(buf, str.length());
    buf.insert(buf.end(), str.begin(), str.end());
}

/// Записать массив байт (varint длина + данные)
static void write_bytes(bytes& buf, const bytes& data) {
    write_varint(buf, data.size());
    buf.insert(buf.end(), data.begin(), data.end());
}

/// Прочитать uint32 в little-endian формате
static uint32_t read_uint32_le(const bytes& data, size_t& offset) {
    if (offset + 4 > data.size()) throw std::runtime_error("Buffer underflow");
    const uint32_t val = static_cast<uint32_t>(data[offset]) |
                         (static_cast<uint32_t>(data[offset + 1]) << 8) |
                         (static_cast<uint32_t>(data[offset + 2]) << 16) |
                         (static_cast<uint32_t>(data[offset + 3]) << 24);
    offset += 4;
    return val;
}

/// Прочитать uint64 в little-endian формате
static uint64_t read_uint64_le(const bytes& data, size_t& offset) {
    if (offset + 8 > data.size()) throw std::runtime_error("Buffer underflow");
    uint64_t val = 0;
    for (int i = 0; i < 8; i++) {
        val |= ((uint64_t)data[offset + i] << (i * 8));
    }
    offset += 8;
    return val;
}

static Amount read_amount_le(const bytes& data, size_t& offset, const char* field) {
    const uint64_t value = read_uint64_le(data, offset);
    if (value > static_cast<uint64_t>((std::numeric_limits<Amount>::max)())) {
        throw std::runtime_error(std::string("NFT: invalid ") + field);
    }
    return static_cast<Amount>(value);
}

/// Прочитать int32 в little-endian формате
static int32_t read_int32_le(const bytes& data, size_t& offset) {
    return std::bit_cast<int32_t>(read_uint32_le(data, offset));
}

/// Прочитать переменную длину (Bitcoin CompactSize формат)
static uint64_t read_varint(const bytes& data, size_t& offset) {
    return geocoin::utils::decode_varint(data, offset);
}

/// Прочитать строку (varint длина + данные)
static std::string read_string_bounded(const bytes& data, size_t& offset, size_t max_len) {
    uint64_t len = read_varint(data, offset);
    if (len > max_len) {
        throw std::runtime_error("String length exceeds limit");
    }
    if (len > data.size() - offset) {
        throw std::runtime_error("Buffer underflow");
    }
    std::string result(data.begin() + offset, data.begin() + offset + len);
    offset += len;
    return result;
}

/// Прочитать массив байт (varint длина + данные)
static bytes read_bytes_bounded(const bytes& data, size_t& offset, size_t max_len) {
    uint64_t len = read_varint(data, offset);
    if (len > max_len) {
        throw std::runtime_error("Bytes length exceeds limit");
    }
    if (len > data.size() - offset) {
        throw std::runtime_error("Buffer underflow");
    }
    bytes result(data.begin() + offset, data.begin() + offset + len);
    offset += len;
    return result;
}

// ============================================================================
// ProofOfLocationData - данные доказательства местоположения
// ============================================================================

bytes ProofOfLocationData::serialize() const {
    bytes result;

    // Координаты в наноградусах (deg * 1e9)
    write_uint64_le(result, std::bit_cast<uint64_t>(latitude_ndeg));
    write_uint64_le(result, std::bit_cast<uint64_t>(longitude_ndeg));

    // Временная метка GPS в миллисекундах
    write_uint64_le(result, timestamp_ms);

    // Точность GPS в сантиметрах (uint32 LE)
    write_uint32_le(result, gps_accuracy_cm);

    // Тип источника данных (GPS_NATIVE, NETWORK_CELLULAR, BEACON_BLE, HYBRID)
    result.push_back(source_type);

    return result;
}

ProofOfLocationData ProofOfLocationData::deserialize(const bytes& data) {
    if (data.size() != 29) {
        throw std::runtime_error("ProofOfLocationData: invalid serialized size");
    }

    ProofOfLocationData result;
    size_t offset = 0;

    result.latitude_ndeg = std::bit_cast<int64_t>(read_uint64_le(data, offset));
    result.longitude_ndeg = std::bit_cast<int64_t>(read_uint64_le(data, offset));
    result.timestamp_ms = read_uint64_le(data, offset);
    result.gps_accuracy_cm = read_uint32_le(data, offset);
    result.source_type = data[offset++];

    return result;
}

// ============================================================================
// NFTMetadata - метаданные NFT
// Содержит всю информацию о NFT, хешируется для получения NFT ID
// ============================================================================

bytes NFTMetadata::serialize() const {
    bytes result;

    // Базовые поля (используем varint для компактности)
    write_string(result, name);           // Название NFT
    write_string(result, description);    // Описание
    write_string(result, image_uri);      // URI изображения (IPFS/Arweave)
    write_string(result, content_hash);   // SHA-256 контента изображения (hex)

    const auto canonical = coordinates::canonicalize_geometry(geometry);
    write_varint(result, canonical.polygons.size());
    for (const auto& part : canonical.polygons) {
        write_varint(result, 1 + part.holes.size());
        const auto write_ring = [&](const GeoRing& ring) {
            write_varint(result, ring.size());
            for (const auto& point : ring) {
                write_uint64_le(result, std::bit_cast<uint64_t>(point.lat_ndeg));
                write_uint64_le(result, std::bit_cast<uint64_t>(point.lon_ndeg));
            }
        };
        write_ring(part.exterior);
        for (const auto& hole : part.holes) write_ring(hole);
    }

    // Консенсусное представление площади (IEEE754 double bits)
    write_uint64_le(result, area_bits);

    // Метаданные о создании
    write_uint64_le(result, created_at);     // Unix-время создания
    write_uint64_le(result, block_height);   // Высота блока создания
    result.insert(result.end(), block_hash.begin(), block_hash.end()); // Хеш блока (32 байта)

    // Опциональные поля
    write_string(result, location_name);  // Название места (например, "Эйфелева башня")

    // Теги для поиска
    write_varint(result, tags.size());
    for (const auto& tag : tags) {
        write_string(result, tag);
    }

    // Адрес создателя
    write_string(result, creator);

    // Кадастровые данные (опционально)
    if (cadastral.has_value()) {
        result.push_back(1);
        const auto& c = cadastral.value();
        if (c.market_value < 0 || c.cadastral_value < 0) {
            throw std::runtime_error("NFTMetadata: cadastral values must be non-negative");
        }
        write_string(result, c.cadastral_number);
        write_string(result, c.owner_name);
        write_string(result, c.land_category);
        write_string(result, c.permitted_use);
        write_uint64_le(result, c.area_sqcm_official);

        result.insert(result.end(), c.ownership_document_hash.begin(), c.ownership_document_hash.end());
        result.insert(result.end(), c.technical_plan_hash.begin(), c.technical_plan_hash.end());

        write_varint(result, c.photo_hashes.size());
        for (const auto& h : c.photo_hashes) {
            result.insert(result.end(), h.begin(), h.end());
        }

        write_uint64_le(result, static_cast<uint64_t>(c.market_value));
        write_uint64_le(result, static_cast<uint64_t>(c.cadastral_value));
        write_uint64_le(result, c.last_valuation_date);

        write_varint(result, c.restrictions.size());
        for (const auto& r : c.restrictions) {
            write_string(result, r);
        }

        result.push_back(c.has_buildings ? 1 : 0);
        write_uint32_le(result, c.building_count);
    } else {
        result.push_back(0);
    }

    return result;
}

static NFTMetadata deserialize_metadata(const bytes& data, size_t& offset) {
    NFTMetadata result;

    // Базовые поля
    result.name = read_string_bounded(data, offset, consensus::params::MAX_NFT_NAME);
    result.description = read_string_bounded(data, offset, consensus::params::MAX_NFT_DESCRIPTION);
    result.image_uri = read_string_bounded(data, offset, consensus::params::MAX_NFT_IMAGE_URI);
    result.content_hash = read_string_bounded(data, offset, 64);

    const auto part_count = read_varint(data, offset);
    if (part_count == 0 || part_count > coordinates::kMaxGeometryParts)
        throw std::runtime_error("NFTMetadata: invalid part count");
    result.geometry.polygons.clear();
    size_t total_points = 0, total_rings = 0;
    for (uint64_t part = 0; part < part_count; ++part) {
        const auto ring_count = read_varint(data, offset);
        if (ring_count == 0 || ring_count > coordinates::kMaxGeometryRings - total_rings)
            throw std::runtime_error("NFTMetadata: invalid ring count");
        total_rings += ring_count;
        NFTMetadata::GeoPolygon polygon;
        for (uint64_t ring_index = 0; ring_index < ring_count; ++ring_index) {
            const auto count = read_varint(data, offset);
            if (count < coordinates::kMinRingVertices || count > coordinates::kMaxGeometryVertices - total_points)
                throw std::runtime_error("NFTMetadata: invalid total point count");
            total_points += count;
            NFTMetadata::GeoRing ring;
            ring.reserve(count);
            for (uint64_t i = 0; i < count; ++i) {
                const int64_t lat = std::bit_cast<int64_t>(read_uint64_le(data, offset));
                const int64_t lon = std::bit_cast<int64_t>(read_uint64_le(data, offset));
                if (!geography::Wgs84Point{lat, lon}.is_valid())
                    throw std::runtime_error("NFTMetadata: invalid WGS84 coordinate");
                ring.push_back({lat, lon});
            }
            if (ring_index == 0) polygon.exterior = std::move(ring);
            else polygon.holes.push_back(std::move(ring));
        }
        result.geometry.polygons.push_back(std::move(polygon));
    }
    if (coordinates::canonicalize_geometry(result.geometry) != result.geometry)
        throw std::runtime_error("NFTMetadata: noncanonical geometry");

    // Площадь
    result.area_bits = read_uint64_le(data, offset);

    // Метаданные создания
    result.created_at = read_uint64_le(data, offset);
    result.block_height = read_uint64_le(data, offset);

    if (offset + 32 > data.size()) throw std::runtime_error("NFTMetadata: Недостаточно данных для block_hash");
    std::copy(data.begin() + offset, data.begin() + offset + 32, result.block_hash.begin());
    offset += 32;

    // Опциональные поля
    result.location_name = read_string_bounded(data, offset, consensus::params::MAX_NFT_LOCATION_NAME);

    // Теги
    uint64_t tag_count = read_varint(data, offset);
    if (tag_count > consensus::params::MAX_NFT_TAGS) {
        throw std::runtime_error("NFTMetadata: too many tags");
    }
    result.tags.reserve(tag_count);
    for (uint64_t i = 0; i < tag_count; i++) {
        result.tags.push_back(read_string_bounded(data, offset, consensus::params::MAX_NFT_TAG_LEN));
    }

    // Создатель
    result.creator = read_string_bounded(data, offset, consensus::params::MAX_ADDRESS_SIZE);

    // Кадастровые данные. Флаг обязателен: без него граница метаданных внутри
    // NFT_CREATE была неоднозначной и прямой parser принимал хвостовые байты.
    if (offset >= data.size()) {
        throw std::runtime_error("NFTMetadata: missing cadastral flag");
    }
    uint8_t has_cadastral = data[offset++];
    if (has_cadastral > 1) {
        throw std::runtime_error("NFTMetadata: invalid cadastral flag");
    }
    if (has_cadastral) {
            NFTMetadata::CadastralData c;
            c.cadastral_number = read_string_bounded(data, offset, consensus::params::MAX_CADASTRAL_NUMBER);
            c.owner_name = read_string_bounded(data, offset, consensus::params::MAX_CADASTRAL_OWNER_NAME);
            c.land_category = read_string_bounded(data, offset, consensus::params::MAX_LAND_CATEGORY);
            c.permitted_use = read_string_bounded(data, offset, consensus::params::MAX_PERMITTED_USE);
            c.area_sqcm_official = read_uint64_le(data, offset);

            if (offset + 32 > data.size()) throw std::runtime_error("NFTMetadata: insufficient ownership_document_hash");
            std::copy(data.begin() + offset, data.begin() + offset + 32, c.ownership_document_hash.begin());
            offset += 32;
            if (offset + 32 > data.size()) throw std::runtime_error("NFTMetadata: insufficient technical_plan_hash");
            std::copy(data.begin() + offset, data.begin() + offset + 32, c.technical_plan_hash.begin());
            offset += 32;

            uint64_t photo_count = read_varint(data, offset);
            if (photo_count > consensus::params::MAX_PHOTO_HASHES) {
                throw std::runtime_error("NFTMetadata: too many photo hashes");
            }
            c.photo_hashes.clear();
            c.photo_hashes.reserve(photo_count);
            for (uint64_t i = 0; i < photo_count; i++) {
                if (offset + 32 > data.size()) throw std::runtime_error("NFTMetadata: insufficient photo_hash");
                Hash256 h;
                std::copy(data.begin() + offset, data.begin() + offset + 32, h.begin());
                offset += 32;
                c.photo_hashes.push_back(h);
            }

            c.market_value = read_amount_le(data, offset, "market value");
            c.cadastral_value = read_amount_le(data, offset, "cadastral value");
            c.last_valuation_date = read_uint64_le(data, offset);

            uint64_t restr_count = read_varint(data, offset);
            if (restr_count > consensus::params::MAX_RESTRICTIONS) {
                throw std::runtime_error("NFTMetadata: too many restrictions");
            }
            c.restrictions.clear();
            c.restrictions.reserve(restr_count);
            for (uint64_t i = 0; i < restr_count; i++) {
                c.restrictions.push_back(read_string_bounded(data, offset, consensus::params::MAX_RESTRICTION_LEN));
            }

            if (offset + 1 > data.size()) throw std::runtime_error("NFTMetadata: insufficient has_buildings");
            const uint8_t has_buildings = data[offset++];
            if (has_buildings > 1) {
                throw std::runtime_error("NFTMetadata: invalid has_buildings flag");
            }
            c.has_buildings = has_buildings == 1;
            c.building_count = read_uint32_le(data, offset);

            result.cadastral = std::move(c);
    }

    return result;
}

NFTMetadata NFTMetadata::deserialize(const bytes& data) {
    size_t offset = 0;
    NFTMetadata result = deserialize_metadata(data, offset);
    if (offset != data.size()) {
        throw std::runtime_error("NFTMetadata: trailing data");
    }
    return result;
}

Hash256 NFTMetadata::calculate_hash() const {
    auto serialized = serialize();
    return geocoin::crypto::sha256(serialized);
}

// ============================================================================
// NFTCreationTransaction - транзакция создания NFT
// ============================================================================

bytes NFTCreationTransaction::serialize() const {
    bytes result;

    // NFT ID (32 байта) - уникальный идентификатор NFT
    result.insert(result.end(), nft_id.begin(), nft_id.end());

    // Метаданные NFT
    auto metadata_bytes = metadata.serialize();
    result.insert(result.end(), metadata_bytes.begin(), metadata_bytes.end());

    // Адрес создателя (для роялти)
    write_string(result, creator_address.str());

    // Процент роялти (0-10000, где 10000 = 100%)
    write_uint32_le(result, royalty_percent);

    // Доказательство местоположения (опционально)
    // Если заполнена, подпись включает GPS данные
    if (pol_data.has_value()) {
        result.push_back(1);  // Флаг наличия PoL данных
        auto pol_bytes = pol_data->serialize();
        result.insert(result.end(), pol_bytes.begin(), pol_bytes.end());
    } else {
        result.push_back(0);  // Нет PoL данных
    }

    // Подпись транзакции
    write_bytes(result, signature);

    return result;
}

bytes NFTCreationTransaction::serialize_for_signing() const {
    bytes result;

    // Сериализуем всё кроме поля signature (для создания подписи)
    result.insert(result.end(), nft_id.begin(), nft_id.end());

    auto metadata_bytes = metadata.serialize();
    result.insert(result.end(), metadata_bytes.begin(), metadata_bytes.end());

    write_string(result, creator_address.str());
    write_uint32_le(result, royalty_percent);

    // Доказательство местоположения
    if (pol_data.has_value()) {
        result.push_back(1);
        auto pol_bytes = pol_data->serialize();
        result.insert(result.end(), pol_bytes.begin(), pol_bytes.end());
    } else {
        result.push_back(0);
    }

    return result;
}

NFTCreationTransaction NFTCreationTransaction::deserialize(const bytes& data) {
    NFTCreationTransaction result;
    size_t offset = 0;

    // NFT ID (32 байта)
    if (offset + 32 > data.size()) throw std::runtime_error("NFTCreationTransaction: Недостаточно данных для nft_id");
    std::copy(data.begin() + offset, data.begin() + offset + 32, result.nft_id.begin());
    offset += 32;

    // Метаданные имеют обязательный конечный cadastral-флаг, поэтому parser
    // может вернуть точную границу без parse->serialize эвристики.
    result.metadata = deserialize_metadata(data, offset);

    // creator_address
    result.creator_address = Address(read_string_bounded(data, offset, consensus::params::MAX_ADDRESS_SIZE));

    // royalty_percent
    result.royalty_percent = read_uint32_le(data, offset);

    // pol_data
    if (offset + 1 > data.size()) {
        throw std::runtime_error("NFTCreationTransaction: missing pol flag");
    }
    uint8_t has_pol = data[offset++];
    if (has_pol > 1) {
        throw std::runtime_error("NFTCreationTransaction: invalid pol flag");
    }
    if (has_pol) {
        constexpr size_t kProofOfLocationSerializedSize = 29;
        if (offset + kProofOfLocationSerializedSize > data.size()) {
            throw std::runtime_error("NFTCreationTransaction: insufficient PoL bytes");
        }
        bytes pol_bytes(data.begin() + offset, data.begin() + offset + kProofOfLocationSerializedSize);
        result.pol_data = ProofOfLocationData::deserialize(pol_bytes);
        offset += kProofOfLocationSerializedSize;
    }

    // signature
    result.signature = read_bytes_bounded(data, offset, consensus::params::MAX_SIGNATURE_SIZE);
    if (offset != data.size()) {
        throw std::runtime_error("NFTCreationTransaction: trailing data");
    }
    return result;
}

// ============================================================================
// NFTTransferTransaction - транзакция передачи NFT
// ============================================================================

bytes NFTTransferTransaction::serialize() const {
    if (fee < 0) {
        throw std::runtime_error("NFTTransferTransaction: fee must be non-negative");
    }
    bytes result;
    result.insert(result.end(), nft_id.begin(), nft_id.end());
    write_string(result, from.str());
    write_string(result, to.str());
    write_bytes(result, signature);
    write_uint64_le(result, fee);
    return result;
}

bytes NFTTransferTransaction::serialize_for_signing() const {
    if (fee < 0) {
        throw std::runtime_error("NFTTransferTransaction: fee must be non-negative");
    }
    bytes result;
    result.insert(result.end(), nft_id.begin(), nft_id.end());
    write_string(result, from.str());
    write_string(result, to.str());
    write_uint64_le(result, fee);
    return result;
}

NFTTransferTransaction NFTTransferTransaction::deserialize(const bytes& data) {
    NFTTransferTransaction result;
    size_t offset = 0;

    if (offset + 32 > data.size()) {
        throw std::runtime_error("NFTTransferTransaction: insufficient nft_id");
    }
    std::copy(data.begin() + offset, data.begin() + offset + 32, result.nft_id.begin());
    offset += 32;

    result.from = Address(read_string_bounded(data, offset, consensus::params::MAX_ADDRESS_SIZE));
    result.to = Address(read_string_bounded(data, offset, consensus::params::MAX_ADDRESS_SIZE));
    result.signature = read_bytes_bounded(data, offset, consensus::params::MAX_SIGNATURE_SIZE);
    result.fee = read_amount_le(data, offset, "transfer fee");
    if (offset != data.size()) {
        throw std::runtime_error("NFTTransferTransaction: trailing data");
    }
    return result;
}

// ============================================================================
// NFTBurnTransaction - транзакция уничтожения NFT
// ============================================================================

bytes NFTBurnTransaction::serialize() const {
    if (fee < 0) {
        throw std::runtime_error("NFTBurnTransaction: fee must be non-negative");
    }
    bytes result;
    result.insert(result.end(), nft_id.begin(), nft_id.end());
    write_string(result, owner.str());
    write_bytes(result, signature);
    write_uint64_le(result, fee);
    return result;
}

bytes NFTBurnTransaction::serialize_for_signing() const {
    if (fee < 0) {
        throw std::runtime_error("NFTBurnTransaction: fee must be non-negative");
    }
    bytes result;
    result.insert(result.end(), nft_id.begin(), nft_id.end());
    write_string(result, owner.str());
    write_uint64_le(result, fee);
    return result;
}

NFTBurnTransaction NFTBurnTransaction::deserialize(const bytes& data) {
    NFTBurnTransaction result;
    size_t offset = 0;

    if (offset + 32 > data.size()) {
        throw std::runtime_error("NFTBurnTransaction: insufficient nft_id");
    }
    std::copy(data.begin() + offset, data.begin() + offset + 32, result.nft_id.begin());
    offset += 32;

    result.owner = Address(read_string_bounded(data, offset, consensus::params::MAX_ADDRESS_SIZE));
    result.signature = read_bytes_bounded(data, offset, consensus::params::MAX_SIGNATURE_SIZE);
    result.fee = read_amount_le(data, offset, "burn fee");
    if (offset != data.size()) {
        throw std::runtime_error("NFTBurnTransaction: trailing data");
    }
    return result;
}

// ============================================================================
// NFTRegistry - реестр NFT в системе
// ============================================================================

NFTRegistryImpl::NFTRegistryImpl() : geometry_index_(std::make_unique<GeometryIndex>()) {}
NFTRegistryImpl::~NFTRegistryImpl() = default;
NFTRegistryImpl::NFTRegistryImpl(NFTRegistryImpl&&) noexcept = default;
NFTRegistryImpl& NFTRegistryImpl::operator=(NFTRegistryImpl&&) noexcept = default;
bool NFTRegistryView::for_each_cadastral_candidate(const NFTMetadata&, const CadastralVisitor& visitor) const {
    return for_each_cadastral(visitor);
}
std::shared_ptr<const coordinates::PreparedGeometry> NFTRegistryView::prepared_geometry(const Hash256& id) const {
    if (!exists(id)) return {};
    return std::make_shared<const coordinates::PreparedGeometry>(get_metadata(id).geometry);
}

bool NFTRegistryImpl::register_nft(const NFTCreationTransaction& nft_tx) {
    // Проверка что NFT с таким ID еще не существует
    if (exists(nft_tx.nft_id)) {
        GEO_LOG_ERROR("nft", "NFT с таким ID уже существует");
        return false;
    }

    // Регистрация владельца
    owners_[nft_tx.nft_id] = nft_tx.creator_address;

    // Сохранение метаданных
    metadata_[nft_tx.nft_id] = nft_tx.metadata;
    auto& stored = metadata_[nft_tx.nft_id];
    stored.geometry = coordinates::canonicalize_geometry(stored.geometry);
    geometry_index_->insert(nft_tx.nft_id, stored.geometry);
    if (stored.cadastral) cadastral_numbers_[stored.cadastral->cadastral_number].insert(nft_tx.nft_id);
    if (nft_tx.metadata.cadastral) cadastral_ids_.insert(nft_tx.nft_id);

    // Добавление в список NFT владельца
    owner_nfts_[nft_tx.creator_address.str()].push_back(nft_tx.nft_id);

    // Индексация по тегам для быстрого поиска
    std::set<std::string> indexed_tags;
    for (const auto& tag : nft_tx.metadata.tags) {
        if (indexed_tags.insert(tag).second) {
            tag_index_[tag].push_back(nft_tx.nft_id);
        }
    }

    GEO_LOG_INFO("nft", "NFT успешно зарегистрирован");
    return true;
}

bool NFTRegistryImpl::transfer_nft(const Hash256& nft_id, const Address& new_owner) {
    // Проверка существования NFT
    if (!exists(nft_id)) {
        GEO_LOG_ERROR("nft", "NFT не существует");
        return false;
    }

    // Получение текущего владельца
    Address old_owner = owners_[nft_id];

    // Обновление владельца
    owners_[nft_id] = new_owner;

    // Обновление списка NFT старого владельца
    auto& old_list = owner_nfts_[old_owner.str()];
    old_list.erase(std::remove(old_list.begin(), old_list.end(), nft_id), old_list.end());

    // Добавление в список NFT нового владельца
    owner_nfts_[new_owner.str()].push_back(nft_id);

    GEO_LOG_INFO("nft", "NFT успешно передан новому владельцу");
    return true;
}

bool NFTRegistryImpl::burn_nft(const Hash256& nft_id) {
    // Проверка существования NFT
    if (!exists(nft_id)) {
        GEO_LOG_ERROR("nft", "NFT не существует");
        return false;
    }

    Address owner = owners_[nft_id];

    // Удаление из маппинга владельцев
    owners_.erase(nft_id);

    // Удаление метаданных
    NFTMetadata metadata = metadata_[nft_id];
    geometry_index_->erase(nft_id);
    if (metadata.cadastral) {
        auto it = cadastral_numbers_.find(metadata.cadastral->cadastral_number);
        if (it != cadastral_numbers_.end()) {
            it->second.erase(nft_id);
            if (it->second.empty()) cadastral_numbers_.erase(it);
        }
    }
    metadata_.erase(nft_id);
    cadastral_ids_.erase(nft_id);

    // Удаление из списка NFT владельца
    auto& owner_list = owner_nfts_[owner.str()];
    owner_list.erase(std::remove(owner_list.begin(), owner_list.end(), nft_id), owner_list.end());

    // Удаление из индексов тегов
    for (const auto& tag : metadata.tags) {
        auto& tag_list = tag_index_[tag];
        tag_list.erase(std::remove(tag_list.begin(), tag_list.end(), nft_id), tag_list.end());
    }

    GEO_LOG_INFO("nft", "NFT уничтожен");
    return true;
}

Address NFTRegistryImpl::get_owner(const Hash256& nft_id) const {
    auto it = owners_.find(nft_id);
    if (it == owners_.end()) {
        return Address();
    }
    return it->second;
}

NFTMetadata NFTRegistryImpl::get_metadata(const Hash256& nft_id) const {
    auto it = metadata_.find(nft_id);
    if (it == metadata_.end()) {
        return NFTMetadata();
    }
    return it->second;
}

bool NFTRegistryImpl::exists(const Hash256& nft_id) const {
    return owners_.find(nft_id) != owners_.end();
}

bool NFTRegistryImpl::for_each_cadastral(const CadastralVisitor& visitor) const {
    for (const auto& id : cadastral_ids_) {
        if (!visitor(id, metadata_.at(id))) return false;
    }
    return true;
}

bool NFTRegistryImpl::for_each_cadastral_candidate(const NFTMetadata& metadata, const CadastralVisitor& visitor) const {
    auto ids = geometry_index_->candidates(coordinates::geometry_boxes(metadata.geometry));
    if (metadata.cadastral) {
        if (const auto it = cadastral_numbers_.find(metadata.cadastral->cadastral_number); it != cadastral_numbers_.end())
            ids.insert(it->second.begin(), it->second.end());
    }
    for (const auto& id : ids)
        if (cadastral_ids_.contains(id) && !visitor(id, metadata_.at(id))) return false;
    return true;
}
std::shared_ptr<const coordinates::PreparedGeometry> NFTRegistryImpl::prepared_geometry(const Hash256& id) const {
    if (!exists(id)) return {};
    return geometry_index_->prepare(id, metadata_.at(id).geometry);
}
std::vector<Hash256> NFTRegistryImpl::find_by_location(int64_t lat_ndeg, int64_t lon_ndeg, uint32_t radius_m) const {
    std::vector<Hash256> result;
    if (!geography::Wgs84Point{lat_ndeg, lon_ndeg}.is_valid()) return result;
    const NFTMetadata::GeoPoint point{lat_ndeg, lon_ndeg};
    for (const auto& id : geometry_index_->candidates(coordinates::radius_boxes(point, radius_m))) {
        const auto distance = prepared_geometry(id)->distance_m(point, radius_m);
        if (distance && *distance <= radius_m) result.push_back(id);
    }
    return result;
}

std::vector<Hash256> NFTRegistryImpl::find_by_tags(
    const std::vector<std::string>& tags
) const {
    std::vector<Hash256> result;

    if (tags.empty()) {
        return result;
    }

    // Найти NFT с первым тегом
    auto it = tag_index_.find(tags[0]);
    if (it != tag_index_.end()) {
        result = it->second;
    }

    // Пересечь со следующими тегами (AND логика). Индексы тегов хранят порядок
    // регистрации, а не сортировку по Hash256, поэтому std::set_intersection
    // здесь теряет элементы на несортированных списках.
    for (size_t i = 1; i < tags.size(); i++) {
        auto it2 = tag_index_.find(tags[i]);
        if (it2 == tag_index_.end()) {
            result.clear();
            break;
        }

        std::vector<Hash256> intersection;
        for (const auto& id : result) {
            if (std::find(it2->second.begin(), it2->second.end(), id) != it2->second.end()) {
                intersection.push_back(id);
            }
        }
        result = intersection;
    }

    return result;
}

std::vector<Hash256> NFTRegistryImpl::get_owned_nfts(const Address& owner) const {
    auto it = owner_nfts_.find(owner.str());
    if (it == owner_nfts_.end()) {
        return std::vector<Hash256>();
    }
    return it->second;
}

std::vector<Hash256> NFTRegistryImpl::get_owned_nfts_page(
    const Address& owner,
    size_t offset,
    size_t limit,
    size_t* total_out
) const {
    auto it = owner_nfts_.find(owner.str());
    if (it == owner_nfts_.end()) {
        if (total_out) {
            *total_out = 0;
        }
        return {};
    }
    const auto& ids = it->second;
    if (total_out) {
        *total_out = ids.size();
    }
    if (limit == 0 || offset >= ids.size()) {
        return {};
    }
    const size_t count = std::min(limit, ids.size() - offset);
    return std::vector<Hash256>(ids.begin() + static_cast<std::ptrdiff_t>(offset),
                                ids.begin() + static_cast<std::ptrdiff_t>(offset + count));
}

std::vector<Hash256> NFTRegistryImpl::get_all_nfts() const {
    std::vector<Hash256> result;
    result.reserve(metadata_.size());
    for (const auto& [id, _] : metadata_) {
        result.push_back(id);
    }
    return result;
}

} // namespace geocoin::nft
