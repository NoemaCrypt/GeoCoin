#include "geocoin/wallet/wallet_persist_internal.hpp"
#include "geocoin/crypto/encryption.hpp"
#include "geocoin/crypto/addresses.hpp"
#include "geocoin/crypto/geo_area.hpp"
#include "nlohmann_json.hpp"

#include <filesystem>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <atomic>
#include <thread>
#if defined(_WIN32)
#include <windows.h>
#elif defined(__linux__)
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
static std::atomic<bool> fail_rename{false};
extern "C" int rename(const char* from, const char* to) noexcept {
    if (fail_rename.load()) { errno = EIO; return -1; }
    return static_cast<int>(::syscall(SYS_renameat, AT_FDCWD, from, AT_FDCWD, to));
}
#endif

namespace {
using namespace geocoin;
using namespace geocoin::wallet;
size_t checks = 0;
void expect(bool value, const std::string& message) {
    ++checks;
    if (!value) throw std::runtime_error(message);
}
bytes read(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    expect(bool(input), "open synthetic fixture");
    return bytes(std::istreambuf_iterator<char>(input), {});
}
void write(const std::filesystem::path& path, const bytes& data) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(data.data()), data.size());
    expect(bool(output), "write synthetic fixture");
}
void string(bytes& out, const std::string& value) {
    append_u32_le(out, static_cast<uint32_t>(value.size()));
    out.insert(out.end(), value.begin(), value.end());
}

struct Fixture {
    std::vector<KeyInfo> keys;
    std::vector<WalletTransaction> history;
    MultisigConfig multisig;
    bytes salt = bytes(16, 0x72);
    Hash256 verifier{};
    Hash256 checkpoint{};
    std::string password = "synthetic-migration-password";

    Fixture() {
        for (byte seed : {byte(41), byte(75), byte(119)}) {
            KeyInfo info;
            info.private_key = secure_bytes(32, seed);
            bytes key(32, seed);
            expect(crypto::compute_address_from_private_key(key, info.address), "derive synthetic address");
            info.label = keys.empty() ? "default" : "deposit-" + std::to_string(seed);
            info.created_at = 1700000000 + seed;
            info.used = seed != 75;
            keys.push_back(info);
        }
        checkpoint.fill(0xa2);
        const auto policy = create_multisig_address(2, 3, {keys[2].address, keys[0].address, keys[1].address});
        multisig = policy.config;
        multisig.address = Address(policy.address);
        expect(derive_password_verifier_pbkdf2(password, salt, kPasswordVerifierIterations, verifier), "derive synthetic verifier");
        WalletTransaction receive;
        receive.tx.version = 2;
        receive.tx.timestamp = 1700000100;
        receive.tx.outputs.push_back({5 * COIN, keys[0].address, {}});
        receive.tx_hash = receive.tx.get_hash();
        receive.is_coinbase = receive.tx.is_coinbase();
        receive.block_height = 17; receive.timestamp = 1700000100;
        receive.amount = 5 * COIN; receive.memo = "first synthetic credit";
        history.push_back(receive);
        WalletTransaction spend;
        spend.tx.version = 2; spend.tx.timestamp = 1700000200;
        core::TxInput input;
        input.prev_tx_hash = receive.tx_hash; input.vout = 0;
        spend.tx.inputs.push_back(input);
        spend.tx.outputs.push_back({COIN, multisig.address, {}});
        spend.tx.outputs.push_back({4 * COIN - 1000, keys[1].address, {}});
        spend.tx_hash = spend.tx.get_hash(); spend.is_coinbase = spend.tx.is_coinbase();
        spend.block_height = 18; spend.timestamp = 1700000200; spend.amount = -COIN - 1000;
        spend.fee = 1000; spend.memo = "synthetic multisig funding";
        history.push_back(spend);
    }

    bytes payload(uint32_t version, bool protected_file = true) const {
        bytes out{protected_file ? kPasswordModePbkdf2 : kPasswordModeNone};
        if (protected_file) {
            out.insert(out.end(), verifier.begin(), verifier.end());
            out.insert(out.end(), salt.begin(), salt.end());
            append_u32_le(out, kPasswordVerifierIterations);
        }
        append_u32_le(out, static_cast<uint32_t>(keys.size()));
        for (const auto& key : keys) {
            out.insert(out.end(), key.private_key.begin(), key.private_key.end());
            string(out, key.label); append_u64_le(out, key.created_at);
            if (version >= 12) out.push_back(key.used ? 1 : 0);
        }
        append_u32_le(out, static_cast<uint32_t>(history.size()));
        for (const auto& tx : history) {
            out.insert(out.end(), tx.tx_hash.begin(), tx.tx_hash.end());
            append_u64_le(out, tx.block_height); append_u64_le(out, tx.timestamp);
            append_i64_le(out, tx.amount); append_u64_le(out, tx.fee);
            out.push_back(tx.is_coinbase ? 1 : 0); string(out, tx.memo);
            const auto data = tx.tx.serialize();
            append_u32_le(out, static_cast<uint32_t>(data.size()));
            out.insert(out.end(), data.begin(), data.end());
        }
        append_u64_le(out, 18);
        if (version >= 12) out.insert(out.end(), checkpoint.begin(), checkpoint.end());
        append_u32_le(out, 1); string(out, multisig.address.str());
        out.push_back(multisig.m); out.push_back(multisig.n);
        append_u32_le(out, static_cast<uint32_t>(multisig.addresses.size()));
        for (const auto& address : multisig.addresses) string(out, address.str());
        if (version >= 13) string(out, keys[0].address.str());
        return out;
    }

