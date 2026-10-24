#pragma once

#include "geocoin/types.hpp"
#include "geocoin/geography/wgs84.hpp"
#include "geocoin/nft/geometry.hpp"
#include <optional>
#include <vector>
#include <memory>
#include <map>
#include <unordered_map>
#include <algorithm>
#include <set>
#include <functional>

namespace geocoin::nft {

namespace coordinates { class PreparedGeometry; }
class GeometryIndex;

struct NFTMetadata;
bool metadata_within_consensus_limits(const NFTMetadata& metadata);

/**
 * @brief Enum для типов источников Proof-of-Location
 */
enum class PoLSourceType : uint8_t {
    GPS_NATIVE = 0,      // Встроенный GPS приемник
    NETWORK_CELLULAR = 1, // Триангуляция сотовых вышек
    BEACON_BLE = 2,      // Bluetooth Low Energy маячки
    HYBRID = 3           // Комбинированный способ
};

/**
 * @brief Данные Proof-of-Location встраиваемые в подпись транзакции
 *
 * Все эти данные включаются в канонический NFTCreationTransaction payload.
 * Невозможно изменить ни геоданные, ни остальные поля NFT без переподписания.
 */
struct ProofOfLocationData {
    int64_t latitude_ndeg = 0;  ///< WGS84 latitude, signed nanodegrees
    int64_t longitude_ndeg = 0; ///< WGS84 longitude, signed nanodegrees
    uint64_t timestamp_ms = 0;  ///< Время GPS снимка (unix time в миллисекундах)
    uint32_t gps_accuracy_cm = 0; ///< Точность GPS в сантиметрах
    uint8_t source_type = 0;    ///< Источник данных (PoLSourceType)

    // Сериализация и десериализация
    bytes serialize() const;
    static ProofOfLocationData deserialize(const bytes& data);

    // Вспомогательные методы
    bool is_valid_accuracy() const {
        // Разрешаем только реалистичный диапазон [1см .. 10км].
        return gps_accuracy_cm > 0 && gps_accuracy_cm <= 1000000U;
    }
};

/**
 * @brief Метаданные NFT
 *
 * Содержит всю информацию о NFT
 * Хешируется для получения NFT ID
 */
struct NFTMetadata {
    using GeoPoint = coordinates::Point;
    using GeoRing = coordinates::Ring;
    using GeoPolygon = coordinates::Polygon;
    using Geometry = coordinates::Geometry;

    struct CadastralData {
        std::string cadastral_number;     ///< "77:01:0001001:1234"
        std::string owner_name;           ///< Имя владельца
        std::string land_category;        ///< "ИЖС", "СНТ", "Коммерческая"
        std::string permitted_use;        ///< "Для строительства дома"
        uint64_t area_sqcm_official = 0;   ///< Official area in square centimetres

        Hash256 ownership_document_hash{}; ///< Хеш свидетельства о собственности
        Hash256 technical_plan_hash{};     ///< Хеш технического плана
        std::vector<Hash256> photo_hashes; ///< Фото участка

        Amount market_value = 0;          ///< Рыночная стоимость
        Amount cadastral_value = 0;       ///< Кадастровая стоимость
        uint64_t last_valuation_date = 0; ///< Дата последней оценки

        std::vector<std::string> restrictions; ///< Ограничения/обременения
        bool has_buildings = false;       ///< Есть ли постройки
        uint32_t building_count = 0;      ///< Количество строений
    };

    // Базовые поля
    std::string name;              ///< Название NFT
    std::string description;       ///< Описание
    std::string image_uri;         ///< URI изображения (IPFS/Arweave)

    // Криптографическая привязка к контенту
    // SHA-256(байты исходного изображения) в hex (64 символа)
    // Консенсусное поле: участвует в хеше метаданных и, следовательно, в NFT ID.
    std::string content_hash;      ///< Hex-строка SHA-256 контента изображения

