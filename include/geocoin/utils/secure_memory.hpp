#pragma once

#include <cstddef>
#include <openssl/crypto.h>

namespace geocoin::utils {

/** Overwrite and clear a contiguous mutable container. */
template <typename Container>
void cleanse_container(Container& value) noexcept
{
    if (!value.empty()) {
        using Element = typename Container::value_type;
        OPENSSL_cleanse(value.data(), value.size() * sizeof(Element));
        value.clear();
    }
}

/** Scope guard for passwords, private keys, and other contiguous secrets. */
template <typename Container>
class ScopedContainerCleanser final
{
public:
    explicit ScopedContainerCleanser(Container& value) noexcept : value_(value) {}
    ~ScopedContainerCleanser() { cleanse_container(value_); }

    ScopedContainerCleanser(const ScopedContainerCleanser&) = delete;
    ScopedContainerCleanser& operator=(const ScopedContainerCleanser&) = delete;
    ScopedContainerCleanser(ScopedContainerCleanser&&) = delete;
    ScopedContainerCleanser& operator=(ScopedContainerCleanser&&) = delete;

private:
    Container& value_;
};

} // namespace geocoin::utils
