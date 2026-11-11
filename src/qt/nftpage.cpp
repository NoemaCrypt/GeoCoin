#include "nftpage_internal.h"
#include "geocoin/nft/geodesy.hpp"

NFTPage::NFTPage(QWidget *parent) :
    QWidget(parent),
    clientModel(nullptr),
    walletModel(nullptr),
    transactionCreatedConnection_(),
    labelWalletAddress(nullptr),
    buttonRefresh(nullptr),
    buttonCreate(nullptr),
    buttonTransfer(nullptr),
    buttonBurn(nullptr),
    tableNfts(nullptr),
    labelNftId(nullptr),
    labelOwner(nullptr),
    labelName(nullptr),
    textDescription(nullptr),
    labelImageUri(nullptr),
    labelContentHash(nullptr)
{
    setupUi();
}

NFTPage::~NFTPage() = default;

void NFTPage::transferNftClicked() {
    if (!walletModel || !walletModel->isWalletLoaded()) {
        lightWarning(this, tr("NFT / Cadastr"), tr("Please load or create a wallet first."));
        return;
    }
    auto chainState = walletModel->getChainState();
    if (!chainState) {
        lightWarning(this, tr("NFT / Cadastr"), tr("Chain state is not available."));
        return;
    }
    QString nftIdHex = labelNftId ? labelNftId->text().trimmed() : QString();
    if (nftIdHex.isEmpty() || nftIdHex == tr("—")) {
        lightWarning(this, tr("NFT / Cadastr"), tr("Select an NFT first."));
        return;
    }

    geocoin::Hash256 nft_id{};
    if (!hexToHash(nftIdHex, nft_id)) {
        lightWarning(this, tr("NFT / Cadastr"), tr("Invalid NFT ID."));
        return;
    }
    const auto meta = chainState->get_nft_metadata(nft_id);
    if (meta.cadastral.has_value()) {
        if (!lightConfirm(this,
                          tr("Transfer NFT / Cadastr"),
                          tr("This is a cadastral NFT. Transfer will automatically annul it (burn). Continue?"))) {
            return;
        }
    }

    QString to;
    if (!lightTextPrompt(this, tr("Transfer NFT / Cadastr"), tr("To address:"), to)) {
        return;
    }
    to = to.trimmed();
    if (to.isEmpty()) {
        return;
    }

    QString feeGc;
    if (!lightTextPrompt(
            this,
            tr("Transfer NFT / Cadastr"),
            tr("Fee (GC):"),
            feeGc,
            GUIUtil::formatAmount(walletModel->nftRelayFee()))) {
        return;
    }
    qint64 feeAngles = 0;
    if (!GUIUtil::parseAmount(feeGc, feeAngles) ||
        feeAngles < walletModel->nftRelayFee()) {
        lightWarning(
            this,
            tr("NFT / Cadastr"),
            tr("Fee must be at least %1 GC.").arg(GUIUtil::formatAmount(
                walletModel->nftRelayFee()))
        );
        return;
    }

    QString password = promptPasswordIfNeeded(this, walletModel);
    GUIUtil::ScopedQStringCleanser passwordCleanser(password);
    if (walletModel->isWalletEncrypted() && password.isEmpty()) {
        return;
    }

    auto status = walletModel->transferNft(nftIdHex, to, feeAngles, password);
    GUIUtil::cleanseQString(password);
    if (status == WalletModel::OK) {
        lightInfo(this, tr("NFT / Cadastr"), tr("NFT transfer transaction sent."));
        refresh();
    } else {
        lightWarning(this, tr("NFT / Cadastr"), statusToMessage(status));
    }
}