    // Географические данные (полигон)
    // ВАЖНО: используем фиксированную точку для детерминизма на разных процессорах
    // Минимум 3 вершины.
    Geometry geometry;

    // Signed metadata bits retained in the wire format and NFT id. Calculated
    // area comes from the polygon through GeographicLib, never from this field.
    uint64_t area_bits = 0;

    // Метаданные о создании
    uint64_t created_at = 0;           ///< Timestamp создания (unix time в секундах)
    Height block_height = 0;           ///< Высота блока создания
    Hash256 block_hash{};            ///< Хеш блока создания

    // Опциональные поля
    std::string location_name;     ///< Название места (например, "Эйфелева башня")
    std::vector<std::string> tags; ///< Теги для поиска
    std::string creator;           ///< Адрес создателя

    // Кадастровые данные (опционально)
    std::optional<CadastralData> cadastral;

    // Сериализация
    bytes serialize() const;
    static NFTMetadata deserialize(const bytes& data);

    // Вычисление хеша метаданных (для ID NFT)
    Hash256 calculate_hash() const;
};

/**
 * @brief Транзакция создания NFT
 */
struct NFTCreationTransaction {
    // Стандартные поля транзакции
    std::vector<geocoin::byte> inputs;     ///< Ссылки на входы (упрощенная версия)
    std::vector<geocoin::byte> outputs;    ///< Ссылки на выходы

    // NFT специфичные поля
    Hash256 nft_id{};                      ///< Уникальный ID NFT (hash метаданных)
    NFTMetadata metadata;                  ///< Метаданные NFT

    // Роялти
    Address creator_address;               ///< Адрес создателя для роялти
    uint32_t royalty_percent = 0;          ///< Процент роялти (0-10000, где 10000 = 100%)

    // Доказательство местоположения (опционально)
    // Если заполнена, подпись включает GPS данные
    std::optional<ProofOfLocationData> pol_data;

    // Подпись транзакции
    // Для обоих вариантов подпись вычисляется над serialize_for_signing().
    // Поэтому creator_address, royalty_percent, metadata и pol_data связаны
    // одной канонической подписью.
    Signature signature;

    // Сериализация
    bytes serialize() const;
    bytes serialize_for_signing() const;  ///< Сериализация для подписи (без signature поля)
    static NFTCreationTransaction deserialize(const bytes& data);
};

/**
 * @brief Транзакция передачи NFT
 */
struct NFTTransferTransaction {
    Hash256 nft_id{};            ///< ID NFT для передачи
    Address from;                ///< Адрес отправителя
    Address to;                  ///< Адрес получателя
    Signature signature;         ///< Подпись отправителя
    Amount fee = 0;              ///< Комиссия за передачу

    bytes serialize() const;
    bytes serialize_for_signing() const;
    static NFTTransferTransaction deserialize(const bytes& data);
};

/**
 * @brief Транзакция уничтожения NFT
 */
struct NFTBurnTransaction {
    Hash256 nft_id{};            ///< ID NFT для уничтожения
    Address owner;               ///< Адрес владельца
    Signature signature;         ///< Подпись владельца
    Amount fee = 0;              ///< Комиссия за уничтожение

    bytes serialize() const;
    bytes serialize_for_signing() const;
    static NFTBurnTransaction deserialize(const bytes& data);
};

// Read contract used by confirmed registries and temporary block projections.
class NFTRegistryView {
public:
    using CadastralVisitor = std::function<bool(const Hash256&, const NFTMetadata&)>;
    virtual ~NFTRegistryView() = default;
    virtual Address get_owner(const Hash256& nft_id) const = 0;
    virtual NFTMetadata get_metadata(const Hash256& nft_id) const = 0;
    virtual bool exists(const Hash256& nft_id) const = 0;
    // Visits live cadastral records by reference; stops when the visitor returns false.
    virtual bool for_each_cadastral(const CadastralVisitor& visitor) const = 0;
    virtual bool for_each_cadastral_candidate(const NFTMetadata& metadata, const CadastralVisitor& visitor) const;
    virtual std::shared_ptr<const coordinates::PreparedGeometry> prepared_geometry(const Hash256& id) const;
};

/**
 * @brief Реестр NFT - хранит все NFT в системе
 */
class NFTRegistry : public NFTRegistryView {
public:
    NFTRegistry() = default;
    virtual ~NFTRegistry() = default;

