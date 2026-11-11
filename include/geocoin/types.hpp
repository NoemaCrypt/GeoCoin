#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>
#include <array>
#include <string>
#include <limits>
#include <new>
#include <openssl/crypto.h>
#include "geocoin/utils/u64_math.hpp"
#if defined(__unix__) || defined(__APPLE__)
#include <sys/mman.h>
#endif

namespace geocoin {

// Базовые типы
using byte = uint8_t;
using bytes = std::vector<byte>;

template <typename T>
struct secure_allocator {
    using value_type = T;

    secure_allocator() noexcept = default;
    template <typename U>
    secure_allocator(const secure_allocator<U>&) noexcept {}

    T* allocate(std::size_t n) {
        if (n == 0) {
            return nullptr;
        }
        void* p = ::operator new(n * sizeof(T));
#if defined(__unix__) || defined(__APPLE__)
        (void)mlock(p, n * sizeof(T));
#endif
        return static_cast<T*>(p);
    }

    void deallocate(T* p, std::size_t n) noexcept {
        if (!p) {
            return;
        }
        OPENSSL_cleanse(p, n * sizeof(T));
#if defined(__unix__) || defined(__APPLE__)
        (void)munlock(p, n * sizeof(T));
#endif
        ::operator delete(p);
    }
};

template <typename T, typename U>
inline bool operator==(const secure_allocator<T>&, const secure_allocator<U>&) noexcept {
    return true;
}

template <typename T, typename U>
inline bool operator!=(const secure_allocator<T>&, const secure_allocator<U>&) noexcept {
    return false;
}

using secure_bytes = std::vector<byte, secure_allocator<byte>>;

// Криптографические типы
using Hash256 = std::array<byte, 32>;
using Hash160 = std::array<byte, 20>;
using Signature = bytes;  // Подпись Falcon-512 в бинарном payload-формате

// Экономические типы
using Amount = int64_t;  // Суммы в англах (10^-8 GC)
constexpr Amount COIN = 100000000;  // 1 GC = 10^8 angles

constexpr bool checked_add_amount(Amount lhs, Amount rhs, Amount& result) noexcept {
    if ((rhs > 0 && lhs > (std::numeric_limits<Amount>::max)() - rhs) ||
        (rhs < 0 && lhs < (std::numeric_limits<Amount>::min)() - rhs)) {
        return false;
    }
    result = lhs + rhs;
    return true;
}

constexpr bool checked_sub_amount(Amount lhs, Amount rhs, Amount& result) noexcept {
    if ((rhs > 0 && lhs < (std::numeric_limits<Amount>::min)() + rhs) ||
        (rhs < 0 && lhs > (std::numeric_limits<Amount>::max)() + rhs)) {
        return false;
    }
    result = lhs - rhs;
    return true;
}

constexpr bool checked_mul_amount(Amount lhs, uint32_t rhs, Amount& result) noexcept {
    if (rhs == 0 || lhs == 0) {
        result = 0;
        return true;
    }
    if (lhs < 0) {
        return false;
    }
    if (lhs > (std::numeric_limits<Amount>::max)() / static_cast<Amount>(rhs)) {
        return false;
    }
    result = lhs * static_cast<Amount>(rhs);
    return true;
}

// Формат адреса: GEO + Base58(version + RIPEMD160(SHA256(area)) + checksum)
// Для обратной совместимости оставляем простой тип
using AddressString = std::string;

/**
 * @brief Полный адрес (GEO...)
 */
struct Address {
    AddressString address;  // GEO + Base58 формат

    // Конструкторы
    Address() : address("") {}

    // Для обратной совместимости - неявное преобразование из строки
    Address(const AddressString& addr) : address(addr) {}
    Address(const char* addr) : address(addr) {}

    // Неявное преобразование в строку для обратной совместимости
    operator AddressString() const { return address; }
    operator const char*() const { return address.c_str(); }

    // Операторы сравнения
    bool operator==(const Address& other) const {
        return address == other.address;
    }

    bool operator!=(const Address& other) const {
        return address != other.address;
    }

