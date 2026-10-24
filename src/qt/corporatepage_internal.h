// Copyright (c) 2026 The GeoCoin developers
// Distributed under the MIT software license

#pragma once

#include "corporatepage.h"
#include "clientmodel.h"
#include "walletmodel.h"
#include "commitmentscanner.h"
#include "guiutil.h"
#include "qtlogging.h"
#include "geocoin/state/state.hpp"
#include "geocoin/core/transaction.hpp"
#include "geocoin/consensus/consensus.hpp"
#include "geocoin/crypto/addresses.hpp"
#include "geocoin/crypto/geo_area.hpp"
#include "geocoin/wallet/multisig.hpp"
#include "geocoin/wallet/wallet.hpp"

#include <QApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QButtonGroup>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDateTime>
#include <QDoubleSpinBox>
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
#include <QRegularExpression>
#include <QScrollArea>
#include <QSizePolicy>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QDir>
#include <QTextEdit>
#include <QTableWidget>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QVBoxLayout>
#include <cstdlib>

namespace {
constexpr int kMaxFounderCommitments = 15;
constexpr int kMaxDirectorCommitments = 20;

inline bool isHexTxid(const QString& value)
{
    static const QRegularExpression txidRe(QStringLiteral("^[0-9a-fA-F]{64}$"));
    return txidRe.match(value.trimmed()).hasMatch();
}

inline QString extractHexTxid(const QString& value)
{
    const QString trimmed = value.trimmed();
    if (isHexTxid(trimmed)) {
        return trimmed;
    }

    static const QRegularExpression embeddedTxidRe(QStringLiteral("([0-9a-fA-F]{64})"));
    const QRegularExpressionMatch match = embeddedTxidRe.match(trimmed);
    return match.hasMatch() ? match.captured(1) : QString();
}

}