void NFTPage::burnNftClicked() {
    if (!walletModel || !walletModel->isWalletLoaded()) {
        lightWarning(this, tr("NFT / Cadastr"), tr("Please load or create a wallet first."));
        return;
    }
    QString nftIdHex = labelNftId ? labelNftId->text().trimmed() : QString();
    if (nftIdHex.isEmpty() || nftIdHex == tr("—")) {
        lightWarning(this, tr("NFT / Cadastr"), tr("Select an NFT first."));
        return;
    }

    QString feeGc;
    if (!lightTextPrompt(
            this,
            tr("Burn NFT / Cadastr"),
            tr("Fee (GC):"),
            feeGc,
            GUIUtil::formatAmount(walletModel->nftRelayFee()))) {
        return;
    }
    qint64 feeAngles = 0;
    if (!GUIUtil::parseAmount(feeGc, feeAngles) ||
        feeAngles < walletModel->nftRelayFee()) {
        lightWarning(
            this,
            tr("NFT / Cadastr"),
            tr("Fee must be at least %1 GC.").arg(GUIUtil::formatAmount(
                walletModel->nftRelayFee()))
        );
        return;
    }

    if (!lightConfirm(this, tr("Burn NFT / Cadastr"), tr("Are you sure you want to burn this NFT?"))) {
        return;
    }

    QString password = promptPasswordIfNeeded(this, walletModel);
    GUIUtil::ScopedQStringCleanser passwordCleanser(password);
    if (walletModel->isWalletEncrypted() && password.isEmpty()) {
        return;
    }

    auto status = walletModel->burnNft(nftIdHex, feeAngles, password);
    GUIUtil::cleanseQString(password);
    if (status == WalletModel::OK) {
        lightInfo(this, tr("NFT / Cadastr"), tr("NFT burn transaction sent."));
        refresh();
    } else {
        lightWarning(this, tr("NFT / Cadastr"), statusToMessage(status));
    }
}

void NFTPage::setClientModel(ClientModel *model) {
    this->clientModel = model;
}

void NFTPage::setWalletModel(WalletModel *model) {
    if (walletModel == model) {
        return;
    }
    if (transactionCreatedConnection_) {
        disconnect(transactionCreatedConnection_);
        transactionCreatedConnection_ = QMetaObject::Connection();
    }
    if (walletStateConnection_) {
        disconnect(walletStateConnection_);
        walletStateConnection_ = QMetaObject::Connection();
    }

    this->walletModel = model;

    if (walletModel) {
        transactionCreatedConnection_ = connect(walletModel, &WalletModel::transactionCreated, this, [this](const QString& txid) {
            lastCreatedTxid_ = txid;
        });
        walletStateConnection_ = connect(walletModel, &WalletModel::walletStateChanged,
                                         this, &NFTPage::refresh);
    }

    refresh();
}

