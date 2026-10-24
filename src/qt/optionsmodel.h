// Copyright (c) 2011-2013 The Bitcoin developers
// Copyright (c) 2025 The GeoCoin developers
// Distributed under the MIT software license

#ifndef GEOCOIN_QT_OPTIONSMODEL_H
#define GEOCOIN_QT_OPTIONSMODEL_H

#include <QObject>
#include <QString>
#include <QStringList>

/** Interface from Qt to configuration data structure for GeoCoin client */
class OptionsModel : public QObject
{
    Q_OBJECT

public:
    explicit OptionsModel(QObject *parent = nullptr);
    ~OptionsModel();

    // Централизованные UI-настройки (QSettings).
    QString getLanguage() const;
    void setLanguage(const QString& languageCode);

    QString getTheme() const;
    void setTheme(const QString& theme);

    QString getStartupPage() const;
    void setStartupPage(const QString& pageId);

    bool getRestoreLastPage() const;
    void setRestoreLastPage(bool enabled);

    static QStringList availableStartupPages();
    static bool isValidStartupPage(const QString& pageId);
};

#endif // GEOCOIN_QT_OPTIONSMODEL_H
