#include "geocoin/utils/rpc_bind_policy.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>

#ifdef _WIN32
#include <stdlib.h>
#endif

namespace {

void expect(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void set_env_value(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

void unset_env_value(const char* name) {
#ifdef _WIN32
    _putenv_s(name, "");
#else
    unsetenv(name);
#endif
}

} // namespace

int main() {
    try {
        unset_env_value("GEOCOIN_ALLOW_PUBLIC_RPC_BIND");

        expect(geocoin::utils::is_loopback_rpc_bind_host("127.0.0.1"), "127.0.0.1 must be loopback");
        expect(geocoin::utils::is_loopback_rpc_bind_host("localhost"), "localhost must be loopback");
        expect(geocoin::utils::is_loopback_rpc_bind_host("::1"), "::1 must be loopback");

        expect(!geocoin::utils::is_loopback_rpc_bind_host("0.0.0.0"), "0.0.0.0 must not be loopback");
        expect(!geocoin::utils::is_loopback_rpc_bind_host("::"), ":: must not be loopback");
        expect(!geocoin::utils::is_loopback_rpc_bind_host("104.253.79.197"), "public IPv4 must not be loopback");

        expect(!geocoin::utils::allow_public_rpc_bind_from_env(), "public RPC env override must be disabled by default");
        set_env_value("GEOCOIN_ALLOW_PUBLIC_RPC_BIND", "1");
        expect(geocoin::utils::allow_public_rpc_bind_from_env(), "public RPC env override must accept 1");
        set_env_value("GEOCOIN_ALLOW_PUBLIC_RPC_BIND", "true");
        expect(geocoin::utils::allow_public_rpc_bind_from_env(), "public RPC env override must accept true");
        set_env_value("GEOCOIN_ALLOW_PUBLIC_RPC_BIND", "yes");
        expect(geocoin::utils::allow_public_rpc_bind_from_env(), "public RPC env override must accept yes");
        set_env_value("GEOCOIN_ALLOW_PUBLIC_RPC_BIND", "0");
        expect(!geocoin::utils::allow_public_rpc_bind_from_env(), "public RPC env override must reject 0");
        unset_env_value("GEOCOIN_ALLOW_PUBLIC_RPC_BIND");

        std::cout << "test_rpc_bind_policy: ok\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "test_rpc_bind_policy failed: " << e.what() << "\n";
        return 1;
    }
}
