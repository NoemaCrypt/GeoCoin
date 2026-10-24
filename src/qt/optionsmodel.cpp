// Copyright (c) 2011-2013 The Bitcoin developers
// Copyright (c) 2025 The GeoCoin developers
// Distributed under the MIT software license

#include "optionsmodel.h"

#include <QSettings>

OptionsModel::OptionsModel(QObject *parent) :
    QObject(parent)
{
}

OptionsModel::~OptionsModel()
{
}

QString OptionsModel::getLanguage() const
{
    QSettings settings;
    return settings.value(QStringLiteral("ui/language"), QStringLiteral("en")).toString();
}

void OptionsModel::setLanguage(const QString& languageCode)
{
    QSettings settings;
    const QString code = languageCode.trimmed().toLower();
    settings.setValue(QStringLiteral("ui/language"), code.isEmpty() ? QStringLiteral("en") : code);
}

QString OptionsModel::getTheme() const
{
    QSettings settings;
    const QString theme = settings.value(QStringLiteral("ui/theme"), QStringLiteral("light")).toString().trimmed().toLower();
    return theme == QStringLiteral("dark") ? QStringLiteral("dark") : QStringLiteral("light");
}

void OptionsModel::setTheme(const QString& theme)
{
    QSettings settings;
    const QString value = theme.trimmed().toLower();
    settings.setValue(QStringLiteral("ui/theme"), value == QStringLiteral("dark") ? QStringLiteral("dark") : QStringLiteral("light"));
}

QString OptionsModel::getStartupPage() const
{
    QSettings settings;
    const QString page = settings.value(QStringLiteral("ui/startup_page"), QStringLiteral("overview")).toString();
    return isValidStartupPage(page) ? page : QStringLiteral("overview");
}

void OptionsModel::setStartupPage(const QString& pageId)
{
    QSettings settings;
    settings.setValue(QStringLiteral("ui/startup_page"), isValidStartupPage(pageId) ? pageId : QStringLiteral("overview"));
}

bool OptionsModel::getRestoreLastPage() const
{
    QSettings settings;
    return settings.value(QStringLiteral("ui/restore_last_page"), true).toBool();
}

void OptionsModel::setRestoreLastPage(bool enabled)
{
    QSettings settings;
    settings.setValue(QStringLiteral("ui/restore_last_page"), enabled);
}

QStringList OptionsModel::availableStartupPages()
{
    return {
        QStringLiteral("overview"),
        QStringLiteral("send"),
        QStringLiteral("receive"),
        QStringLiteral("history"),
        QStringLiteral("mining"),
        QStringLiteral("multisig"),
        QStringLiteral("corporate"),
        QStringLiteral("nft")
    };
}

bool OptionsModel::isValidStartupPage(const QString& pageId)
{
    return availableStartupPages().contains(pageId);
}
