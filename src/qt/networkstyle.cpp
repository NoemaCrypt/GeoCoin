// Copyright (c) 2014 The Bitcoin developers
// Copyright (c) 2025 The GeoCoin developers
// Distributed under the MIT software license

#include "networkstyle.h"

#include <QApplication>

NetworkStyle::NetworkStyle(const QString &_appName, const int iconColorHueShift,
                          const int iconColorSaturationReduction, const char *_titleAddText):
    appName(_appName),
    titleAddText(qApp->translate("SplashScreen", _titleAddText))
{
    // Загружаем иконки из ресурсов
    appIcon = QIcon(":/icons/geocoin");
    trayAndWindowIcon = QIcon(":/icons/geocoin");
}

const NetworkStyle *NetworkStyle::instantiate(const QString &networkId)
{
    if (networkId == "main") {
        return new NetworkStyle("GeoCoin", 0, 0, "");
    } else if (networkId == "test") {
        return new NetworkStyle("GeoCoin", 70, 30, "[testnet]");
    } else if (networkId == "regtest") {
        return new NetworkStyle("GeoCoin", 160, 30, "[regtest]");
    }
    return nullptr;
}
