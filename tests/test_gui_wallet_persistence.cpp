#include "test_utils.hpp"
#include "walletmodel.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <iostream>

namespace {

class Model {
public:
    explicit Model(const QString& directory)
    {
        model.setDataDirectory(directory);
    }

    WalletModel model;
};

void check_create_close_and_reload()
{
    QTemporaryDir dir;
    geocoin::test::expect(dir.isValid(), "create wallet directory");
    const QString password = QStringLiteral("correct-horse");

    QString address;
    {
        Model session(dir.path());
        const QString path = session.model.getWalletFilePath();
        geocoin::test::expect(session.model.createWallet(path, password), "GUI createWallet writes an encrypted wallet");
        geocoin::test::expect(session.model.isWalletLoaded(), "created wallet is loaded");
        geocoin::test::expect(session.model.isWalletEncrypted(), "created wallet is encrypted");
        geocoin::test::expect(!session.model.hasUnsavedChanges(), "fresh wallet has no unsaved changes");
        address = session.model.getAddress();
        geocoin::test::expect(!address.isEmpty(), "created wallet has an address");
        geocoin::test::expect(QFile::exists(path), "wallet.dat exists after create");

        QString error;
        geocoin::test::expect(!session.model.saveWallet(QString(), &error), "save rejects a missing password");
        geocoin::test::expect(!error.isEmpty(), "missing password explains the failure");
        geocoin::test::expect(!session.model.saveWallet(QStringLiteral("wrong-password"), &error),
                     "save rejects the wrong password");
        geocoin::test::expect(session.model.saveWallet(password, &error), "save accepts the current password");
        geocoin::test::expect(!session.model.hasUnsavedChanges(), "successful save clears the dirty flag");

        session.model.closeWallet();
        geocoin::test::expect(!session.model.isWalletLoaded(), "closeWallet unloads the wallet");
        geocoin::test::expect(!session.model.saveWallet(password, &error), "save fails when no wallet is loaded");
        geocoin::test::expect(!session.model.loadWallet(path, QStringLiteral("wrong-password")),
                     "reload rejects the wrong password");
        geocoin::test::expect(!session.model.isWalletLoaded(), "failed reload does not leave a wallet open");
        geocoin::test::expect(session.model.loadWallet(path, password), "reload accepts the correct password");
        geocoin::test::expect(session.model.getAddress() == address, "reload restores the same address");
    }

    Model restarted(dir.path());
    const QString path = restarted.model.getWalletFilePath();
    geocoin::test::expect(restarted.model.loadWallet(path, password), "a new GUI session restores the wallet");
    geocoin::test::expect(restarted.model.getAddress() == address, "restored session has the original address");
    geocoin::test::expect(!restarted.model.createWallet(path, password), "create refuses to overwrite an existing wallet");
}

void check_backup_restore_and_passphrase_change()
{
    QTemporaryDir dir;
    QTemporaryDir backupDir;
    geocoin::test::expect(dir.isValid() && backupDir.isValid(), "create backup directories");
    const QString password = QStringLiteral("backup-secret");
    const QString replacement = QStringLiteral("replacement-secret");

    QString address;
    QString privateKey;
    {
        Model session(dir.path());
        const QString path = session.model.getWalletFilePath();
        geocoin::test::expect(session.model.createWallet(path, password), "create wallet for backup");
        address = session.model.getAddress();
        privateKey = session.model.exportPrivateKey(password);
        geocoin::test::expect(privateKey.size() == 64, "export returns a 32-byte key");
        geocoin::test::expect(session.model.exportPrivateKey(QStringLiteral("nope")).isEmpty(),
                     "export rejects the wrong password");

        const QString backupPath = QDir(backupDir.path()).filePath(QStringLiteral("wallet.dat"));
        QString error;
        geocoin::test::expect(!session.model.backupWallet(backupPath, QStringLiteral("nope"), &error),
                     "backup rejects the wrong password");
        geocoin::test::expect(session.model.backupWallet(backupPath, password, &error), "backup writes a second wallet file");
        geocoin::test::expect(session.model.getWalletFilePath() == path, "backup does not retarget the open wallet");
        geocoin::test::expect(!session.model.changeWalletPassphrase(QStringLiteral("nope"), replacement, &error),
                     "passphrase change rejects the current password");
        geocoin::test::expect(session.model.changeWalletPassphrase(password, replacement, &error),
                     "passphrase change rewrites the wallet");
        geocoin::test::expect(!session.model.encryptWallet(replacement, &error),
                     "encrypt rejects a wallet that is already encrypted");
    }

    {
        Model original(dir.path());
        geocoin::test::expect(!original.model.loadWallet(original.model.getWalletFilePath(), password),
                     "old password no longer opens the rewritten wallet");
        geocoin::test::expect(original.model.loadWallet(original.model.getWalletFilePath(), replacement),
                     "new password opens the rewritten wallet");
        geocoin::test::expect(original.model.getAddress() == address, "passphrase change keeps the address");
    }

    Model restored(backupDir.path());
    geocoin::test::expect(restored.model.loadWallet(restored.model.getWalletFilePath(), password),
                 "backup still opens with the password used to write it");
    geocoin::test::expect(restored.model.getAddress() == address, "backup restores the original address");

    QTemporaryDir importedDir;
    geocoin::test::expect(importedDir.isValid(), "create import directory");
    Model imported(importedDir.path());
    geocoin::test::expect(imported.model.importPrivateKey(privateKey, replacement, false),
                 "import restores a wallet from its exported key");
    geocoin::test::expect(imported.model.getAddress() == address, "imported key produces the original address");
    imported.model.closeWallet();
    geocoin::test::expect(imported.model.loadWallet(imported.model.getWalletFilePath(), replacement),
                 "imported wallet reloads with its new password");
    geocoin::test::expect(imported.model.getAddress() == address, "reloaded import keeps the address");
}

void check_unencrypted_wallet_can_be_encrypted()
{
    QTemporaryDir dir;
    geocoin::test::expect(dir.isValid(), "create unencrypted wallet directory");
    Model session(dir.path());
    const QString path = session.model.getWalletFilePath();
    geocoin::test::expect(session.model.createWallet(path, QString()), "create an unencrypted wallet");
    geocoin::test::expect(session.model.isWalletLoaded() && !session.model.isWalletEncrypted(),
                 "empty password leaves the wallet unencrypted");
    const QString address = session.model.getAddress();
    QString error;
    geocoin::test::expect(session.model.saveWallet(QString(), &error), "unencrypted wallet saves without a password");
    const QString password = QStringLiteral("new-encryption");
    geocoin::test::expect(session.model.encryptWallet(password, &error), "encryptWallet protects an open wallet");
    geocoin::test::expect(session.model.isWalletEncrypted(), "wallet reports encryption after encryptWallet");
    session.model.closeWallet();
    geocoin::test::expect(!session.model.loadWallet(path, QString()), "empty password no longer opens an encrypted wallet");
    geocoin::test::expect(session.model.loadWallet(path, password), "encryption password opens the wallet");
    geocoin::test::expect(session.model.getAddress() == address, "encryption keeps the address");
}

void overwrite_with_garbage(const QString& path)
{
    QFile file(path);
    geocoin::test::expect(file.open(QIODevice::WriteOnly | QIODevice::Truncate), "overwrite wallet file");
    const QByteArray payload("corrupt-wallet");
    geocoin::test::expect(file.write(payload) == payload.size(), "write a corrupt wallet payload");
}

void check_corrupt_file_recovery()
{
    QTemporaryDir dir;
    geocoin::test::expect(dir.isValid(), "create corrupt wallet directory");
    Model session(dir.path());
    const QString path = session.model.getWalletFilePath();
    const QString password = QStringLiteral("intact-wallet");
    geocoin::test::expect(session.model.createWallet(path, password), "create wallet before corruption");
    const QString address = session.model.getAddress();
    QString saveError;
    geocoin::test::expect(session.model.saveWallet(password, &saveError),
                          "a second save keeps the previous wallet as backup");
    session.model.closeWallet();

    overwrite_with_garbage(path);
    geocoin::test::expect(session.model.loadWallet(path, password),
                 "a corrupt primary wallet is restored from its backup");
    geocoin::test::expect(session.model.getAddress() == address, "backup recovery keeps the address");
    session.model.closeWallet();

    overwrite_with_garbage(path);
    overwrite_with_garbage(path + QStringLiteral(".bak"));
    geocoin::test::expect(!session.model.loadWallet(path, password),
                 "corrupt primary and backup files are rejected");
    geocoin::test::expect(!session.model.isWalletLoaded(), "rejected files do not become the open wallet");
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication::setOrganizationName(QStringLiteral("GeoCoinWalletPersistenceTest"));
    QCoreApplication::setApplicationName(QStringLiteral("GeoCoinWalletPersistenceTest"));
    QApplication application(argc, argv);

    try {
        check_create_close_and_reload();
        check_backup_restore_and_passphrase_change();
        check_unencrypted_wallet_can_be_encrypted();
        check_corrupt_file_recovery();
        std::cout << "GUI wallet persistence tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