    bytes envelope(uint32_t version, const bytes& payload, bool encrypted = true) const {
        bytes cipher = payload;
        if (encrypted) expect(crypto::encrypt_aes256(payload, password, cipher), "encrypt synthetic payload");
        bytes result;
        append_u32_le(result, 0x47454F43); append_u32_le(result, version);
        result.push_back(encrypted ? 1 : 0); append_u32_le(result, static_cast<uint32_t>(cipher.size()));
        result.insert(result.end(), cipher.begin(), cipher.end());
        if (encrypted) {
            bytes mac;
            expect(compute_wallet_hmac(password, cipher, crypto::kPbkdf2Iterations, result, mac), "authenticate synthetic envelope");
            result.insert(result.end(), mac.begin(), mac.end());
        }
        return result;
    }

    bytes sha256_payload(uint32_t version) const {
        // Historical v11 fixture: mode 1 + raw SHA256(password), then key count.
        const auto state = payload(version, false);
        bytes out{1};
        const auto hash = crypto::sha256(reinterpret_cast<const byte*>(password.data()), password.size());
        out.insert(out.end(), hash.begin(), hash.end());
        out.insert(out.end(), state.begin() + 1, state.end());
        return out;
    }

    void verify(Wallet& wallet, uint32_t original_version) const {
        expect(wallet.get_key_count() == keys.size() && wallet.get_address() == keys[0].address, "all keys and historical primary preserved");
        for (const auto& key : keys) {
            KeyInfo actual;
            expect(wallet.get_key_info(key.address, actual), "all key records exist");
            expect(actual.private_key == key.private_key && actual.label == key.label && actual.created_at == key.created_at &&
                actual.used == (original_version == 11 || key.used), "seed, label, date and usage preserved");
        }
        expect(wallet.get_last_scanned_height() == 18 && wallet.get_last_scanned_block_hash() ==
            (original_version == 11 ? Hash256{} : checkpoint), "historical checkpoint preserved without inventing a hash");
        const auto actual = wallet.get_transactions("", 0);
        expect(actual.size() == history.size(), "full history preserved");
        for (const auto& tx : history) {
            WalletTransaction restored;
            expect(wallet.get_transaction(tx.tx_hash, restored), "history index rebuilt");
            expect(restored.tx.serialize() == tx.tx.serialize() && restored.tx_hash == tx.tx_hash &&
                restored.block_height == tx.block_height && restored.timestamp == tx.timestamp &&
                restored.amount == tx.amount && restored.fee == tx.fee && restored.memo == tx.memo &&
                restored.is_coinbase == tx.is_coinbase, "all history fields preserved");
        }
        const auto policies = wallet.get_multisig_addresses();
        expect(policies.size() == 1 && policies[0].address == multisig.address && policies[0].m == multisig.m &&
            policies[0].n == multisig.n && policies[0].addresses == multisig.addresses, "full multisig policy preserved");
    }
};

void legacy_tests(const std::filesystem::path& root, const Fixture& fixture, uint32_t version) {
    const auto path = root / ("synthetic-v" + std::to_string(version) + ".dat");
    const auto original = fixture.envelope(version, fixture.payload(version));
    write(path, original);
    Wallet wallet;
    auto result = wallet.load_from_file_result(path.string(), "");
    expect(result.error == WalletLoadError::PASSWORD_REQUIRED, "password-required structured error");
    result = wallet.load_from_file_result(path.string(), "wrong-password");
    expect(result.error == WalletLoadError::AUTHENTICATION_FAILED && wallet.get_key_count() == 0, "wrong password publishes no state");
    result = wallet.load_from_file_result(path.string(), fixture.password);
    expect(result.ok() && result.migration_needed && result.authenticated && result.version == version, "structured authenticated legacy dispatch");
    fixture.verify(wallet, version);
    expect(!wallet.save(fixture.password) && !wallet.change_password(path.string(), fixture.password, "new") &&
        !wallet.enable_vault(fixture.password) && read(path) == original, "legacy cannot be implicitly rewritten or rekeyed");
    WalletMigrationOptions options;
    expect(Wallet::migrate_file(path.string(), fixture.password, options).error == WalletMigrationError::CONFIRMATION_REQUIRED,
        "explicit intent required before migration");
    expect(!std::filesystem::exists(path.string() + ".pre-v13.bak"), "unconfirmed migration writes nothing");
    options.confirmed = true;
    expect(Wallet::migrate_file(path.string(), "wrong-password", options).error == WalletMigrationError::LOAD_FAILED &&
        read(path) == original && !std::filesystem::exists(path.string() + ".pre-v13.bak"), "failed authentication writes nothing");
    options.output_path = (root / ("migrated-v" + std::to_string(version) + ".dat")).string();
    auto migration = Wallet::migrate_file(path.string(), fixture.password, options);
    expect(migration.ok() && migration.durable && read(path) == original && read(migration.backup_path) == original,
        std::string("alternate migration and exact backup: ") + wallet_migration_error_name(migration.error));
    Wallet native;
    result = native.load_from_file_result(migration.output_path, fixture.password);
    expect(result.ok() && !result.migration_needed && result.format == WalletFileFormat::NATIVE_V13, "migrated output is native v13");
    fixture.verify(native, version);
    expect(native.verify_password(fixture.password) && !native.verify_password("wrong-password"), "password unchanged");
    expect(Wallet::migrate_file(path.string(), fixture.password, options).error == WalletMigrationError::DESTINATION_EXISTS,
        "backup and output are never overwritten");
    options.output_path.clear(); options.backup_path = (root / ("in-place-v" + std::to_string(version) + ".bak")).string();
    migration = Wallet::migrate_file(path.string(), fixture.password, options);
    expect(migration.ok() && migration.durable && read(migration.backup_path) == original, "in-place migration with exact backup");
    Wallet reopened;
    expect(reopened.load_from_file_result(path.string(), fixture.password).ok(), "native in-place reload");
    fixture.verify(reopened, version);
}