    bool operator<(const Address& other) const {
        return address < other.address;
    }

    // Проверка на пустоту
    bool empty() const { return address.empty(); }

    // Получить строковое представление
    const AddressString& str() const { return address; }
    const char* c_str() const { return address.c_str(); }
};

// Высота блока
using Height = uint64_t;

// Метка времени (Unix time в секундах)
using Timestamp = uint64_t;

// Вызов для аутентификации
using Challenge = uint64_t;

/**
 * 256-bit unsigned integer
 * Используется для chainwork и target в PoW
 */
struct uint256 {
    uint64_t data[4] = {0, 0, 0, 0};  // data[0] = младшие 64 бита, data[3] = старшие 64 бита

    uint256() = default;

    uint256(uint64_t value) {
        data[0] = value;
        data[1] = 0;
        data[2] = 0;
        data[3] = 0;
    }

    // Операторы сравнения
    bool operator==(const uint256& other) const {
        return data[0] == other.data[0] &&
               data[1] == other.data[1] &&
               data[2] == other.data[2] &&
               data[3] == other.data[3];
    }

    bool operator!=(const uint256& other) const {
        return !(*this == other);
    }

    bool operator<(const uint256& other) const {
        for (int i = 3; i >= 0; i--) {
            if (data[i] < other.data[i]) return true;
            if (data[i] > other.data[i]) return false;
        }
        return false;
    }

    bool operator>(const uint256& other) const {
        return other < *this;
    }

    bool operator<=(const uint256& other) const {
        return !(other < *this);
    }

    bool operator>=(const uint256& other) const {
        return !(*this < other);
    }

    // Арифметические операторы
    uint256 operator+(const uint256& other) const {
        uint256 result;
        uint64_t carry = 0;

        for (int i = 0; i < 4; i++) {
            utils::add_u64_carry(data[i], other.data[i], carry, result.data[i], carry);
        }

        return result;
    }

    uint256 operator-(const uint256& other) const {
        uint256 result;
        uint64_t borrow = 0;

        for (int i = 0; i < 4; i++) {
            utils::sub_u64_borrow(data[i], other.data[i], borrow, result.data[i], borrow);
        }

        return result;
    }

    uint256 operator*(uint64_t multiplier) const {
        if (multiplier == 0) {
            return uint256();
        }
        if (multiplier == 1) {
            return *this;
        }

        if (data[3] == 0 && data[2] == 0 && data[1] == 0) {
            const utils::U128Parts product = utils::mul_u64_wide(data[0], multiplier);
            uint256 result;
            result.data[0] = product.lo;
            result.data[1] = product.hi;
            return result;
        }

        uint256 result;
        uint64_t carry = 0;

        for (int i = 0; i < 4; i++) {
            const utils::U128Parts product = utils::mul_u64_wide(data[i], multiplier);
            uint64_t next_carry = 0;
            uint64_t low = 0;
            utils::add_u64_carry(product.lo, carry, 0, low, next_carry);
            result.data[i] = low;
            uint64_t hi_with_carry = 0;
            uint64_t ignored = 0;
            utils::add_u64_carry(product.hi, next_carry, 0, hi_with_carry, ignored);
            carry = hi_with_carry;
        }

        return result;
    }

    uint256 operator/(uint64_t divisor) const {
        uint256 result;
        if (divisor == 0) {
            return result;
        }
        uint64_t remainder = 0;

        for (int i = 3; i >= 0; i--) {
            result.data[i] = utils::div_u128_by_u64_fast(remainder, data[i], divisor, &remainder);
        }

        return result;
    }

    // Проверка на ноль
    bool is_zero() const {
        return data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 0;
    }

    // Конвертация в double (для вычисления хешрейта)
    // Аналог getdouble() из Bitcoin
    double get_double() const {
        double ret = 0.0;
        double fact = 1.0;
        for (int i = 0; i < 4; i++) {
            ret += fact * data[i];
            fact *= 18446744073709551616.0; // 2^64
        }
        return ret;
    }
};

} // namespace geocoin
