#include "geocoin/crypto/addresses.hpp"
#include "geocoin/wallet/multisig.hpp"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void print_usage(const char* argv0) {
    std::cout
        << "Usage:\n"
        << "  " << argv0 << " create <m> <n> <address1> ... <addressN>\n"
        << "  " << argv0 << " verify <multisig_address>\n";
}

uint8_t parse_u8(const char* s, const char* what) {
    int v = std::stoi(s);
    if (v < 0 || v > 255) {
        throw std::out_of_range(std::string(what) + " out of range");
    }
    return static_cast<uint8_t>(v);
}
}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc < 2) {
            print_usage(argv[0]);
            return 1;
        }

        const std::string cmd = argv[1];

        if (cmd == "create") {
            if (argc < 5) {
                print_usage(argv[0]);
                return 1;
            }

            const uint8_t m = parse_u8(argv[2], "m");
            const uint8_t n = parse_u8(argv[3], "n");

            if (argc != 4 + static_cast<int>(n)) {
                std::cerr << "Expected exactly " << static_cast<int>(n)
                          << " participant addresses\n";
                return 1;
            }

            std::vector<geocoin::Address> addrs;
            addrs.reserve(n);

            for (int i = 0; i < n; ++i) {
                geocoin::Address addr(argv[4 + i]);
                if (!geocoin::crypto::verify_address(addr)) {
                    std::cerr << "Invalid address: " << addr.str() << "\n";
                    return 1;
                }
                addrs.push_back(addr);
            }

            const auto multisig = geocoin::wallet::create_multisig_address(m, n, addrs);
            std::cout << multisig.address << "\n";
            return 0;
        }

        if (cmd == "verify") {
            if (argc != 3) {
                print_usage(argv[0]);
                return 1;
            }

            const geocoin::Address addr(argv[2]);
            const bool valid = geocoin::wallet::is_multisig_address(addr.str()) &&
                               geocoin::crypto::verify_address(addr);
            std::cout << (valid ? "valid" : "invalid") << "\n";
            return valid ? 0 : 2;
        }

        print_usage(argv[0]);
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "multisig_cli error: " << e.what() << "\n";
        return 1;
    }
}