void NFTPage::setupUi() {
    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->setSpacing(20);

    QLabel *titleLabel = new QLabel(tr("NFT / Cadastr"), this);
    QFont titleFont = titleLabel->font();
    titleFont.setPointSize(16);
    titleFont.setBold(true);
    titleLabel->setFont(titleFont);
    layout->addWidget(titleLabel);

    QGroupBox *walletGroup = new QGroupBox(tr("Wallet"), this);
    QVBoxLayout *walletLayout = new QVBoxLayout(walletGroup);

    QHBoxLayout *addrLayout = new QHBoxLayout();
    addrLayout->addWidget(new QLabel(tr("Address:"), this));
    labelWalletAddress = new QLabel(tr("No wallet loaded"), this);
    labelWalletAddress->setFont(GUIUtil::geocoinAddressFont());
    addrLayout->addWidget(labelWalletAddress);
    addrLayout->addStretch();
    walletLayout->addLayout(addrLayout);

    buttonRefresh = new QPushButton(tr("Refresh"), this);
    buttonRefresh->setMinimumHeight(32);
    connect(buttonRefresh, &QPushButton::clicked, this, &NFTPage::refresh);
    walletLayout->addWidget(buttonRefresh);

    QHBoxLayout *actionsLayout = new QHBoxLayout();
    buttonCreate = new QPushButton(tr("Create NFT / Cadastr"), this);
    buttonTransfer = new QPushButton(tr("Transfer"), this);
    buttonBurn = new QPushButton(tr("Burn"), this);
    buttonCreate->setMinimumHeight(32);
    buttonTransfer->setMinimumHeight(32);
    buttonBurn->setMinimumHeight(32);
    connect(buttonCreate, &QPushButton::clicked, this, &NFTPage::createNftClicked);
    connect(buttonTransfer, &QPushButton::clicked, this, &NFTPage::transferNftClicked);
    connect(buttonBurn, &QPushButton::clicked, this, &NFTPage::burnNftClicked);
    actionsLayout->addWidget(buttonCreate);
    actionsLayout->addWidget(buttonTransfer);
    actionsLayout->addWidget(buttonBurn);
    actionsLayout->addStretch();
    walletLayout->addLayout(actionsLayout);

    layout->addWidget(walletGroup);

    QGroupBox *listGroup = new QGroupBox(tr("My NFTs"), this);
    QVBoxLayout *listLayout = new QVBoxLayout(listGroup);

    tableNfts = new QTableWidget(this);
    tableNfts->setColumnCount(2);
    tableNfts->setHorizontalHeaderLabels({tr("NFT ID"), tr("Name")});
    tableNfts->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    tableNfts->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    tableNfts->setSelectionBehavior(QAbstractItemView::SelectRows);
    tableNfts->setSelectionMode(QAbstractItemView::SingleSelection);
    tableNfts->setEditTriggers(QAbstractItemView::NoEditTriggers);
    connect(tableNfts, &QTableWidget::itemSelectionChanged, this, &NFTPage::handleSelectionChanged);

    listLayout->addWidget(tableNfts);
    layout->addWidget(listGroup);

    QGroupBox *detailsGroup = new QGroupBox(tr("Details"), this);
    QVBoxLayout *detailsLayout = new QVBoxLayout(detailsGroup);

    auto addRow = [&](const QString& label, QWidget* valueWidget) {
        QHBoxLayout *row = new QHBoxLayout();
        row->addWidget(new QLabel(label, this));
        row->addWidget(valueWidget, 1);
        detailsLayout->addLayout(row);
    };

    labelNftId = new QLabel(tr("—"), this);
    labelNftId->setFont(GUIUtil::geocoinAddressFont());
    addRow(tr("NFT ID:"), labelNftId);

    labelOwner = new QLabel(tr("—"), this);
    labelOwner->setFont(GUIUtil::geocoinAddressFont());
    addRow(tr("Owner:"), labelOwner);

    labelName = new QLabel(tr("—"), this);
    addRow(tr("Name:"), labelName);

    labelImageUri = new QLabel(tr("—"), this);
    labelImageUri->setTextInteractionFlags(Qt::TextSelectableByMouse);
    addRow(tr("Image URI:"), labelImageUri);

    labelContentHash = new QLabel(tr("—"), this);
    labelContentHash->setTextInteractionFlags(Qt::TextSelectableByMouse);
    labelContentHash->setWordWrap(true);
    addRow(tr("Content hash:"), labelContentHash);

    textDescription = new QTextEdit(this);
    textDescription->setReadOnly(true);
    textDescription->setMinimumHeight(80);
    detailsLayout->addWidget(new QLabel(tr("Description:"), this));
    detailsLayout->addWidget(textDescription);

    layout->addWidget(detailsGroup);

    clearDetails();
    applyTheme();
}

void NFTPage::applyTheme()
{
    const QString theme = GUIUtil::currentTheme();
    setStyleSheet(QStringLiteral("NFTPage { background-color: transparent; }")
                  + GUIUtil::glassPanelStyleSheet(theme)
                  + GUIUtil::glassInputStyleSheet(theme)
                  + GUIUtil::glassListStyleSheet(theme));
}

void NFTPage::clearDetails() {
    if (labelNftId) labelNftId->setText(tr("—"));
    if (labelOwner) labelOwner->setText(tr("—"));
    if (labelName) labelName->setText(tr("—"));
    if (labelImageUri) labelImageUri->setText(tr("—"));
    if (labelContentHash) labelContentHash->setText(tr("—"));
    if (textDescription) textDescription->setPlainText(QString());
}

