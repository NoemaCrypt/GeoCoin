// Copyright (c) 2026 The GeoCoin developers
// Distributed under the MIT software license

#pragma once

#include "multisigpage.h"

#include "clientmodel.h"
#include "walletmodel.h"
#include "commitmentscanner.h"
#include "guiutil.h"
#include "qtlogging.h"
#include "geocoin/wallet/multisig.hpp"
#include "geocoin/wallet/wallet.hpp"
#include "geocoin/state/state.hpp"
#include "geocoin/consensus/consensus.hpp"
#include "geocoin/types.hpp"
#include "geocoin/crypto/geo_area.hpp"

#include <QApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QButtonGroup>
#include <QAbstractButton>
#include <QCheckBox>
#include <QClipboard>
#include <QCryptographicHash>
#include <QComboBox>
#include <QDateTime>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollArea>
#include <QSizePolicy>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QDir>
#include <QFileInfo>
#include <QSignalBlocker>
#include <QTextCursor>
#include <QTextEdit>
#include <QTableWidget>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QVBoxLayout>
#include <QRegularExpression>
#include <QDialogButtonBox>
#include <QTimer>
#include <cstdlib>

namespace {
bool readExactJsonAmount(const QJsonObject& object,
                         const QString& anglesField,
                         const QString& gcField,
                         qint64& amount)
{
    const QJsonValue angles = object.value(anglesField);
    if (angles.isString()) {
        const QString text = angles.toString();
        if (text.isEmpty() || text.size() > 19) return false;
        for (const QChar ch : text) {
            if (ch < QLatin1Char('0') || ch > QLatin1Char('9')) return false;
        }
        bool ok = false;
        const qint64 parsed = text.toLongLong(&ok, 10);
        if (ok && parsed >= 0) {
            amount = parsed;
            return true;
        }
        return false;
    }
    const QJsonValue gc = object.value(gcField);
    return gc.isString() && GUIUtil::parseAmount(gc.toString(), amount);
}
}
