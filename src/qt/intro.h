// Copyright (c) 2011-2013 The Bitcoin developers
// Copyright (c) 2025 The GeoCoin developers
// Distributed under the MIT software license

#ifndef GEOCOIN_QT_INTRO_H
#define GEOCOIN_QT_INTRO_H

#include <QDialog>
#include <QString>

QT_BEGIN_NAMESPACE
class QLabel;
class QLineEdit;
class QPushButton;
class QRadioButton;
QT_END_NAMESPACE

/** Introduction screen (pre-GUI startup).
  Allows the user to choose a data directory,
  in which the wallet and block chain will be stored.
 */
class Intro : public QDialog
{
    Q_OBJECT

public:
    explicit Intro(QWidget *parent = nullptr);
    ~Intro();

    QString getDataDirectory();
    void setDataDirectory(const QString &dataDir);

    /**
     * Determine data directory. Let the user choose if the current one doesn't exist.
     */
    static QString pickDataDirectory(QWidget *parent = nullptr);

    /**
     * Determine default data directory for operating system.
     */
    static QString getDefaultDataDirectory();

private slots:
    void on_ellipsisButton_clicked();
    void on_dataDirDefault_clicked();
    void on_dataDirCustom_clicked();

private:
    QLineEdit *dataDirectoryEdit;
    QRadioButton *dataDirDefault;
    QRadioButton *dataDirCustom;
    QPushButton *ellipsisButton;
    QLabel *freeSpaceLabel;
    QLabel *warningLabel;

    void setupUi();
    void updateFreeSpace();
};

#endif // GEOCOIN_QT_INTRO_H