void NFTPage::refresh() {
    clearDetails();

    rowNftIds_.clear();
    pendingById_.clear();

    if (!walletModel) {
        labelWalletAddress->setText(tr("No wallet loaded"));
        tableNfts->setRowCount(0);
        return;
    }

    const QString addrStr = walletModel->getAddress();
    if (addrStr.isEmpty()) {
        labelWalletAddress->setText(tr("No wallet loaded"));
        tableNfts->setRowCount(0);
        return;
    }
    labelWalletAddress->setText(addrStr);

    auto chainState = walletModel->getChainState();
    if (!chainState) {
        tableNfts->setRowCount(0);
        return;
    }

    geocoin::Address owner(addrStr.toStdString());
    const auto ids = chainState->get_owned_nfts(owner);

    QSet<QString> confirmed;
    confirmed.reserve(static_cast<int>(ids.size()));
    for (const auto& id : ids) {
        confirmed.insert(hashToHex(id));
    }

    // Pending NFTs (unconfirmed) from wallet history
    auto wallet = walletModel->getWallet();
    if (wallet) {
        const auto txs = wallet->get_transactions("", 0);
        for (const auto& wtx : txs) {
            if (wtx.block_height != 0) {
                continue;
            }
            if (wtx.tx.type != geocoin::core::TransactionType::NFT_CREATE) {
                continue;
            }

            if (!wtx.tx.nft_create.has_value()) {
                continue;
            }

            const QString nftIdHex = hashToHex(wtx.tx.nft_create->nft_id);
            if (confirmed.contains(nftIdHex)) {
                continue;
            }

            PendingNft p;
            p.nftIdHex = nftIdHex;
            p.name = QString::fromStdString(wtx.tx.nft_create->metadata.name);
            p.description = QString::fromStdString(wtx.tx.nft_create->metadata.description);
            p.imageUri = QString::fromStdString(wtx.tx.nft_create->metadata.image_uri);
            p.contentHash = QString::fromStdString(wtx.tx.nft_create->metadata.content_hash);
            pendingById_.insert(nftIdHex, p);
        }
    }

    const int pendingCount = pendingById_.size();
    const int confirmedCount = static_cast<int>(ids.size());
    tableNfts->setRowCount(pendingCount + confirmedCount);

    int row = 0;

    // Pending first
    for (auto it = pendingById_.cbegin(); it != pendingById_.cend(); ++it) {
        const PendingNft& p = it.value();
        rowNftIds_.push_back(p.nftIdHex);
        QTableWidgetItem* idItem = new QTableWidgetItem(p.nftIdHex);
        QTableWidgetItem* nameItem = new QTableWidgetItem(p.name + tr(" (PENDING)"));
        tableNfts->setItem(row, 0, idItem);
        tableNfts->setItem(row, 1, nameItem);
        ++row;
    }

    // Confirmed
    for (int i = 0; i < static_cast<int>(ids.size()); ++i) {
        const auto& id = ids[i];
        const auto meta = chainState->get_nft_metadata(id);

        const QString idHex = hashToHex(id);
        rowNftIds_.push_back(idHex);

        QTableWidgetItem* idItem = new QTableWidgetItem(idHex);
        QTableWidgetItem* nameItem = new QTableWidgetItem(QString::fromStdString(meta.name));
        tableNfts->setItem(row, 0, idItem);
        tableNfts->setItem(row, 1, nameItem);
        ++row;
    }

    // Auto-generate cadastral certificates after confirmation
    if (walletModel) {
        const QDir base(walletModel->getDataDirectory());
        QSettings settings;
        QString templatePath = settings.value(QStringLiteral("cadastre/template_path")).toString();
        if (!templatePath.isEmpty() && QUrl(templatePath).isLocalFile()) {
            templatePath = QUrl(templatePath).toLocalFile();
        }
        if (QFileInfo::exists(templatePath)) {
            auto wallet = walletModel->getWallet();
            if (wallet) {
                int createdCount = 0;
                const auto txs = wallet->get_transactions("", 0);
                for (const auto& wtx : txs) {
                    if (wtx.block_height == 0) {
                        continue;
                    }
                    if (wtx.tx.type != geocoin::core::TransactionType::NFT_CREATE || !wtx.tx.nft_create.has_value()) {
                        continue;
                    }
                    const auto& meta = wtx.tx.nft_create->metadata;
                    if (!meta.cadastral.has_value()) {
                        continue;
                    }
                    const QString txid = hashToHex(wtx.tx_hash);
                    const QString outFolder = base.filePath(QStringLiteral("cadastre_certificates"));
                    const QString outName = QStringLiteral("cadastre_cert_") + txid.left(12) + QStringLiteral(".png");
                    const QString outPath = QDir(outFolder).filePath(outName);
                    if (QFileInfo::exists(outPath)) {
                        continue;
                    }

                    geocoin::core::Block block;
                    if (!chainState->get_block_by_height(wtx.block_height, block)) {
                        continue;
                    }

                    const QString issueDate = QDateTime::fromSecsSinceEpoch(
                        static_cast<qint64>(block.header.timestamp), Qt::UTC
                    ).toString(QStringLiteral("yyyy-MM-dd HH:mm:ss 'UTC'"));

                    const geocoin::Address ownerAddr = chainState->get_nft_owner(wtx.tx.nft_create->nft_id);
                    const QString ownerStr = QString::fromStdString(ownerAddr.str());

                    const auto& c = meta.cadastral.value();
                    QString buildingInfo = c.has_buildings ? tr("Yes") : tr("No");
                    if (c.has_buildings && c.building_count > 0) {
                        buildingInfo += QStringLiteral(" (") + QString::number(c.building_count) + QStringLiteral(")");
                    }

                    QStringList coordinateParts;
                    for (size_t part = 0; part < meta.geometry.polygons.size(); ++part) {
                        const auto& polygon = meta.geometry.polygons[part];
                        coordinateParts << tr("Part %1: %2")
                            .arg(part + 1).arg(formatPolygonCompact(polygon.exterior, 9));
                        for (size_t hole = 0; hole < polygon.holes.size(); ++hole) {
                            coordinateParts << tr("Hole %1: %2")
                                .arg(hole + 1).arg(formatPolygonCompact(polygon.holes[hole], 9));
                        }
                    }
                    const QString coords = coordinateParts.join(QStringLiteral("; "));
                    const auto metrics = geocoin::nft::coordinates::measure_geometry(meta.geometry);
                    const QString areaWgs = metrics ?
                        QString::number(metrics->area_sqm(), 'f', 2) + QStringLiteral(" sqm") : tr("Unavailable");

                    QString ownershipHash = isNullHash(c.ownership_document_hash) ? QString() : hashToHex(c.ownership_document_hash);
                    QString techHash = isNullHash(c.technical_plan_hash) ? QString() : hashToHex(c.technical_plan_hash);
                    QString photoHash;
                    if (!c.photo_hashes.empty()) {
                        photoHash = hashToHex(c.photo_hashes.front());
                    }

                    QMap<QString, QString> fields;
                    fields.insert(QStringLiteral("issue_date"), issueDate);
                    fields.insert(QStringLiteral("issuer"), QStringLiteral("GeoCoin Wallet"));
                    fields.insert(QStringLiteral("cadastral_center"), QString::fromStdString(meta.location_name));
                    fields.insert(QStringLiteral("land_registry"), QString::fromStdString(c.land_category));
                    fields.insert(QStringLiteral("parcel_id"), QString::fromStdString(c.cadastral_number));
                    fields.insert(QStringLiteral("area"), QString::number(c.area_sqcm_official / 10000.0, 'f', 4) + QStringLiteral(" sqm"));
                    fields.insert(QStringLiteral("location_name"), QString::fromStdString(meta.location_name));
                    fields.insert(QStringLiteral("owner_address"), ownerStr);
                    const QString ownerName = QString::fromStdString(c.owner_name);
                    fields.insert(QStringLiteral("owner_name"), ownerName.isEmpty() ? ownerStr : ownerName);
                    fields.insert(QStringLiteral("building_info"), buildingInfo);
                    fields.insert(QStringLiteral("coordinates"), coords);
                    fields.insert(QStringLiteral("area_wgs84"), areaWgs);
                    fields.insert(QStringLiteral("map_data"), meta.geometry.polygons.size() > 1
                        ? QStringLiteral("MultiPolygon") : QStringLiteral("Polygon"));
                    if (!ownershipHash.isEmpty()) {
                        fields.insert(QStringLiteral("doc_hash"), ownershipHash);
                    }
                    if (!techHash.isEmpty()) {
                        fields.insert(QStringLiteral("metadata_hash"), techHash);
                    }
                    fields.insert(QStringLiteral("photo_hash"), photoHash);
                    fields.insert(QStringLiteral("block_id"), QString::number(wtx.block_height));
                    const QString geoTagStr = QString::number(static_cast<qulonglong>(block.header.geo_tag));
                    fields.insert(QStringLiteral("blockchain_por"), hashToHex(block.get_hash()) + QStringLiteral(" | geo_tag=") + geoTagStr);
                    fields.insert(QStringLiteral("network"), QStringLiteral("MAINNET"));
                    fields.insert(QStringLiteral("timestamp"), issueDate);
                    fields.insert(QStringLiteral("doc_hash_proof"), ownershipHash);

                    const QString cadQr = QStringLiteral("geocoin:cadastre?txid=") + txid;
                    if (createCadastralCertificate(templatePath, outPath, fields, cadQr)) {
                        createdCount++;
                    }
                }
                if (createdCount > 0) {
                    lightInfo(this, tr("NFT"), tr("Cadastral certificate created."));
                }
            }
        } else {
            bool hasConfirmedCadastral = false;
            auto wallet = walletModel->getWallet();
            if (wallet) {
                const auto txs = wallet->get_transactions("", 0);
                for (const auto& wtx : txs) {
                    if (wtx.block_height == 0) {
                        continue;
                    }
                    if (wtx.tx.type != geocoin::core::TransactionType::NFT_CREATE || !wtx.tx.nft_create.has_value()) {
                        continue;
                    }
                    if (wtx.tx.nft_create->metadata.cadastral.has_value()) {
                        hasConfirmedCadastral = true;
                        break;
                    }
                }
            }
            static bool warned = false;
            if (hasConfirmedCadastral && !warned) {
                warned = true;
                lightWarning(this, tr("NFT"), tr("No certificate template selected. Use Upload template in Create NFT dialog."));
            }
        }
    }
}

