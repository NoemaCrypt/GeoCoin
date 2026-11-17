#include "test_utils.hpp"
#include "walletmodel.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QSettings>
#include <QElapsedTimer>
#include <QThread>
#include <QProcess>

#include <iostream>

namespace {

class Model {
public:
    explicit Model(const QString& directory)
    {
        model.setDataDirectory(directory);
        path = QDir(directory).filePath(QStringLiteral("selected-name.wallet"));
    }

    WalletModel model;
    QString path;
};

void quiesce(WalletModel& model)
{
    model.beginQuiesce();
    QElapsedTimer timeout;
    timeout.start();
    while (!model.isQuiesced() && timeout.elapsed() < 5000) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    geocoin::test::expect(model.isQuiesced(), "wallet synchronization quiesces before close");
}

void quiesce_and_close(WalletModel& model)
{
    quiesce(model);
    model.closeWallet();
}

void check_create_close_and_reload()
{
    QTemporaryDir dir;
    geocoin::test::expect(dir.isValid(), "create wallet directory");
    const QString password = QStringLiteral("correct-horse");

    QString address;
    {
        Model session(dir.path());
        const QString path = session.path;
        geocoin::test::expect(session.model.getWalletFilePath().isEmpty(), "data directory does not force a wallet filename");
        geocoin::test::expect(session.model.createWallet(path, password), "GUI createWallet writes an encrypted wallet");
        geocoin::test::expect(session.model.isWalletLoaded(), "created wallet is loaded");
        geocoin::test::expect(session.model.isWalletEncrypted(), "created wallet is encrypted");
        geocoin::test::expect(!session.model.hasUnsavedChanges(), "fresh wallet has no unsaved changes");
        address = session.model.getAddress();
        geocoin::test::expect(!address.isEmpty(), "created wallet has an address");
        geocoin::test::expect(QFile::exists(path), "selected wallet filename exists after create");

        QString error;
        geocoin::test::expect(!session.model.saveWallet(QString(), &error), "save rejects a missing password");
        geocoin::test::expect(!error.isEmpty(), "missing password explains the failure");
        geocoin::test::expect(!session.model.saveWallet(QStringLiteral("wrong-password"), &error),
                     "save rejects the wrong password");
        geocoin::test::expect(session.model.saveWallet(password, &error), "save accepts the current password");
        geocoin::test::expect(!session.model.hasUnsavedChanges(), "successful save clears the dirty flag");

        quiesce_and_close(session.model);
        geocoin::test::expect(!session.model.isWalletLoaded(), "closeWallet unloads the wallet");
        geocoin::test::expect(!session.model.saveWallet(password, &error), "save fails when no wallet is loaded");
        geocoin::test::expect(!session.model.loadWallet(path, QStringLiteral("wrong-password")),
                     "reload rejects the wrong password");
        geocoin::test::expect(!session.model.isWalletLoaded(), "failed reload does not leave a wallet open");
        geocoin::test::expect(session.model.loadWallet(path, password), "reload accepts the correct password");
        geocoin::test::expect(session.model.getAddress() == address, "reload restores the same address");
    }

    Model restarted(dir.path());
    const QString path = restarted.path;
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
        const QString path = session.path;
        geocoin::test::expect(session.model.createWallet(path, password), "create wallet for backup");
        address = session.model.getAddress();
        privateKey = session.model.exportPrivateKey(password);
        geocoin::test::expect(privateKey.size() == 64, "export returns a 32-byte key");
        geocoin::test::expect(session.model.exportPrivateKey(QStringLiteral("nope")).isEmpty(),
                     "export rejects the wrong password");

        const QString backupPath = QDir(backupDir.path()).filePath(QStringLiteral("selected-name.wallet"));
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
        geocoin::test::expect(!original.model.loadWallet(original.path, password),
                     "old password no longer opens the rewritten wallet");
        geocoin::test::expect(original.model.loadWallet(original.path, replacement),
                     "new password opens the rewritten wallet");
        geocoin::test::expect(original.model.getAddress() == address, "passphrase change keeps the address");
    }

    Model restored(backupDir.path());
    geocoin::test::expect(restored.model.loadWallet(restored.path, password),
                 "backup still opens with the password used to write it");
    geocoin::test::expect(restored.model.getAddress() == address, "backup restores the original address");

