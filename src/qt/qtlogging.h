// Copyright (c) 2026 The GeoCoin developers
// Distributed under the MIT software license

#ifndef GEOCOIN_QT_QTLOGGING_H
#define GEOCOIN_QT_QTLOGGING_H

#include <QDebug>
#include <cstdlib>

namespace geocoin::qt {

inline bool debugEnabled() {
    static const bool enabled = (std::getenv("GEOCOIN_QT_DEBUG") != nullptr);
    return enabled;
}

inline bool infoEnabled() {
    static const bool enabled = (std::getenv("GEOCOIN_QT_INFO") != nullptr) || debugEnabled();
    return enabled;
}

} // namespace geocoin::qt

#define GEO_QT_LOG_DEBUG if (!::geocoin::qt::debugEnabled()) {} else qDebug()
#define GEO_QT_LOG_INFO if (!::geocoin::qt::infoEnabled()) {} else qInfo()

#endif // GEOCOIN_QT_QTLOGGING_H