void NFTPage::handleSelectionChanged() {
    clearDetails();

    if (!walletModel) {
        return;
    }
    auto chainState = walletModel->getChainState();
    if (!chainState) {
        return;
    }

    const auto selected = tableNfts->selectedItems();
    if (selected.isEmpty()) {
        return;
    }

    QTableWidgetItem* idItem = tableNfts->item(selected[0]->row(), 0);
    if (!idItem) {
        return;
    }

    const QString nftIdHex = idItem->text().trimmed();
    labelNftId->setText(nftIdHex);

    // Pending NFTs are not in ChainState yet, so display cached metadata.
    auto pendingIt = pendingById_.find(nftIdHex);
    if (pendingIt != pendingById_.end()) {
        const PendingNft& p = pendingIt.value();
        labelOwner->setText(tr("PENDING"));
        labelName->setText(p.name);
        labelImageUri->setText(p.imageUri);
        if (labelContentHash) labelContentHash->setText(p.contentHash);
        textDescription->setPlainText(p.description);
        return;
    }

    geocoin::Hash256 nft_id{};
    if (!hexToHash(nftIdHex, nft_id)) {
        return;
    }

    const auto owner = chainState->get_nft_owner(nft_id);
    const auto meta = chainState->get_nft_metadata(nft_id);

    labelNftId->setText(hashToHex(nft_id));
    labelOwner->setText(QString::fromStdString(owner.str()));
    labelName->setText(QString::fromStdString(meta.name));
    labelImageUri->setText(QString::fromStdString(meta.image_uri));
    if (labelContentHash) labelContentHash->setText(QString::fromStdString(meta.content_hash));
    textDescription->setPlainText(QString::fromStdString(meta.description));
}
