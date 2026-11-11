#pragma once

#include "geocoin/rpc/rpc_types.hpp"
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>

namespace geocoin::rpc {

inline bool has_param(const json& params, std::size_t index) {
    return params.size() > index && !params[index].is_null();
}

inline void require_min_params(
    const json& params,
    std::size_t count,
    const std::string& error_message,
    RPCErrorCode code = RPC_INVALID_PARAMS
) {
    if (params.size() < count) {
        throw RPCException(code, error_message);
    }
}

inline const json& required_param(
    const json& params,
    std::size_t index,
    const std::string& error_message,
    RPCErrorCode code = RPC_INVALID_PARAMS
) {
    if (params.size() <= index) {
        throw RPCException(code, error_message);
    }
    return params[index];
}

inline const json& required_object_param(
    const json& params,
    std::size_t index,
    const std::string& error_message,
    RPCErrorCode code = RPC_INVALID_PARAMS
) {
    const json& value = required_param(params, index, error_message, code);
    if (!value.is_object()) {
        throw RPCException(code, error_message);
    }
    return value;
}

inline void require_only_fields(
    const json& object,
    std::initializer_list<std::string_view> allowed_fields,
    const std::string& error_prefix = "Неизвестное поле: ",
    RPCErrorCode code = RPC_INVALID_PARAMETER
) {
    if (!object.is_object()) {
        throw RPCException(code, "Ожидался объект параметров");
    }
    for (auto it = object.begin(); it != object.end(); ++it) {
        bool allowed = false;
        for (const std::string_view field : allowed_fields) {
            if (it.key() == field) {
                allowed = true;
                break;
            }
        }
        if (!allowed) {
            throw RPCException(code, error_prefix + it.key());
        }
    }
}

inline const json& required_field(
    const json& object,
    const std::string& field,
    const std::string& error_message,
    RPCErrorCode code = RPC_INVALID_PARAMETER
) {
    if (!object.is_object() || !object.contains(field)) {
        throw RPCException(code, error_message);
    }
    return object[field];
}

inline const json& required_string_field(
    const json& object,
    const std::string& field,
    const std::string& error_message,
    RPCErrorCode code = RPC_INVALID_PARAMETER
) {
    const json& value = required_field(object, field, error_message, code);
    if (!value.is_string()) {
        throw RPCException(code, error_message);
    }
    return value;
}

inline const json& required_object_field(
    const json& object,
    const std::string& field,
    const std::string& error_message,
    RPCErrorCode code = RPC_INVALID_PARAMETER
) {
    const json& value = required_field(object, field, error_message, code);
    if (!value.is_object()) {
        throw RPCException(code, error_message);
    }
    return value;
}

inline const json& required_array_field(
    const json& object,
    const std::string& field,
    const std::string& error_message,
    RPCErrorCode code = RPC_INVALID_PARAMETER
) {
    const json& value = required_field(object, field, error_message, code);
    if (!value.is_array()) {
        throw RPCException(code, error_message);
    }
    return value;
}

template <typename T>
T required_integer_value(
    const json& value,
    const std::string& error_message,
    RPCErrorCode code = RPC_INVALID_PARAMETER
) {
    static_assert(std::is_integral_v<T> && !std::is_same_v<T, bool>);
    if (value.is_number_unsigned()) {
        const auto raw = value.get<uint64_t>();
        if (raw <= static_cast<uint64_t>((std::numeric_limits<T>::max)())) {
            return static_cast<T>(raw);
        }
    } else if (value.is_number_integer()) {
        const auto raw = value.get<int64_t>();
        if constexpr (std::is_signed_v<T>) {
            if (raw >= (std::numeric_limits<T>::min)() &&
                raw <= (std::numeric_limits<T>::max)()) {
                return static_cast<T>(raw);
            }
        } else {
            if (raw >= 0 && static_cast<uint64_t>(raw) <= (std::numeric_limits<T>::max)()) {
                return static_cast<T>(raw);
            }
        }
    }
    throw RPCException(code, error_message);
}

inline uint64_t required_u64_value(
    const json& value,
    const std::string& error_message,
    RPCErrorCode code = RPC_INVALID_PARAMETER
) {
    return required_integer_value<uint64_t>(value, error_message, code);
}

inline uint64_t required_u64_field(
    const json& object,
    const std::string& field,
    const std::string& error_message,
    RPCErrorCode code = RPC_INVALID_PARAMETER
) {
    return required_u64_value(required_field(object, field, error_message, code), error_message, code);
}

inline uint64_t optional_u64_field(
    const json& object,
    const std::string& field,
    uint64_t default_value,
    const std::string& error_message,
    RPCErrorCode code = RPC_INVALID_PARAMETER
) {
    if (!object.is_object() || !object.contains(field) || object[field].is_null()) {
        return default_value;
    }
    return required_u64_value(object[field], error_message, code);
}

inline uint64_t optional_u64_param(
    const json& params,
    std::size_t index,
    uint64_t default_value,
    const std::string& error_message,
    RPCErrorCode code = RPC_INVALID_PARAMETER
) {
    if (!has_param(params, index)) {
        return default_value;
    }
    return required_u64_value(params[index], error_message, code);
}

inline std::size_t u64_to_size_checked(
    uint64_t value,
    const std::string& error_message,
    RPCErrorCode code = RPC_INVALID_PARAMETER
) {
    if (value > static_cast<uint64_t>((std::numeric_limits<std::size_t>::max)())) {
        throw RPCException(code, error_message);
    }
    return static_cast<std::size_t>(value);
}

inline std::size_t optional_bounded_size_param(
    const json& params,
    std::size_t index,
    std::size_t default_value,
    std::size_t minimum,
    std::size_t maximum,
    const std::string& name,
    RPCErrorCode code = RPC_INVALID_PARAMETER
) {
    const uint64_t raw = optional_u64_param(
        params,
        index,
        static_cast<uint64_t>(default_value),
        name + " must be a non-negative integer",
        code
    );
    const std::size_t value = u64_to_size_checked(raw, name + " is too large", code);
    if (value < minimum || value > maximum) {
        throw RPCException(
            code,
            name + " must be between " + std::to_string(minimum) +
                " and " + std::to_string(maximum)
        );
    }
    return value;
}

inline uint32_t u64_to_u32_checked(
    uint64_t value,
    const std::string& error_message,
    RPCErrorCode code = RPC_INVALID_PARAMETER
) {
    if (value > static_cast<uint64_t>(std::numeric_limits<uint32_t>::max())) {
        throw RPCException(code, error_message);
    }
    return static_cast<uint32_t>(value);
}

inline uint8_t u64_to_u8_checked(
    uint64_t value,
    const std::string& error_message,
    RPCErrorCode code = RPC_INVALID_PARAMETER
) {
    if (value > static_cast<uint64_t>(std::numeric_limits<uint8_t>::max())) {
        throw RPCException(code, error_message);
    }
    return static_cast<uint8_t>(value);
}

inline uint32_t required_u32_field(
    const json& object,
    const std::string& field,
    const std::string& error_message,
    RPCErrorCode code = RPC_INVALID_PARAMETER
) {
    return u64_to_u32_checked(required_u64_field(object, field, error_message, code), error_message, code);
}

inline uint8_t required_u8_field(
    const json& object,
    const std::string& field,
    const std::string& error_message,
    RPCErrorCode code = RPC_INVALID_PARAMETER
) {
    return u64_to_u8_checked(required_u64_field(object, field, error_message, code), error_message, code);
}

inline uint32_t optional_u32_field(
    const json& object,
    const std::string& field,
    uint32_t default_value,
    const std::string& error_message,
    RPCErrorCode code = RPC_INVALID_PARAMETER
) {
    return u64_to_u32_checked(optional_u64_field(object, field, default_value, error_message, code), error_message, code);
}

inline uint8_t optional_u8_field(
    const json& object,
    const std::string& field,
    uint8_t default_value,
    const std::string& error_message,
    RPCErrorCode code = RPC_INVALID_PARAMETER
) {
    return u64_to_u8_checked(optional_u64_field(object, field, default_value, error_message, code), error_message, code);
}

template <typename T>
T optional_param(const json& params, std::size_t index, T default_value) {
    if (!has_param(params, index)) {
        return default_value;
    }
    if constexpr (std::is_integral_v<T> && !std::is_same_v<T, bool>) {
        return required_integer_value<T>(params[index], "Expected an integer in range at parameter " + std::to_string(index + 1));
    } else {
        return params[index].get<T>();
    }
}

} // namespace geocoin::rpc
