#pragma once

#include "geocoin/network/messages.hpp"

#include <cstdint>

namespace geocoin::network {

inline uint64_t service_flag(ServiceFlags flag) {
    return static_cast<uint64_t>(flag);
}

inline bool has_service(uint64_t services, ServiceFlags flag) {
    return (services & service_flag(flag)) != 0;
}

inline uint64_t build_local_services(bool archive_mode) {
    uint64_t services = service_flag(ServiceFlags::NODE_NETWORK)
        | service_flag(ServiceFlags::NODE_WITNESS);
    if (archive_mode) {
        services |= service_flag(ServiceFlags::NODE_ARCHIVE);
    } else {
        services |= service_flag(ServiceFlags::NODE_NETWORK_LIMITED);
    }
    return services;
}

} // namespace geocoin::network
