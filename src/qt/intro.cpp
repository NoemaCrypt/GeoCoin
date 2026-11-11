// Copyright (c) 2011-2013 The Bitcoin developers
// Copyright (c) 2025 The GeoCoin developers
// Distributed under the MIT software license

#include "intro.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QGroupBox>
#include <QFileDialog>
#include <QDir>
#include <QStorageInfo>
#include <QDialogButtonBox>
#include <QMessageBox>
#include <QStandardPaths>
#include <QSettings>

/* Minimum free space (in GB) needed for data directory */
static const quint64 MINIMUM_FREE_SPACE_GB = 10;

Intro::Intro(QWidget *parent) :
    QDialog(parent)
{
    setWindowTitle(tr("Welcome to GeoCoin"));
    setupUi();
    setDataDirectory(getDefaultDataDirectory());
}

Intro::~Intro()
{
}

void Intro::setupUi()
{
    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->setSpacing(15);

    // Welcome message
    QLabel *welcomeLabel = new QLabel(
        tr("<b>Welcome to GeoCoin!</b><br><br>"
           "GeoCoin will download and store a copy of the blockchain. "
           "You can choose where to store this data."),
        this
    );
    welcomeLabel->setWordWrap(true);
    layout->addWidget(welcomeLabel);

    // Data directory selection
    QGroupBox *dirGroup = new QGroupBox(tr("Data Directory"), this);
    QVBoxLayout *dirLayout = new QVBoxLayout(dirGroup);

    // Default location radio
    dataDirDefault = new QRadioButton(tr("Use the default data directory"), this);
    dataDirDefault->setChecked(true);
    connect(dataDirDefault, &QRadioButton::clicked, this, &Intro::on_dataDirDefault_clicked);
    dirLayout->addWidget(dataDirDefault);

    // Custom location radio
    dataDirCustom = new QRadioButton(tr("Use a custom data directory:"), this);
    connect(dataDirCustom, &QRadioButton::clicked, this, &Intro::on_dataDirCustom_clicked);
    dirLayout->addWidget(dataDirCustom);

    // Path input
    QHBoxLayout *pathLayout = new QHBoxLayout();
    pathLayout->addSpacing(30);
    dataDirectoryEdit = new QLineEdit(this);
    dataDirectoryEdit->setEnabled(false);
    pathLayout->addWidget(dataDirectoryEdit);

    ellipsisButton = new QPushButton(tr("..."), this);
    ellipsisButton->setMaximumWidth(50);
    ellipsisButton->setEnabled(false);
    connect(ellipsisButton, &QPushButton::clicked, this, &Intro::on_ellipsisButton_clicked);
    pathLayout->addWidget(ellipsisButton);

    dirLayout->addLayout(pathLayout);

    // Free space label
    freeSpaceLabel = new QLabel(this);
    freeSpaceLabel->setStyleSheet("QLabel { color: gray; }");
    dirLayout->addWidget(freeSpaceLabel);

    layout->addWidget(dirGroup);

    // Warning label
    warningLabel = new QLabel(
        tr("<b>Note:</b> The blockchain will require at least %1 GB of disk space. "
           "This will grow over time.").arg(MINIMUM_FREE_SPACE_GB),
        this
    );
    warningLabel->setWordWrap(true);
    warningLabel->setStyleSheet("QLabel { color: #FF8C00; }");
    layout->addWidget(warningLabel);

    // Buttons
    QDialogButtonBox *buttonBox = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
        this
    );
    connect(buttonBox, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttonBox);

    setMinimumWidth(500);
}

QString Intro::getDataDirectory()
{
    return dataDirectoryEdit->text();
}

void Intro::setDataDirectory(const QString &dataDir)
{
    dataDirectoryEdit->setText(dataDir);
    updateFreeSpace();

    if (dataDir == getDefaultDataDirectory()) {
        dataDirDefault->setChecked(true);
        dataDirectoryEdit->setEnabled(false);
        ellipsisButton->setEnabled(false);
    } else {
        dataDirCustom->setChecked(true);
        dataDirectoryEdit->setEnabled(true);
        ellipsisButton->setEnabled(true);
    }
}

QString Intro::getDefaultDataDirectory()
{
    #ifdef Q_OS_WIN
        // Windows: %APPDATA%/GeoCoin
        return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    #elif defined(Q_OS_MAC)
        // macOS: ~/Library/Application Support/GeoCoin
        return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    #else
        // Linux/Unix: ~/.geocoin
        return QDir::homePath() + "/.geocoin";
    #endif
}

QString Intro::pickDataDirectory(QWidget *parent)
{
    // Check if data directory was previously saved in settings
    QSettings settings("GeoCoin", "GeoCoin-Qt");
    QString savedDir = settings.value("dataDir", QString()).toString();

    // If we have a saved directory, use it (user already chose)
    if (!savedDir.isEmpty()) {
        QDir dir(savedDir);
        if (dir.exists()) {
            return savedDir;
        }
    }

    // Если директория данных еще не выбрана, показываем стартовый диалог.
    Intro intro(parent);

    if (intro.exec() == QDialog::Accepted) {
        QString dataDir = intro.getDataDirectory();

        // Create directory if it doesn't exist
        QDir dir(dataDir);
        if (!dir.exists()) {
            if (!dir.mkpath(".")) {
                QMessageBox::critical(parent, tr("GeoCoin"),
                    tr("Error: Cannot create data directory:\n%1").arg(dataDir));
                return QString();
            }
        }

        // Save selected directory to settings
        settings.setValue("dataDir", dataDir);

        return dataDir;
    }

    // User cancelled
    return QString();
}

void Intro::on_ellipsisButton_clicked()
{
    QString dir = QFileDialog::getExistingDirectory(
        this,
        tr("Choose data directory"),
        dataDirectoryEdit->text(),
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks
    );

    if (!dir.isEmpty()) {
        setDataDirectory(dir + "/geocoin-data");
    }
}

void Intro::on_dataDirDefault_clicked()
{
    setDataDirectory(getDefaultDataDirectory());
}

void Intro::on_dataDirCustom_clicked()
{
    dataDirectoryEdit->setEnabled(true);
    ellipsisButton->setEnabled(true);
}

void Intro::updateFreeSpace()
{
    QString path = dataDirectoryEdit->text();
    QStorageInfo storage(path);

    if (storage.isValid() && storage.isReady()) {
        quint64 freeSpaceGB = storage.bytesAvailable() / (1024 * 1024 * 1024);

        QString freeSpaceText = tr("%1 GB of free space available").arg(freeSpaceGB);

        if (freeSpaceGB < MINIMUM_FREE_SPACE_GB) {
            freeSpaceLabel->setText(freeSpaceText + tr(" (Warning: Less than %1 GB)").arg(MINIMUM_FREE_SPACE_GB));
            freeSpaceLabel->setStyleSheet("QLabel { color: red; }");
        } else {
            freeSpaceLabel->setText(freeSpaceText);
            freeSpaceLabel->setStyleSheet("QLabel { color: green; }");
        }
    } else {
        freeSpaceLabel->setText(tr("Cannot determine free space"));
        freeSpaceLabel->setStyleSheet("QLabel { color: gray; }");
    }
}