    QTemporaryDir importedDir;
    geocoin::test::expect(importedDir.isValid(), "create import directory");
    Model imported(importedDir.path());
    geocoin::test::expect(imported.model.createWallet(imported.path, replacement), "choose an explicit destination for imported keys");
    quiesce(imported.model);
    geocoin::test::expect(imported.model.importPrivateKey(privateKey, replacement, false),
                 "import restores a wallet from its exported key");
    geocoin::test::expect(imported.model.getAddress() == address, "imported key produces the original address");
    quiesce_and_close(imported.model);
    geocoin::test::expect(imported.model.loadWallet(imported.path, replacement),
                 "imported wallet reloads with its new password");
    geocoin::test::expect(imported.model.getAddress() == address, "reloaded import keeps the address");
}

void check_unencrypted_wallet_can_be_encrypted()
{
    QTemporaryDir dir;
    geocoin::test::expect(dir.isValid(), "create unencrypted wallet directory");
    Model session(dir.path());
    const QString path = session.path;
    geocoin::test::expect(session.model.createWallet(path, QString()), "create an unencrypted wallet");
    geocoin::test::expect(session.model.isWalletLoaded() && !session.model.isWalletEncrypted(),
                 "empty password leaves the wallet unencrypted");
    const QString address = session.model.getAddress();
    QString error;
    geocoin::test::expect(session.model.saveWallet(QString(), &error), "unencrypted wallet saves without a password");
    const QString password = QStringLiteral("new-encryption");
    geocoin::test::expect(session.model.encryptWallet(password, &error), "encryptWallet protects an open wallet");
    geocoin::test::expect(session.model.isWalletEncrypted(), "wallet reports encryption after encryptWallet");
    quiesce_and_close(session.model);
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
    const QString path = session.path;
    const QString password = QStringLiteral("intact-wallet");
    geocoin::test::expect(session.model.createWallet(path, password), "create wallet before corruption");
    const QString address = session.model.getAddress();
    QString saveError;
    geocoin::test::expect(session.model.saveWallet(password, &saveError),
                          "a second save keeps the previous wallet as backup");
    quiesce_and_close(session.model);

    overwrite_with_garbage(path);
    geocoin::test::expect(!session.model.loadWallet(path, password),
                 "selected corrupt primary fails without silently opening its backup");
    geocoin::test::expect(!session.model.isWalletLoaded(), "failed primary load leaves no active wallet");
    geocoin::test::expect(session.model.loadWallet(path + QStringLiteral(".bak"), password),
                 "explicitly selecting the intact backup restores the wallet");
    geocoin::test::expect(session.model.getAddress() == address, "explicit backup restore keeps the address");
    geocoin::test::expect(session.model.getWalletFilePath() == path + QStringLiteral(".bak"), "explicit backup becomes the selected active path");
    quiesce_and_close(session.model);

    overwrite_with_garbage(path);
    overwrite_with_garbage(path + QStringLiteral(".bak"));
    geocoin::test::expect(!session.model.loadWallet(path, password),
                 "corrupt primary and backup files are rejected");
    geocoin::test::expect(!session.model.isWalletLoaded(), "rejected files do not become the open wallet");
}