    // Основные операции
    virtual bool register_nft(const NFTCreationTransaction& nft_tx) = 0;
    virtual bool transfer_nft(const Hash256& nft_id, const Address& new_owner) = 0;
    virtual bool burn_nft(const Hash256& nft_id) = 0;

    // Поиск по координатам
    virtual std::vector<Hash256> find_by_location(
        int64_t lat_ndeg,
        int64_t lon_ndeg,
        uint32_t radius_m
    ) const = 0;

    // Поиск по тегам
    virtual std::vector<Hash256> find_by_tags(
        const std::vector<std::string>& tags
    ) const = 0;

    // Получить все NFT владельца
    virtual std::vector<Hash256> get_owned_nfts(const Address& owner) const = 0;

    // Получить все NFT (для глобальных проверок, например кадастра)
    virtual std::vector<Hash256> get_all_nfts() const = 0;
};

/**
 * @brief Всемирная реализация NFTRegistry (в памяти + база данных)
 */
class NFTRegistryImpl : public NFTRegistry {
private:
    // Маппинг NFT ID -> Владелец
    std::map<Hash256, Address> owners_;

    // Маппинг NFT ID -> Метаданные
    std::map<Hash256, NFTMetadata> metadata_;

    // Маппинг адреса -> список NFT ID
    std::unordered_map<std::string, std::vector<Hash256>> owner_nfts_;

    // Маппинг тега -> список NFT ID
    std::unordered_map<std::string, std::vector<Hash256>> tag_index_;

    // Derived from metadata; ordinary NFTs do not reserve cadastral land.
    std::set<Hash256> cadastral_ids_;
    std::unordered_map<std::string, std::set<Hash256>> cadastral_numbers_;
    std::unique_ptr<GeometryIndex> geometry_index_;

public:
    NFTRegistryImpl();
    ~NFTRegistryImpl();
    NFTRegistryImpl(NFTRegistryImpl&&) noexcept;
    NFTRegistryImpl& operator=(NFTRegistryImpl&&) noexcept;
    NFTRegistryImpl(const NFTRegistryImpl&) = delete;
    NFTRegistryImpl& operator=(const NFTRegistryImpl&) = delete;

    bool register_nft(const NFTCreationTransaction& nft_tx) override;
    bool transfer_nft(const Hash256& nft_id, const Address& new_owner) override;
    bool burn_nft(const Hash256& nft_id) override;

    Address get_owner(const Hash256& nft_id) const override;
    NFTMetadata get_metadata(const Hash256& nft_id) const override;
    bool exists(const Hash256& nft_id) const override;
    bool for_each_cadastral(const CadastralVisitor& visitor) const override;
    bool for_each_cadastral_candidate(const NFTMetadata& metadata, const CadastralVisitor& visitor) const override;
    std::shared_ptr<const coordinates::PreparedGeometry> prepared_geometry(const Hash256& id) const override;

    std::vector<Hash256> find_by_location(
        int64_t lat_ndeg,
        int64_t lon_ndeg,
        uint32_t radius_m
    ) const override;

    std::vector<Hash256> find_by_tags(
        const std::vector<std::string>& tags
    ) const override;

    std::vector<Hash256> get_owned_nfts(const Address& owner) const override;
    std::vector<Hash256> get_owned_nfts_page(
        const Address& owner,
        size_t offset,
        size_t limit,
        size_t* total_out = nullptr
    ) const;
    std::vector<Hash256> get_all_nfts() const override;
};

} // namespace geocoin::nft
