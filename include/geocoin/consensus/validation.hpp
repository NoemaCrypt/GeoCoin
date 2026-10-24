#pragma once

#include <string>
#include <cstdint>

namespace geocoin::consensus {

/**
 * Режим валидации
 */
enum class ValidationMode {
    FULL,           // Полная валидация (для майнинга и relay)
    HEADER_ONLY     // Структура блока без повторной проверки подписей транзакций
};

/**
 * Результат валидации
 */
enum class ValidationResult {
    VALID,          // Блок/транзакция валидны
    INVALID,        // Невалидны (нарушение правил консенсуса)
    ERROR_STATE,    // Ошибка (проблема с данными, не DoS)
    MISSING_INPUTS  // Отсутствуют входы (для транзакций)
};

/**
 * Причина отклонения
 */
enum class RejectCode : uint8_t {
    MALFORMED = 0x01,           // Неверный формат
    INVALID = 0x10,             // Нарушение правил консенсуса
    OBSOLETE = 0x11,            // Устаревшая версия
    DUPLICATE = 0x12,           // Дубликат
    NONSTANDARD = 0x40,         // Нестандартная транзакция
    DUST = 0x41,                // Dust-выход
    INSUFFICIENTFEE = 0x42,     // Недостаточная комиссия
    CHECKPOINT = 0x43           // Несовпадение checkpoint
};

/**
 * Состояние валидации
 * Аналог CValidationState из Bitcoin
 *
 * Отслеживает результат валидации блока/транзакции
 * и причину отклонения.
 */
class ValidationState {
public:
    ValidationState() = default;

    // Установить состояние "валидно"
    bool valid() {
        result_ = ValidationResult::VALID;
        return true;
    }

    // Установить состояние "невалидно" (нарушение консенсуса)
    bool invalid(
        RejectCode code,
        const std::string& reject_reason = "",
        const std::string& debug_message = "",
        bool dos = false
    ) {
        result_ = ValidationResult::INVALID;
        reject_code_ = code;
        reject_reason_ = reject_reason;
        debug_message_ = debug_message;
        dos_ban_ = dos;
        return false;
    }

    // Установить состояние "ошибка" (не DoS)
    bool error(const std::string& reject_reason = "") {
        result_ = ValidationResult::ERROR_STATE;
        reject_reason_ = reject_reason;
        return false;
    }

    // Установить состояние "отсутствуют входы"
    bool missing_inputs() {
        result_ = ValidationResult::MISSING_INPUTS;
        return false;
    }

    // Проверить, валидно ли состояние
    bool is_valid() const {
        return result_ == ValidationResult::VALID;
    }

    // Проверить, невалидно ли состояние
    bool is_invalid() const {
        return result_ == ValidationResult::INVALID;
    }

    // Проверить, ошибка ли
    bool is_error() const {
        return result_ == ValidationResult::ERROR_STATE;
    }

    // Проверить, отсутствуют ли входы
    bool is_missing_inputs() const {
        return result_ == ValidationResult::MISSING_INPUTS;
    }

    // Нужно ли банить пира (DoS)
    bool should_ban() const {
        return dos_ban_;
    }

    // Получить код отклонения
    RejectCode get_reject_code() const {
        return reject_code_;
    }

    // Получить причину отклонения
    std::string get_reject_reason() const {
        return reject_reason_;
    }

    // Получить отладочное сообщение
    std::string get_debug_message() const {
        return debug_message_;
    }

    // Получить результат
    ValidationResult get_result() const {
        return result_;
    }

    // Сбросить состояние
    void reset() {
        result_ = ValidationResult::VALID;
        reject_code_ = RejectCode::INVALID;
        reject_reason_.clear();
        debug_message_.clear();
        dos_ban_ = false;
    }

private:
    ValidationResult result_ = ValidationResult::VALID;
    RejectCode reject_code_ = RejectCode::INVALID;
    std::string reject_reason_;
    std::string debug_message_;
    bool dos_ban_ = false;
};

} // namespace geocoin::consensus