void check_staged_paths_and_locks()
{
    QTemporaryDir nodeA;
    QTemporaryDir nodeB;
    QTemporaryDir walletDir;
    geocoin::test::expect(nodeA.isValid() && nodeB.isValid() && walletDir.isValid(), "create isolated node and wallet directories");
    const QString first = walletDir.filePath(QStringLiteral("active arbitrary name.bin"));
    const QString second = walletDir.filePath(QStringLiteral("candidate.data"));
    const QString password = QStringLiteral("staged-wallet-secret");
    Model active(nodeA.path());
    geocoin::test::expect(active.model.createWallet(first, password), "wallet can live outside its node directory with an arbitrary name");
    const auto original = active.model.getWallet();
    const QString address = active.model.getAddress();
    const QString remembered = WalletModel::initialWalletPath(nodeA.path());
    geocoin::test::expect(remembered == first, "only the successful selected wallet is remembered");
    geocoin::test::expect(WalletModel::initialWalletPath(nodeB.path()).isEmpty(), "remembered path is scoped to the node directory");
    QString saveError;
    geocoin::test::expect(active.model.saveWallet(password, &saveError), "create the active wallet's backup");
    {
        Model backupOwner(nodeB.path());
        geocoin::test::expect(backupOwner.model.loadWallet(first + QStringLiteral(".bak"), password), "explicit backup can be active independently");
        geocoin::test::expect(!active.model.saveWallet(password, &saveError) && saveError.contains(QStringLiteral("backup_lock")),
            "primary save cannot overwrite an independently active backup");
        quiesce_and_close(backupOwner.model);
    }
    geocoin::test::expect(active.model.saveWallet(password, &saveError), "save succeeds after the backup owner releases its lock");
    {
        Model candidate(nodeB.path());
        geocoin::test::expect(candidate.model.createWallet(second, password), "create an independent candidate wallet");
        quiesce_and_close(candidate.model);
    }
    geocoin::test::expect(!active.model.stageWallet(second, QStringLiteral("bad-passphrase")), "wrong-password candidate is rejected");
    geocoin::test::expect(active.model.walletOperationCode() == QStringLiteral("authentication_failed"), "password failure is structured");
    geocoin::test::expect(!active.model.stageWallet(walletDir.filePath(QStringLiteral("missing.wallet")), password), "missing candidate is rejected without creating a file");
    geocoin::test::expect(active.model.getWallet() == original && active.model.getAddress() == address, "candidate errors retain the original wallet and address");
    geocoin::test::expect(active.model.stageWallet(second, password), "validated candidate can be staged beside the active wallet");
    geocoin::test::expect(active.model.getWallet() == original, "staging alone does not switch the wallet");
    active.model.discardStagedWallet();
    geocoin::test::expect(active.model.getWallet() == original && WalletModel::initialWalletPath(nodeA.path()) == remembered,
        "canceling the staged candidate retains the wallet and remembered path");
    Model contender(nodeB.path());
    geocoin::test::expect(!contender.model.stageWallet(first, password) && contender.model.walletOperationCode() == QStringLiteral("wallet_lock"),
        "per-wallet lock collides even with a different node directory");
    QProcess child;
    child.start(QCoreApplication::applicationFilePath(), {QStringLiteral("-platform"), QStringLiteral("offscreen"),
        QStringLiteral("--wallet-lock-probe"), first});
    geocoin::test::expect(child.waitForFinished(15000) && child.exitStatus() == QProcess::NormalExit && child.exitCode() == 0,
        "wallet lock also excludes a second process");
    geocoin::test::expect(contender.model.stageWallet(second, password), "cancel releases the candidate lock");
    contender.model.discardStagedWallet();
    geocoin::test::expect(active.model.stageWallet(second, password), "candidate can be staged again");
    geocoin::test::expect(!active.model.activateStagedWallet(), "active wallet cannot switch before quiescing");
    quiesce(active.model);
    geocoin::test::expect(active.model.activateStagedWallet(), "quiesced candidate becomes active");
    geocoin::test::expect(active.model.getWalletFilePath() == second && active.model.getWallet() != original, "switch activates the selected candidate path");
    geocoin::test::expect(contender.model.stageWallet(first, password), "switch releases the previous wallet lock");
    contender.model.discardStagedWallet();
    quiesce_and_close(active.model);
    active.model.closeWallet();
    geocoin::test::expect(!active.model.isWalletLoaded(), "double close is idempotent after quiescing");
}

} // namespace

int main(int argc, char** argv)
{
    QTemporaryDir settingsDir;
    if (!settingsDir.isValid()) return 1;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, settingsDir.path());
    QCoreApplication::setOrganizationName(QStringLiteral("GeoCoinWalletPersistenceTest"));
    QCoreApplication::setApplicationName(QStringLiteral("GeoCoinWalletPersistenceTest"));
    QApplication application(argc, argv);
    const int probe = application.arguments().indexOf(QStringLiteral("--wallet-lock-probe"));
    if (probe >= 0 && probe + 1 < application.arguments().size()) {
        WalletModel contender;
        const bool opened = contender.stageWallet(application.arguments()[probe + 1], QString());
        return !opened && contender.walletOperationCode() == QStringLiteral("wallet_lock") ? 0 : 2;
    }

    try {
        check_create_close_and_reload();
        check_backup_restore_and_passphrase_change();
        check_unencrypted_wallet_can_be_encrypted();
        check_corrupt_file_recovery();
        check_staged_paths_and_locks();
        std::cout << "GUI wallet persistence tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
