// Copyright (c) 2014 The Bitcoin developers
// Copyright (c) 2025 The GeoCoin developers
// Distributed under the MIT software license

#ifndef GEOCOIN_QT_NETWORKSTYLE_H
#define GEOCOIN_QT_NETWORKSTYLE_H

#include <QString>
#include <QIcon>
#include <QPixmap>

/** Coin network-specific GUI style information */
class NetworkStyle
{
public:
    /** Get style associated with provided network id, or 0 if not known */
    static const NetworkStyle *instantiate(const QString &networkId);

    const QString &getAppName() const { return appName; }
    const QIcon &getAppIcon() const { return appIcon; }
    const QIcon &getTrayAndWindowIcon() const { return trayAndWindowIcon; }
    const QString &getTitleAddText() const { return titleAddText; }

    NetworkStyle(const QString &appName, const int iconColorHueShift, const int iconColorSaturationReduction, const char *titleAddText);

private:
    QString appName;
    QIcon appIcon;
    QIcon trayAndWindowIcon;
    QString titleAddText;
};

#endif // GEOCOIN_QT_NETWORKSTYLE_H
