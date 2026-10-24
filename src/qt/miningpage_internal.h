// Copyright (c) 2025 The GeoCoin developers
// Distributed under the MIT software license

#pragma once

#include "miningpage.h"
#include "clientmodel.h"
#include "walletmodel.h"
#include "guiutil.h"
#include "geocoin/rpc/rpc_server.hpp"
#include "geocoin/types.hpp"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QCheckBox>
#include <QListWidget>
#include <QListWidgetItem>
#include <QTabWidget>
#include <QTabBar>
#include <QMessageBox>
#include <QSettings>
#include <QSizePolicy>
#include <QProcess>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileDevice>
#include <QFont>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalBlocker>
#include <QStandardPaths>
#include <QTemporaryFile>
#include <QTimer>
#include <QUuid>
#include <QVector>
#include <algorithm>

namespace {
constexpr int RoleCudaId = Qt::UserRole;
constexpr int RoleHipId = Qt::UserRole + 1;
constexpr int RoleOpenClId = Qt::UserRole + 2;
constexpr int RoleBaseText = Qt::UserRole + 3;

int cpuMiningThreadLimit()
{
    return GUIUtil::availableCpuThreads();
}

void stopProcessQuietly(QProcess* process, int gracefulTimeoutMs = 1500, int killTimeoutMs = 1500)
{
    if (!process || process->state() == QProcess::NotRunning) {
        return;
    }
    process->terminate();
    if (!process->waitForFinished(gracefulTimeoutMs)) {
        process->kill();
        process->waitForFinished(killTimeoutMs);
    }
}

int itemIntRole(const QListWidgetItem* item, int role, int fallback = -1)
{
    if (!item) {
        return fallback;
    }
    bool ok = false;
    const int value = item->data(role).toInt(&ok);
    return ok ? value : fallback;
}

QString normalizedGpuName(QString value)
{
    value = value.toLower().simplified();
    value.remove(QStringLiteral("nvidia corporation"));
    value.remove(QStringLiteral("advanced micro devices"));
    value.remove(QStringLiteral("amd"));
    value.remove(QStringLiteral("nvidia"));
    value.remove(QStringLiteral("(r)"));
    value.remove(QStringLiteral("(tm)"));
    return value.simplified();
}

QString vendorFamily(const QJsonObject& primary, const QJsonObject& fallback);

bool samePhysicalGpuModel(const QJsonObject& backendDevice, const QJsonObject& openClDevice)
{
    const QString cudaName = normalizedGpuName(backendDevice.value(QStringLiteral("name")).toString());
    const QString openClName = normalizedGpuName(openClDevice.value(QStringLiteral("name")).toString());
    if (cudaName.isEmpty() || openClName.isEmpty()) {
        return false;
    }
    if (cudaName == openClName || cudaName.contains(openClName) || openClName.contains(cudaName)) {
        return true;
    }

    const QString backendDetail = normalizedGpuName(backendDevice.value(QStringLiteral("detail")).toString());
    const QString openClDetail = normalizedGpuName(openClDevice.value(QStringLiteral("detail")).toString());
    const QString backendVendor = vendorFamily(backendDevice, QJsonObject());
    const QString openClVendor = vendorFamily(openClDevice, QJsonObject());
    if (!backendVendor.isEmpty() && backendVendor == openClVendor) {
        if (!backendDetail.isEmpty() && (backendDetail.contains(openClName) || openClName.contains(backendDetail))) {
            return true;
        }
        if (!openClDetail.isEmpty() && (openClDetail.contains(cudaName) || cudaName.contains(openClDetail))) {
            return true;
        }
    }
    return false;
}

QString deviceDisplayName(const QJsonObject& primary, const QJsonObject& fallback)
{
    QString name = primary.value(QStringLiteral("name")).toString().simplified();
    if (name.isEmpty()) {
        name = fallback.value(QStringLiteral("name")).toString().simplified();
    }
    if (name.isEmpty()) {
        name = primary.value(QStringLiteral("vendor")).toString().simplified();
    }
    return name.isEmpty() ? QStringLiteral("GPU") : name;
}

QString vendorFamily(const QJsonObject& primary, const QJsonObject& fallback)
{
    const QString raw = (primary.value(QStringLiteral("vendor")).toString() + QLatin1Char(' ') +
                         primary.value(QStringLiteral("name")).toString() + QLatin1Char(' ') +
                         fallback.value(QStringLiteral("vendor")).toString() + QLatin1Char(' ') +
                         fallback.value(QStringLiteral("name")).toString()).toLower();
    if (raw.contains(QStringLiteral("amd")) || raw.contains(QStringLiteral("advanced micro devices")) || raw.contains(QStringLiteral("radeon"))) {
        return QStringLiteral("AMD");
    }
    if (raw.contains(QStringLiteral("nvidia")) || raw.contains(QStringLiteral("geforce")) || raw.contains(QStringLiteral("rtx"))) {
        return QStringLiteral("Nvidia");
    }
    return QString();
}

QString unifiedGpuText(const QString& name,
                       int cudaId,
                       int hipId,
                       int openClId,
                       const QString& vendor,
                       const QString& detail)
{
    QStringList capabilities;
    QStringList ids;
    if (cudaId >= 0) {
        capabilities << QStringLiteral("Nvidia CUDA");
        ids << QStringLiteral("CUDA:%1").arg(cudaId);
    }
    if (hipId >= 0) {
        capabilities << QStringLiteral("AMD HIP");
        ids << QStringLiteral("HIP:%1").arg(hipId);
    }
    if (openClId >= 0) {
        if (cudaId < 0 && hipId < 0 && !vendor.isEmpty()) {
            capabilities << QStringLiteral("%1 OpenCL").arg(vendor);
        } else {
            capabilities << QStringLiteral("OpenCL");
        }
        ids << QStringLiteral("OpenCL:%1").arg(openClId);
    }

    QString text = QStringLiteral("%1 (%2)")
        .arg(name, capabilities.join(QLatin1Char('/')));
    if (!ids.isEmpty()) {
        text += QStringLiteral("  ") + ids.join(QStringLiteral(" / "));
    }
    if (!detail.trimmed().isEmpty()) {
        text += QStringLiteral("  ") + detail.trimmed();
    }
    return text;
}

QString gpuBackendButtonStyleSheet()
{
    const bool dark = GUIUtil::isDarkTheme();
    const QString fg = dark ? QStringLiteral("#F8EBD0") : QStringLiteral("#20170B");
    const QString bg = dark ? QStringLiteral("rgba(20, 26, 38, 165)") : QStringLiteral("rgba(255, 255, 255, 74)");
    const QString bgHover = dark ? QStringLiteral("rgba(42, 51, 68, 205)") : QStringLiteral("rgba(255, 250, 232, 150)");
    const QString checkedBg = dark ? QStringLiteral("#D6A84F") : QStringLiteral("rgba(255, 242, 196, 205)");
    const QString checkedFg = dark ? QStringLiteral("#101318") : QStringLiteral("#1E160D");
    const QString border = dark ? QStringLiteral("rgba(214, 168, 79, 190)") : QStringLiteral("rgba(181, 144, 77, 180)");
    const QString edge = dark ? QStringLiteral("rgba(255, 242, 198, 150)") : QStringLiteral("rgba(255, 255, 255, 220)");
    const QString disabled = dark ? QStringLiteral("#6F6A5F") : QStringLiteral("#8F8578");
    return QStringLiteral(
        "QCheckBox {"
        "  color: %1;"
        "  background: qlineargradient(x1:0, y1:0, x2:1, y2:1, stop:0 %7, stop:0.25 %2, stop:1 rgba(0,0,0,18));"
        "  border: 1px solid %5;"
        "  border-top-color: %8;"
        "  border-left-color: %8;"
        "  border-radius: 10px;"
        "  padding: 9px 20px;"
        "  font-size: 12pt;"
        "  font-weight: 900;"
        "}"
        "QCheckBox:hover { background-color: %7; border: 1px solid %8; }"
        "QCheckBox:checked { background-color: %3; color: %4; border: 2px solid %5; }"
        "QCheckBox:disabled { color: %6; border-color: %6; background-color: rgba(120, 120, 120, 45); }"
        "QCheckBox::indicator { width: 0px; height: 0px; margin: 0px; }"
    ).arg(fg, bg, checkedBg, checkedFg, border, disabled, bgHover, edge);
}

QString gpuDeviceListStyleSheet()
{
    const QString theme = GUIUtil::currentTheme();
    const bool dark = GUIUtil::isDarkTheme(theme);
    const QString fg = dark ? QStringLiteral("#F8EBD0") : QStringLiteral("#17110A");
    const QString selected = dark ? QStringLiteral("rgba(214, 168, 79, 105)") : QStringLiteral("rgba(255, 218, 121, 128)");
    return GUIUtil::glassListStyleSheet(theme) + QStringLiteral(
        "QListWidget {"
        "  color: %1;"
        "  font-size: 11pt;"
        "  font-weight: 800;"
        "}"
        "QListWidget::item { background-color: transparent; padding: 6px 8px; min-height: 26px; }"
        "QListWidget::item:selected { background-color: %2; color: %1; border-radius: 7px; }"
        "QListWidget::indicator { width: 18px; height: 18px; }"
    ).arg(fg, selected);
}
}