void failure_tests(const std::filesystem::path& root, const Fixture& fixture) {
    const auto path = root / "bad.dat";
    Wallet active;
    expect(active.initialize(bytes(32, 12)), "active wallet fixture");
    const auto address = active.get_address();
    const auto active_path = active.get_file_path();
    auto reject = [&](const bytes& file, WalletLoadError expected) {
        write(path, file);
        const auto result = active.load_from_file_result(path.string(), fixture.password);
        expect(result.error == expected, std::string("structured failure: ") + wallet_load_error_name(result.error));
        expect(active.get_key_count() == 1 && active.get_address() == address && active.get_file_path() == active_path,
            "candidate failure retains active wallet state and path");
        WalletMigrationOptions options; options.confirmed = true;
        const auto migration = Wallet::migrate_file(path.string(), fixture.password, options);
        expect(migration.error == WalletMigrationError::LOAD_FAILED && read(path) == file &&
            !std::filesystem::exists(path.string() + ".pre-v13.bak"), "corrupt migration leaves source and disk unchanged");
    };
    auto plain = fixture.payload(11);
    plain.push_back(0x81);
    reject(fixture.envelope(11, plain), WalletLoadError::CORRUPT_DATA);
    plain = fixture.payload(12); plain.resize(plain.size() - 3);
    reject(fixture.envelope(12, plain), WalletLoadError::CORRUPT_DATA);
    plain = fixture.payload(11); plain[1] ^= 1;
    reject(fixture.envelope(11, plain), WalletLoadError::AUTHENTICATION_FAILED);
    Fixture duplicate = fixture; duplicate.keys[1] = duplicate.keys[0];
    reject(fixture.envelope(11, duplicate.payload(11)), WalletLoadError::CORRUPT_DATA);
    Fixture history = fixture; history.history[0].tx_hash[0] ^= 1;
    reject(fixture.envelope(12, history.payload(12)), WalletLoadError::CORRUPT_DATA);
    Fixture policy = fixture; policy.multisig.m = 0;
    reject(fixture.envelope(11, policy.payload(11)), WalletLoadError::CORRUPT_DATA);
    auto file = fixture.envelope(11, fixture.payload(11)); file.back() ^= 1;
    reject(file, WalletLoadError::AUTHENTICATION_FAILED);
    file.resize(8); reject(file, WalletLoadError::CORRUPT_DATA);
    write(path.string() + ".bak", fixture.envelope(13, fixture.payload(13)));
    reject(fixture.envelope(14, fixture.payload(13)), WalletLoadError::UNSUPPORTED_VERSION);
    expect(!active.load_from_file_result(path.string(), fixture.password, true).ok(), "unknown version never falls back to older backup");
    write(path, fixture.envelope(13, fixture.payload(13)));
    expect(!active.load_from_file_result(path.string(), "wrong", true).ok(), "bad authentication never falls back to backup");
    std::filesystem::remove(path);
    expect(active.load_from_file_result(path.string(), fixture.password).error == WalletLoadError::IO_ERROR,
        "structured load never silently recovers a missing source");
    const auto recovery = active.load_from_file_result(path.string(), fixture.password, true);
    expect(recovery.ok() && recovery.recovered_from_backup && recovery.source_path == path.string() + ".bak" &&
        active.get_file_path() == path.string(), "explicit backup recovery reports provenance and retains primary binding");

    const auto source = root / "rollback-v11.dat";
    const auto original = fixture.envelope(11, fixture.payload(11)); write(source, original);
    WalletMigrationOptions options; options.confirmed = true;
    options.backup_path = (root / "missing-directory" / "backup.dat").string();
    expect(Wallet::migrate_file(source.string(), fixture.password, options).error == WalletMigrationError::BACKUP_FAILED &&
        read(source) == original, "backup failure preserves source");
    options.backup_path = (root / "rollback.bak").string();
    options.output_path = (root / "missing-directory" / "output.dat").string();
    expect(Wallet::migrate_file(source.string(), fixture.password, options).error == WalletMigrationError::WRITE_FAILED &&
        read(source) == original && read(options.backup_path) == original, "temporary write failure preserves source and backup");
    options.backup_path = (root / "commit-failure.bak").string(); options.output_path.clear();
#if defined(_WIN32)
    HANDLE held = ::CreateFileW(source.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    expect(held != INVALID_HANDLE_VALUE, "deny source deletion during atomic replace");
    const auto migration = Wallet::migrate_file(source.string(), fixture.password, options);
    ::CloseHandle(held);
    expect(migration.error == WalletMigrationError::COMMIT_FAILED && !migration.committed &&
        read(source) == original && read(options.backup_path) == original, "atomic replace failure preserves source and verified backup");
#elif defined(__linux__)
    fail_rename = true;
    const auto migration = Wallet::migrate_file(source.string(), fixture.password, options);
    fail_rename = false;
    expect(!migration.committed && read(source) == original, "rename failure preserves source");
#endif
    options.backup_path = (root / "source-changed.bak").string();
    auto concurrent = original; concurrent.back() ^= 1;
    std::atomic<bool> done{false};
    bool changed = false;
    std::thread writer([&] {
        while (!done.load()) {
            std::error_code error;
            if (std::filesystem::exists(options.backup_path, error) && !error) {
                std::ofstream output(source, std::ios::binary | std::ios::trunc);
                output.write(reinterpret_cast<const char*>(concurrent.data()), concurrent.size());
                changed = bool(output);
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });
    const auto changed_result = Wallet::migrate_file(source.string(), fixture.password, options);
    done = true;
    writer.join();
    expect(changed && changed_result.error == WalletMigrationError::SOURCE_CHANGED && !changed_result.committed &&
        read(source) == concurrent && read(options.backup_path) == original, "concurrent source replacement is detected and never overwritten");
}

void native_tests(const std::filesystem::path& root, const Fixture& fixture) {
    const auto path = root / "native.dat";
    write(path, fixture.envelope(13, fixture.payload(13)));
    Wallet native;
    auto result = native.load_from_file_result(path.string(), fixture.password);
    expect(result.ok() && result.format == WalletFileFormat::NATIVE_V13 && !result.migration_needed, "native dispatcher");
    fixture.verify(native, 13);
    auto extended = fixture.history[1];
    extended.block_hash.fill(0x31); extended.block_index = 4; extended.block_time = extended.timestamp + 2;
    Hash256 old{}; old.fill(0x43);
    extended.removed_blocks.push_back({old, 15, 2, 1700000001});
    extended.change_outputs = {1}; extended.broadcast = true; extended.received_at = 1700000199;
    extended.note_conflict(fixture.history[0].tx_hash, {old, 15, 2, 1700000001});
    native.add_transaction(extended);
    expect(native.save(fixture.password), "save WEX1 extended history");
    const auto verify_extension = [&](Wallet& wallet) {
        WalletTransaction tx;
        expect(wallet.get_transaction(extended.tx_hash, tx) && tx.block_hash == extended.block_hash &&
            tx.block_index == extended.block_index && tx.block_time == extended.block_time &&
            tx.removed_blocks.size() == 1 && tx.removed_blocks[0].hash == old && tx.removed_blocks[0].height == 15 &&
            tx.removed_blocks[0].index == 2 && tx.removed_blocks[0].time == 1700000001 &&
            tx.change_outputs == extended.change_outputs && tx.broadcast && tx.received_at == extended.received_at &&
            tx.conflicts.size() == 1 && tx.conflicts[0].first == fixture.history[0].tx_hash &&
            tx.conflicts[0].second.hash == old, "full WEX1 history survives native reload");
    };
    Wallet reloaded;
    expect(reloaded.load_from_file_result(path.string(), fixture.password).ok(), "reload extended v13");
    verify_extension(reloaded);
    WalletMigrationOptions options; options.confirmed = true;
    expect(Wallet::migrate_file(path.string(), fixture.password, options).error == WalletMigrationError::NOT_NEEDED, "native never migrated");
    expect(native.enable_vault(fixture.password), "make synthetic GEOV vault");
    Wallet vault;
    result = vault.load_from_file_result(path.string(), "");
    expect(result.ok() && !result.authenticated && vault.is_locked() && result.format == WalletFileFormat::VAULT_GEOV,
        "GEOV public-only locked load is explicitly unauthenticated");
    result = vault.load_from_file_result(path.string(), "wrong");
    expect(result.error == WalletLoadError::AUTHENTICATION_FAILED && vault.is_locked(), "GEOV wrong password retains state");
    result = vault.load_from_file_result(path.string(), fixture.password);
    expect(result.ok() && result.authenticated && vault.is_locked(), "GEOV authenticates both key envelopes without unlocking");
    expect(vault.unlock(fixture.password, 10), "unlock native vault");
    fixture.verify(vault, 13);
    verify_extension(vault);
    vault.lock();
    auto file = read(path);
    file[20] ^= 1; write(root / "vault-corrupt.dat", file);
    expect(!vault.load_from_file_result((root / "vault-corrupt.dat").string(), fixture.password).ok(), "GEOV corruption rejected");
    file = read(path);
    auto document = nlohmann::json::parse(file.begin() + 8, file.end() - 32);
    const auto vault_document = document;
    const auto write_document = [&](const std::filesystem::path& target, const nlohmann::json& changed) {
        const auto serialized = changed.dump();
        bytes file{'G', 'E', 'O', 'V'}; append_u32_le(file, static_cast<uint32_t>(serialized.size()));
        file.insert(file.end(), serialized.begin(), serialized.end());
        const auto checksum = crypto::sha256(file); file.insert(file.end(), checksum.begin(), checksum.end());
        write(target, file);
    };
    document["format"] = 2;
    write_document(root / "vault-unknown.dat", document);
    expect(vault.load_from_file_result((root / "vault-unknown.dat").string(), fixture.password).error ==
        WalletLoadError::UNSUPPORTED_VERSION, "GEOV unknown native version rejected");
    document = vault_document; document["multisig"][0]["m"] = 258;
    write_document(root / "vault-overflow.dat", document);
    expect(vault.load_from_file_result((root / "vault-overflow.dat").string(), fixture.password).error ==
        WalletLoadError::CORRUPT_DATA, "GEOV numeric narrowing cannot wrap into a valid policy");
    document = vault_document; document["keys"][0]["created"] = -1;
    write_document(root / "vault-negative.dat", document);
    expect(vault.load_from_file_result((root / "vault-negative.dat").string(), fixture.password).error ==
        WalletLoadError::CORRUPT_DATA, "GEOV negative unsigned metadata rejected");
    document = vault_document;
    auto encrypted = document["key_vault"].get<std::string>(); encrypted.back() = encrypted.back() == '0' ? '1' : '0';
    document["key_vault"] = encrypted;
    write_document(root / "vault-key-corrupt.dat", document);
    expect(vault.load_from_file_result((root / "vault-key-corrupt.dat").string(), fixture.password).error ==
        WalletLoadError::AUTHENTICATION_FAILED, "GEOV authenticates key vault as well as password wrapper");

    const auto plaintext = root / "plain-v11.dat";
    const auto original = fixture.envelope(11, fixture.payload(11, false), false); write(plaintext, original);
    options.output_path = (root / "plain-v13.dat").string();
    const auto migration = Wallet::migrate_file(plaintext.string(), "", options);
    expect(migration.ok() && read(plaintext) == original, "plaintext migration supported without adding a password");
    Wallet plain;
    result = plain.load_from_file_result(options.output_path, "");
    expect(result.ok() && !result.encrypted && !plain.has_password_protection(), "plaintext security unchanged");
    fixture.verify(plain, 11);

    const auto no_verifier = fixture.payload(11, false);
    const auto no_verifier_path = root / "encrypted-no-verifier-v11.dat";
    write(no_verifier_path, fixture.envelope(11, no_verifier));
    options.output_path = (root / "encrypted-no-verifier-v13.dat").string();
    const auto preserved = Wallet::migrate_file(no_verifier_path.string(), fixture.password, options);
    expect(preserved.ok(), "encrypted legacy without inner verifier migrates without adding one");
    const auto saved = read(options.output_path);
    bytes cipher(saved.begin() + kWalletHeaderSize, saved.end() - kWalletHmacSize), restored;
    expect(crypto::decrypt_aes256(cipher, fixture.password, restored) && restored[0] == kPasswordModeNone,
        "migration preserves verifier mode, not just the password");
    Wallet no_verifier_wallet;
    expect(no_verifier_wallet.load_from_file_result(options.output_path, fixture.password).ok() &&
        !no_verifier_wallet.save(""), "encrypted wallet cannot be silently downgraded when inner verifier is absent");
}

void path_tests(const std::filesystem::path& root, const Fixture& fixture) {
    const auto unicode = root / std::filesystem::path(u8"\u041a\u043e\u0448\u0435\u043b\u0435\u043a-\u8d22\u5e03");
    std::filesystem::create_directories(unicode);
    const auto source = unicode / std::filesystem::path(u8"\u0438\u0441\u0442\u043e\u0447\u043d\u0438\u043a.dat");
    const auto original = fixture.envelope(12, fixture.payload(12));
    write(source, original);
    const auto source_utf8 = wallet_path_string(source);
    expect(set_private_file_permissions(source_utf8), "secure synthetic source before making it read-only");
#if defined(_WIN32)
    expect(::SetFileAttributesW(source.c_str(), FILE_ATTRIBUTE_READONLY), "mark synthetic source read-only");
#elif defined(__linux__)
    expect(::chmod(source.c_str(), 0400) == 0 && ::chmod(unicode.c_str(), 0500) == 0, "make source file and directory read-only");
#endif
    WalletMigrationOptions options; options.confirmed = true;
    options.output_path = wallet_path_string(root / std::filesystem::path(u8"\u043d\u043e\u0432\u044b\u0439-v13.dat"));
    const auto migration = Wallet::migrate_file(source_utf8, fixture.password, options);
#if defined(_WIN32)
    const bool still_readonly = (::GetFileAttributesW(source.c_str()) & FILE_ATTRIBUTE_READONLY) != 0;
    (void)::SetFileAttributesW(source.c_str(), FILE_ATTRIBUTE_NORMAL);
    expect(still_readonly, "source read-only attribute unchanged");
#elif defined(__linux__)
    struct stat info{};
    const bool unchanged_mode = ::stat(source.c_str(), &info) == 0 && (info.st_mode & 0777) == 0400;
    (void)::chmod(unicode.c_str(), 0700);
    expect(unchanged_mode, "source privacy checked without chmod on already-private input");
#endif
    expect(migration.ok() && migration.durable && read(source) == original && read(wallet_path(migration.backup_path)) == original,
        std::string("read-only Unicode source migrates to alternate output: ") + wallet_migration_error_name(migration.error));
    expect(migration.backup_path == options.output_path + ".pre-v13.bak", "out-of-place backup defaults beside output, never source");
    expect(std::distance(std::filesystem::directory_iterator(unicode), std::filesystem::directory_iterator{}) == 1,
        "alternate migration writes no files in source directory");
    Wallet loaded;
    expect(loaded.load_from_file_result(options.output_path, fixture.password).ok() && loaded.get_file_path() == options.output_path,
        "UTF-8 output loads and binds without codepage loss");
    fixture.verify(loaded, 12);
    expect(loaded.save(fixture.password), "UTF-8-bound native wallet saves and preserves backup");
    expect(loaded.enable_vault(fixture.password), "UTF-8 vault save");
    Wallet vault;
    expect(vault.load_from_file_result(options.output_path, fixture.password).ok() && vault.is_locked(), "UTF-8 vault reload");
    const auto backup = wallet_path_string(root / std::filesystem::path(u8"\u0440\u0435\u0437\u0435\u0440\u0432.geov"));
    expect(vault.backup(backup) && vault.get_file_path() == options.output_path, "UTF-8 vault backup preserves binding");
}

void legacy_sha256_tests(const std::filesystem::path& root, const Fixture& fixture, uint32_t version) {
    const auto source = root / ("sha256-v" + std::to_string(version) + ".dat");
    const auto original = fixture.envelope(version, fixture.sha256_payload(version));
    write(source, original);
    WalletMigrationOptions options;
    options.output_path = (root / ("sha256-converted-v" + std::to_string(version) + ".dat")).string();
    expect(Wallet::migrate_file(source.string(), fixture.password, options).error == WalletMigrationError::CONFIRMATION_REQUIRED &&
        !std::filesystem::exists(options.output_path), "SHA256 migration requires explicit confirmation");
    options.confirmed = true;
    expect(Wallet::migrate_file(source.string(), "wrong", options).error == WalletMigrationError::LOAD_FAILED &&
        !std::filesystem::exists(options.output_path + ".pre-v13.bak") && read(source) == original,
        "SHA256 wrong password cannot create backup or change source");
    Wallet staged;
    const auto loaded = staged.load_from_file_result(source.string(), fixture.password);
    expect(loaded.ok() && loaded.authenticated && loaded.migration_needed && loaded.version == version,
        "authenticated legacy SHA256 schema loads");
    fixture.verify(staged, version);
    expect(staged.has_password_protection() && staged.verify_password(fixture.password) && !staged.verify_password("wrong"),
        "staged SHA256 wallet uses a strong in-memory password verifier");
    expect(!staged.save(fixture.password) && read(source) == original, "SHA256 staging never implicitly rewrites source");

    const auto migration = Wallet::migrate_file(source.string(), fixture.password, options);
    expect(migration.ok() && migration.durable && read(source) == original && read(wallet_path(migration.backup_path)) == original,
        "SHA256 alternate migration preserves source and exact backup");
    Wallet native;
    const auto reloaded = native.load_from_file_result(options.output_path, fixture.password);
    expect(reloaded.ok() && reloaded.format == WalletFileFormat::NATIVE_V13 && !reloaded.migration_needed &&
        native.verify_password(fixture.password) && !native.verify_password("wrong"), "converted SHA256 wallet is strict native v13");
    fixture.verify(native, version);
    const auto saved = read(wallet_path(options.output_path));
    bytes cipher(saved.begin() + kWalletHeaderSize, saved.end() - kWalletHmacSize), plain;
    expect(crypto::decrypt_aes256(cipher, fixture.password, plain) && plain.size() >= 53 && plain[0] == kPasswordModePbkdf2,
        "converted SHA256 payload stores PBKDF2 mode, never mode 1");
    const bytes salt(plain.begin() + 33, plain.begin() + 49);
    uint32_t iterations = 0;
    for (unsigned i = 0; i < 4; ++i) iterations |= uint32_t(plain[49 + i]) << (8 * i);
    Hash256 verifier{};
    expect(iterations == kPasswordVerifierIterations && derive_password_verifier_pbkdf2(fixture.password, salt, iterations, verifier) &&
        constant_time_compare(verifier.data(), plain.data() + 1, verifier.size()), "SHA256 upgrade uses current salted verifier and unchanged password");

    const auto bad = root / ("sha256-rejected-v" + std::to_string(version) + ".dat");
    Wallet active;
    expect(active.initialize(bytes(32, 13)), "active wallet for SHA256 rejection checks");
    const auto active_address = active.get_address();
    const auto active_path = active.get_file_path();
    auto reject = [&](const bytes& file, WalletLoadError expected) {
        write(bad, file);
        const auto result = active.load_from_file_result(bad.string(), fixture.password);
        expect(result.error == expected && active.get_address() == active_address && active.get_key_count() == 1 &&
            active.get_file_path() == active_path, "SHA256 rejection preserves active wallet");
        WalletMigrationOptions rejected; rejected.confirmed = true;
        rejected.output_path = (root / "sha256-must-not-exist.dat").string();
        expect(Wallet::migrate_file(bad.string(), fixture.password, rejected).error == WalletMigrationError::LOAD_FAILED &&
            read(bad) == file && !std::filesystem::exists(rejected.output_path) &&
            !std::filesystem::exists(rejected.output_path + ".pre-v13.bak"), "rejected SHA256 schema cannot migrate or write artifacts");
    };
    reject(fixture.envelope(version, fixture.sha256_payload(version), false), WalletLoadError::CORRUPT_DATA);
    auto payload = fixture.sha256_payload(version); payload[1] ^= 1;
    reject(fixture.envelope(version, payload), WalletLoadError::AUTHENTICATION_FAILED);
    payload = fixture.sha256_payload(version); payload.resize(32);
    reject(fixture.envelope(version, payload), WalletLoadError::CORRUPT_DATA);
    payload = fixture.sha256_payload(version); payload.push_back(0x87);
    reject(fixture.envelope(version, payload), WalletLoadError::CORRUPT_DATA);
    payload = fixture.sha256_payload(version); payload[0] = 9;
    reject(fixture.envelope(version, payload), WalletLoadError::CORRUPT_DATA);
    reject(fixture.envelope(13, fixture.sha256_payload(13)), WalletLoadError::CORRUPT_DATA);
    reject(fixture.envelope(14, fixture.sha256_payload(13)), WalletLoadError::UNSUPPORTED_VERSION);
    auto corrupted = original; corrupted.back() ^= 1;
    reject(corrupted, WalletLoadError::AUTHENTICATION_FAILED);
    // Corrupt GCM but recompute HMAC to ensure AES authentication is independently required.
    bytes body(original.begin(), original.end() - kWalletHmacSize);
    body.back() ^= 1;
    cipher.assign(body.begin() + kWalletHeaderSize, body.end());
    bytes mac;
    expect(compute_wallet_hmac(fixture.password, cipher, crypto::kPbkdf2Iterations, body, mac), "authenticate synthetic bad-GCM envelope");
    body.insert(body.end(), mac.begin(), mac.end());
    reject(body, WalletLoadError::AUTHENTICATION_FAILED);

    options.output_path = (root / "sha256-missing-directory" / "converted.dat").string();
    options.backup_path = (root / ("sha256-rollback-v" + std::to_string(version) + ".bak")).string();
    const auto rollback = Wallet::migrate_file(source.string(), fixture.password, options);
    expect(rollback.error == WalletMigrationError::WRITE_FAILED && !rollback.committed && read(source) == original &&
        read(wallet_path(options.backup_path)) == original, "failure after staged verifier upgrade preserves original SHA256 file and exact backup");
}

void load_state_regression_tests(const std::filesystem::path& root, const Fixture& fixture) {
    for (uint32_t version : {11U, 12U, 13U}) {
        const bool encrypted = version == 13;
        const auto source = root / ("clear-reuse-v" + std::to_string(version) + ".dat");
        const auto original = fixture.envelope(version, fixture.payload(version, encrypted), encrypted);
        write(source, original);
        Wallet wallet;
        expect(wallet.load_from_file_result(source.string(), encrypted ? fixture.password : "").ok(),
            "load synthetic wallet before clear/reuse regression");

        if (encrypted) {
            const auto active_path = wallet.get_file_path();
            auto invalid_path = source.string();
            invalid_path.push_back('\0');
            invalid_path += ".ignored";
            auto unchanged = [&] {
                fixture.verify(wallet, version);
                expect(wallet.get_file_path() == active_path && wallet.has_password_protection() &&
                    !wallet.is_vault() && wallet.last_load_result().error == WalletLoadError::IO_ERROR,
                    "invalid path retains active wallet binding and security state");
            };
            auto reject_invalid_path = [&](const std::string& path) {
                for (bool allow_backup : {false, true}) {
                    const auto result = wallet.load_from_file_result(path, fixture.password, allow_backup);
                    expect(!result.ok() && result.error == WalletLoadError::IO_ERROR,
                        "invalid path returns structured IO_ERROR with either backup setting");
                    unchanged();
                }
                expect(!wallet.load_from_file(path, fixture.password),
                    "boolean loader rejects invalid path without throwing");
                unchanged();
            };
            reject_invalid_path(invalid_path);
#if defined(_WIN32)
            auto invalid_utf8_path = source.string();
            invalid_utf8_path.push_back(static_cast<char>(0xff));
            reject_invalid_path(invalid_utf8_path);
#endif
        }

        wallet.clear();
        const auto cleared = wallet.last_load_result();
        expect(cleared.error == WalletLoadError::NONE && cleared.format == WalletFileFormat::UNKNOWN &&
            cleared.version == 0 && !cleared.migration_needed && !cleared.encrypted && !cleared.authenticated &&
            !cleared.recovered_from_backup && cleared.source_path.empty(), "clear resets complete load provenance");
        expect(wallet.get_key_count() == 0 && wallet.get_file_path() == Wallet::get_default_wallet_path() &&
            !wallet.has_password_protection(),
            "clear removes prior wallet binding and password state");
        expect(wallet.initialize(bytes(32, 23)), "reuse cleared wallet with a synthetic key");
        const auto address = wallet.get_address();
        const auto destination = root / ("reused-v" + std::to_string(version) + ".dat");
        expect(wallet.save_to_file(destination.string(), "") && wallet.save(""),
            "reused wallet saves plaintext twice without stale legacy/encryption policy");
        Wallet reopened;
        const auto result = reopened.load_from_file_result(destination.string(), "");
        expect(result.ok() && result.format == WalletFileFormat::NATIVE_V13 && !result.encrypted &&
            !result.migration_needed && reopened.get_address() == address && reopened.get_key_count() == 1,
            "reused wallet persists as a fresh plaintext native wallet");
        expect(read(source) == original, "invalid paths and clear/reuse never rewrite original fixture");
    }
}

void long_path_tests(const std::filesystem::path& root, const Fixture& fixture) {
    const auto directory = wallet_path(wallet_path_string(root / std::filesystem::path(u8"\u0434\u043b\u0438\u043d\u043d\u044b\u0439 \u043f\u0443\u0442\u044c") /
        std::string(120, 'x') / std::string(80, 'y')));
    std::filesystem::create_directories(directory);
    const auto source = directory / "legacy-wallet.dat";
    const auto original = fixture.envelope(11, fixture.sha256_payload(11));
    write(source, original);
    const auto source_utf8 = wallet_path_string(source);
    expect(source.native().size() > 260, "long synthetic wallet path exceeds legacy Windows limit");
    WalletMigrationOptions options; options.confirmed = true;
    options.output_path = wallet_path_string(directory / "converted-wallet.dat");
    const auto migration = Wallet::migrate_file(source_utf8, fixture.password, options);
    expect(migration.ok() && migration.durable && read(source) == original && read(wallet_path(migration.backup_path)) == original,
        "long Unicode path supports verified legacy conversion and backup");
    Wallet wallet;
    expect(wallet.load_from_file_result(options.output_path, fixture.password).ok() &&
        wallet.get_file_path() == absolute_wallet_path(options.output_path), "long Unicode native wallet reload and binding");
    fixture.verify(wallet, 11);
    expect(wallet.save(fixture.password) && wallet.enable_vault(fixture.password), "long Unicode native save and vault conversion");
    Wallet vault;
    expect(vault.load_from_file_result(options.output_path, fixture.password).ok() && vault.is_locked(), "long Unicode vault reload");
    const auto backup = wallet_path_string(directory / "vault-copy.dat");
    expect(vault.backup(backup), "long Unicode vault backup");
    Wallet restored;
    expect(restored.load_from_file_result(backup, fixture.password).ok() && restored.unlock(fixture.password, 10), "long Unicode backup restore");
    fixture.verify(restored, 11);
}
} // namespace

int main(int argc, char** argv) {
    try {
        // All fixtures are generated from fixed synthetic seeds; never read external wallets.
        const char* work_root = std::getenv("GEOCOIN_TEST_WORK_ROOT");
        const auto base = argc > 1 ? wallet_path(argv[1]) :
            (work_root && *work_root ? wallet_path(work_root) / "wallet-format-migration" :
                wallet_path("out/wallet-format-migration"));
        const auto root = std::filesystem::absolute(base) /
            ("synthetic-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(root);
        Fixture fixture;
        legacy_tests(root, fixture, 11); legacy_tests(root, fixture, 12);
        legacy_sha256_tests(root, fixture, 11); legacy_sha256_tests(root, fixture, 12);
        failure_tests(root, fixture); native_tests(root, fixture); path_tests(root, fixture);
        load_state_regression_tests(root, fixture);
        long_path_tests(root, fixture);
        std::vector<std::filesystem::path> pending{root};
        while (!pending.empty()) {
            const auto directory = wallet_path(wallet_path_string(pending.back()));
            pending.pop_back();
            for (const auto& entry : std::filesystem::directory_iterator(directory)) {
                const auto path = wallet_path(wallet_path_string(entry.path()));
                expect(wallet_path_string(path.filename()).find(".tmp.") == std::string::npos, "no migration temporary files leaked");
                if (std::filesystem::is_directory(path)) pending.push_back(path);
            }
        }
        std::cout << "Wallet format/migration: " << checks << " checks passed; synthetic fixtures: " << root << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Wallet format/migration failure after " << checks << " checks: " << error.what() << '\n';
        return 1;
    }
}
