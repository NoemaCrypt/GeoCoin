#include "test_utils.hpp"
#include "geocoin/wallet/wallet.hpp"
#include <atomic>
#include <cerrno>
#include <iostream>
#if defined(__linux__)
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
static std::atomic<bool> fail_directory_sync{false};
extern "C" int fsync(int fd) {
    struct stat info{};
    if (fail_directory_sync.load() && ::fstat(fd, &info) == 0 && S_ISDIR(info.st_mode)) {
        errno = EIO; return -1;
    }
    return static_cast<int>(::syscall(SYS_fsync, fd));
}
#endif

int main() {
    using namespace geocoin;
    using test::expect;
    const auto root = test::make_temp_dir("geocoin-wallet-durable");
    try {
        const auto primary = root / "wallet.dat", exported = root / "export.dat";
        wallet::Wallet wallet;
        expect(wallet.initialize(bytes(32, 42)), "initialize wallet");
        expect(wallet.save_to_file(primary.string(), "first-password"), "save primary");
        expect(wallet.save_to_file(exported.string(), "first-password"), "export copy");
        expect(wallet.get_file_path() == primary.string(), "export preserves primary path");
        expect(wallet.save("first-password"), "save creates backup");
        std::filesystem::remove(primary);
        wallet::Wallet recovered;
        expect(recovered.load_from_file(primary.string(), "first-password"), "load backup when primary is missing");
        expect(recovered.get_file_path() == primary.string(), "backup load binds the primary path");
        expect(recovered.save("first-password"), "recreate primary after backup load");
#if defined(__linux__)
        fail_directory_sync = true;
        const bool saved = recovered.change_password(primary.string(), "first-password", "second-password");
        fail_directory_sync = false;
        expect(!saved, "directory fsync failure is reported to caller");
        expect(recovered.last_save_status() == wallet::Wallet::SaveStatus::COMMITTED_NOT_DURABLE,
               "published file distinguished from durable save");
        expect(recovered.verify_password("second-password"), "in-memory password matches published file");
        wallet::Wallet reopened;
        expect(reopened.load_from_file(primary.string(), "second-password"), "published file has new password");
        expect(recovered.save("second-password") && recovered.last_save_status() == wallet::Wallet::SaveStatus::DURABLE,
               "retry confirms durable save");
#endif
        std::filesystem::remove_all(root);
        std::cout << "Wallet persistence regressions passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::filesystem::remove_all(root);
        std::cerr << error.what() << '\n'; return 1;
    }
}
