#pragma once

#include "geocoin/nft/nft_types.hpp"
#include "geocoin/types.hpp"

namespace geocoin::nft {

/**
 * @brief Валидатор NFT транзакций
 *
 * Проверяет:
 * 1. Синтаксис транзакции
 * 2. Координаты (валидный полигон)
 * 3. Подпись транзакции
 * 4. Proof-of-Location (если требуется)
 */
class NFTValidator {
public:
    /**
     * @brief Валидировать транзакцию создания NFT
     *
     * Проверяет:
     * - Метаданные не пусты
     * - Координаты образуют валидный полигон
     * - NFT с таким ID еще не существует
     * - Подпись корректна
     * - GPS данные совпадают с местом (если требуется PoL)
     * - Время GPS не слишком старое (макс 1 час)
     * - Точность GPS достаточна (не более 100м по умолчанию)
     */
    static bool validate_nft_creation(
        const NFTCreationTransaction& nft_tx,
        const NFTRegistryView& registry
    );

    /**
     * @brief Валидировать транзакцию передачи NFT
     *
     * Проверяет:
     * - NFT существует
     * - Отправитель является владельцем
     * - Подпись корректна
     */
    static bool validate_nft_transfer(
        const NFTTransferTransaction& tx,
        const NFTRegistryView& registry
    );

    /**
     * @brief Валидировать транзакцию уничтожения NFT
     *
     * Проверяет:
     * - NFT существует
     * - Владелец подписал транзакцию
     * - Подпись корректна
     */
    static bool validate_nft_burn(
        const NFTBurnTransaction& tx,
        const NFTRegistryView& registry
    );

    // State-dependent rules for already validated payloads. Shared by full
    // validation and mining selection; no repeated geometry/signature checks.
    static bool validate_creation_state(const NFTCreationTransaction& tx, const NFTRegistryView& registry);
    static bool validate_transfer_state(const NFTTransferTransaction& tx, const NFTRegistryView& registry);
    static bool validate_burn_state(const NFTBurnTransaction& tx, const NFTRegistryView& registry);

    // Cadastral ownership transfers consume the old NFT.
    static bool transfer_burns_nft(const NFTMetadata& metadata) { return metadata.cadastral.has_value(); }

    /**
     * @brief Проверить все детерминированные правила Proof-of-Location.
     *
     * Функция доступна отдельно, чтобы правила отключенного до активации
     * формата можно было проверять без изменения сетевых параметров.
     */
    static bool validate_proof_of_location(
        const ProofOfLocationData& pol_data,
        const NFTMetadata& metadata
    );

private:
    /**
     * @brief Проверить что координаты образуют валидный полигон
     */
    static bool is_valid_polygon(const NFTMetadata& metadata);

    /**
     * @brief Проверить что GPS координаты совпадают с целевым местом
     *
     * Требует, чтобы GPS был внутри полигона или в пределах точности GPS + 10м до любой вершины.
     */
    static bool validate_pol_location(
        const ProofOfLocationData& pol_data,
        const NFTMetadata& metadata
    );

    /**
     * @brief Проверить что время GPS согласовано с временем создания NFT
     *
     * Проверяет, что GPS timestamp:
     * - не позже reference_time_ms
     * - не старше 1 часа относительно reference_time_ms
     */
    static bool validate_pol_timestamp(
        const ProofOfLocationData& pol_data,
        uint64_t reference_time_ms
    );

    /**
     * @brief Проверить что точность GPS достаточна
     *
     * По умолчанию не более 100м (10000 см)
     */
    static bool validate_pol_accuracy(const ProofOfLocationData& pol_data);
};

/**
 * @brief Утилиты для работы с координатами
 */
namespace coordinates {

/**
 * @brief Рассчитать расстояние между двумя точками в метрах
 *
 * Shortest WGS84 geodesic, GeographicLib 2.3.0, rounded to whole metres.
 * Invalid coordinates return INT32_MAX. Input is signed nanodegrees.
 */
int32_t calculate_distance_m(
    int64_t lat1_ndeg,
    int64_t lon1_ndeg,
    int64_t lat2_ndeg,
    int64_t lon2_ndeg
);

/**
 * @brief Проверить что точка находится внутри полигона
 *
 * Smaller WGS84 geodesic region; boundary is included.
 */
bool point_in_polygon(
    const std::vector<NFTMetadata::GeoPoint>& polygon,
    int64_t lat_point_ndeg,
    int64_t lon_point_ndeg
);

/**
 * @brief Проверить пересечение двух полигонов (строгое пересечение)
 *
 * Interior overlap is rejected; shared boundaries/vertices are allowed.
 */
bool polygons_intersect(
    const std::vector<NFTMetadata::GeoPoint>& p1,
    const std::vector<NFTMetadata::GeoPoint>& p2
);

} // namespace coordinates

} // namespace geocoin::nft
