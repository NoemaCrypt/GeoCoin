#include "nftpage_internal.h"
#include "geocoin/nft/geometry_codec.hpp"
#include <QGraphicsView>
#include <QGraphicsScene>
#include <QPainterPath>
#include <QResizeEvent>
#include <QTimer>
#include "geocoin/nft/geodesy.hpp"

class GeometryPreviewView final : public QGraphicsView {
public:
    explicit GeometryPreviewView(QWidget* parent) : QGraphicsView(parent) {
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setRenderHint(QPainter::Antialiasing);
    }
protected:
    void resizeEvent(QResizeEvent* event) override {
        QGraphicsView::resizeEvent(event);
        if (scene() && !scene()->items().empty()) fitInView(scene()->sceneRect(), Qt::KeepAspectRatio);
    }
};

class CreateNftDialog final : public QDialog {
public:
    explicit CreateNftDialog(QWidget* parent, WalletModel* walletModel)
        : QDialog(parent)
        , walletModel_(walletModel)
        , nameEdit_(new QLineEdit(this))
        , descriptionEdit_(new QTextEdit(this))
        , imageUriEdit_(new QLineEdit(this))
        , browseImageButton_(new QPushButton(tr("Browse..."), this))
        , certificateTemplateEdit_(new QLineEdit(this))
        , uploadCertificateButton_(new QPushButton(tr("Upload template..."), this))
        , locationNameEdit_(new QLineEdit(this))
        , tagsEdit_(new QLineEdit(this))
        , cadastralNumberEdit_(new QLineEdit(this))
        , ownerNameEdit_(new QLineEdit(this))
        , landCategoryEdit_(new QLineEdit(this))
        , permittedUseEdit_(new QLineEdit(this))
        , areaOfficialEdit_(new QDoubleSpinBox(this))
        , ownershipHashEdit_(new QLineEdit(this))
        , technicalPlanHashEdit_(new QLineEdit(this))
        , photoHashesEdit_(new QLineEdit(this))
        , docsArchiveEdit_(new QLineEdit(this))
        , uploadDocsArchiveButton_(new QPushButton(tr("Upload documents (zip)..."), this))
        , marketValueEdit_(new QLineEdit(this))
        , cadastralValueEdit_(new QLineEdit(this))
        , valuationDateEdit_(new QDateTimeEdit(this))
        , restrictionsEdit_(new QLineEdit(this))
        , hasBuildingsCheck_(new QCheckBox(tr("Has buildings"), this))
        , buildingCountEdit_(new QSpinBox(this))
        , centerLatDeg_(new QDoubleSpinBox(this))
        , centerLonDeg_(new QDoubleSpinBox(this))
        , sizeMeters_(new QDoubleSpinBox(this))
        , autoFillSquareButton_(new QPushButton(tr("Auto-fill square"), this))
        , polygonEdit_(new QTextEdit(this))
        , feeGc_(new QLineEdit(this))
        , royaltyPercent_(new QSpinBox(this))
        , submitted_ok_(false)
    {
        setWindowTitle(tr("Create NFT / Cadastr"));
        setModal(true);

        setAutoFillBackground(true);
        setStyleSheet(lightDialogStyleSheet());

        auto setupCoordBox = [](QDoubleSpinBox* b, double min, double max) {
            b->setRange(min, max);
            b->setDecimals(9);
            b->setSingleStep(0.000001);
            b->setValue(0.0);
        };

        setupCoordBox(centerLatDeg_, -90.0, 90.0);
        setupCoordBox(centerLonDeg_, -180.0, 180.0);

        sizeMeters_->setDecimals(2);
        sizeMeters_->setRange(1.0, 1000000.0);
        sizeMeters_->setSingleStep(10.0);
        sizeMeters_->setValue(100.0);

        const QString minimumFee = GUIUtil::formatAmount(walletModel_->nftRelayFee());
        feeGc_->setText(minimumFee);
        feeGc_->setPlaceholderText(minimumFee);

        royaltyPercent_->setRange(0, 100);
        royaltyPercent_->setValue(0);

        descriptionEdit_->setMinimumHeight(80);
        polygonEdit_->setMinimumHeight(80);
        polygonEdit_->setPlaceholderText(tr("One point per line: lat,lon (degrees)\nExample:\n53.900000,27.566667\n53.901000,27.566667\n53.901000,27.568000\n53.900000,27.568000"));

        areaOfficialEdit_->setDecimals(4);
        areaOfficialEdit_->setRange(0.0, 1e12);
        areaOfficialEdit_->setSingleStep(10.0);
        areaOfficialEdit_->setValue(0.0);

        marketValueEdit_->setText(QStringLiteral("0.00000000"));
        marketValueEdit_->setPlaceholderText(QStringLiteral("0.00000000"));
        cadastralValueEdit_->setText(QStringLiteral("0.00000000"));
        cadastralValueEdit_->setPlaceholderText(QStringLiteral("0.00000000"));

        valuationDateEdit_->setDisplayFormat("yyyy-MM-dd");
        valuationDateEdit_->setCalendarPopup(true);
        valuationDateEdit_->setDate(QDate::currentDate());
        valuationDateEdit_->setLocale(QLocale());
        if (valuationDateEdit_->calendarWidget()) {
            valuationDateEdit_->calendarWidget()->setLocale(QLocale());
            valuationDateEdit_->calendarWidget()->setStyleSheet(lightDialogStyleSheet());
        }

        buildingCountEdit_->setRange(0, 1000000);
        buildingCountEdit_->setValue(0);

        QFormLayout* form = new QFormLayout();
        form->addRow(tr("Name *"), nameEdit_);
        form->addRow(tr("Description"), descriptionEdit_);

        {
            QWidget* row = new QWidget(this);
            QHBoxLayout* rowLayout = new QHBoxLayout(row);
            rowLayout->setContentsMargins(0, 0, 0, 0);
            rowLayout->addWidget(imageUriEdit_);
            rowLayout->addWidget(browseImageButton_);
            row->setLayout(rowLayout);
            form->addRow(tr("Image URI"), row);
            connect(browseImageButton_, &QPushButton::clicked, this, [this]() {
                onBrowseImage();
            });
        }

        {
            QWidget* row = new QWidget(this);
            QHBoxLayout* rowLayout = new QHBoxLayout(row);
            rowLayout->setContentsMargins(0, 0, 0, 0);
            rowLayout->addWidget(certificateTemplateEdit_);
            rowLayout->addWidget(uploadCertificateButton_);
            row->setLayout(rowLayout);
            form->addRow(tr("Certificate template *"), row);
            connect(uploadCertificateButton_, &QPushButton::clicked, this, [this]() {
                onUploadCertificateTemplate();
            });
        }

        form->addRow(tr("Location name *"), locationNameEdit_);
        form->addRow(tr("Tags (comma-separated)"), tagsEdit_);
        form->addRow(tr("Cadastral number *"), cadastralNumberEdit_);
        form->addRow(tr("Owner name *"), ownerNameEdit_);
        form->addRow(tr("Land category *"), landCategoryEdit_);
        form->addRow(tr("Permitted use *"), permittedUseEdit_);
        form->addRow(tr("Official area (sqm) *"), areaOfficialEdit_);
        form->addRow(tr("Ownership doc hash (hex) *"), ownershipHashEdit_);
        form->addRow(tr("Technical plan hash (hex)"), technicalPlanHashEdit_);
        form->addRow(tr("Photo hashes (comma-separated hex)"), photoHashesEdit_);

        {
            QWidget* row = new QWidget(this);
            QHBoxLayout* rowLayout = new QHBoxLayout(row);
            rowLayout->setContentsMargins(0, 0, 0, 0);
            rowLayout->addWidget(docsArchiveEdit_);
            rowLayout->addWidget(uploadDocsArchiveButton_);
            row->setLayout(rowLayout);
            form->addRow(tr("Documents archive (zip)"), row);
            connect(uploadDocsArchiveButton_, &QPushButton::clicked, this, [this]() {
                onUploadDocsArchive();
            });
        }
        form->addRow(tr("Market value (GC)"), marketValueEdit_);
        form->addRow(tr("Cadastral value (GC)"), cadastralValueEdit_);
        form->addRow(tr("Last valuation date"), valuationDateEdit_);
        form->addRow(tr("Restrictions (comma-separated)"), restrictionsEdit_);
        form->addRow(QString(), hasBuildingsCheck_);
        form->addRow(tr("Building count"), buildingCountEdit_);

        {
            QWidget* row = new QWidget(this);
            QHBoxLayout* rowLayout = new QHBoxLayout(row);
            rowLayout->setContentsMargins(0, 0, 0, 0);
            rowLayout->addWidget(centerLatDeg_);
            rowLayout->addWidget(centerLonDeg_);
            rowLayout->addWidget(sizeMeters_);
            rowLayout->addWidget(autoFillSquareButton_);
            row->setLayout(rowLayout);

            centerLatDeg_->setToolTip(tr("Center latitude (degrees)"));
            centerLonDeg_->setToolTip(tr("Center longitude (degrees)"));
            sizeMeters_->setToolTip(tr("Approximate size in meters (used as half-side for square)"));

            form->addRow(tr("Center lat/lon + size (m)"), row);
            connect(autoFillSquareButton_, &QPushButton::clicked, this, [this]() {
                onAutoFillSquare();
            });
        }

        form->addRow(tr("Geometry: lat,lon lines or GeoJSON"), polygonEdit_);
        geometryStatus_ = new QLabel(this);
        geometryStatus_->setWordWrap(true);
        geometryPreview_ = new GeometryPreviewView(this);
        geometryPreview_->setMinimumHeight(170);
        geometryScene_ = new QGraphicsScene(geometryPreview_);
        geometryPreview_->setScene(geometryScene_);
        form->addRow(tr("Geometry check"), geometryStatus_);
        form->addRow(tr("Outline preview"), geometryPreview_);
        auto* geometryTimer = new QTimer(this);
        geometryTimer->setSingleShot(true); geometryTimer->setInterval(200);
        connect(polygonEdit_, &QTextEdit::textChanged, this, [geometryTimer] { geometryTimer->start(); });
        connect(geometryTimer, &QTimer::timeout, this, [this] { updateGeometryPreview(); });
        form->addRow(tr("Fee (GC)"), feeGc_);
        form->addRow(tr("Royalty percent"), royaltyPercent_);

        QDialogButtonBox* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
        connect(buttons, &QDialogButtonBox::accepted, this, &CreateNftDialog::onSubmit);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

        QWidget* formWidget = new QWidget(this);
        formWidget->setLayout(form);

        QScrollArea* scroll = new QScrollArea(this);
        scroll->setWidget(formWidget);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);

        QVBoxLayout* root = new QVBoxLayout(this);
        root->addWidget(scroll);
        root->addWidget(buttons);
        setLayout(root);

        setMinimumSize(900, 720);

        // Load last used certificate template (if any)
        QSettings settings;
        const QString lastTemplate = settings.value(QStringLiteral("cadastre/template_path")).toString();
        if (!lastTemplate.isEmpty()) {
            certificateTemplateEdit_->setText(lastTemplate);
        }
    }

    QString name() const { return nameEdit_->text().trimmed(); }
    QString description() const { return descriptionEdit_->toPlainText().trimmed(); }
    QString imageUri() const { return imageUriEdit_->text().trimmed(); }
    QString certificateTemplatePath() const { return certificateTemplateEdit_->text().trimmed(); }
    QString locationName() const { return locationNameEdit_->text().trimmed(); }
    QString tagsCsv() const { return tagsEdit_->text().trimmed(); }
    QString cadastralNumber() const { return cadastralNumberEdit_->text().trimmed(); }
    QString ownerName() const { return ownerNameEdit_->text().trimmed(); }
    QString landCategory() const { return landCategoryEdit_->text().trimmed(); }
    QString permittedUse() const { return permittedUseEdit_->text().trimmed(); }
    double areaOfficial() const { return areaOfficialEdit_->value(); }
    QString ownershipDocHash() const { return ownershipHashEdit_->text().trimmed(); }
    QString technicalPlanHash() const { return technicalPlanHashEdit_->text().trimmed(); }
    QString photoHashesCsv() const { return photoHashesEdit_->text().trimmed(); }
    QString docsArchivePath() const { return docsArchiveEdit_->text().trimmed(); }
    QString marketValue() const { return marketValueEdit_->text().trimmed(); }
    QString cadastralValue() const { return cadastralValueEdit_->text().trimmed(); }
    uint64_t valuationDate() const { return static_cast<uint64_t>(valuationDateEdit_->date().startOfDay().toSecsSinceEpoch()); }
    QString restrictionsCsv() const { return restrictionsEdit_->text().trimmed(); }
    bool hasBuildings() const { return hasBuildingsCheck_->isChecked(); }
    int buildingCount() const { return buildingCountEdit_->value(); }
    bool submittedOk() const { return submitted_ok_; }

    bool polygon(geocoin::nft::NFTMetadata::Geometry& out, QString& error) const {
        try {
            const auto text = polygonEdit_->toPlainText().trimmed();
            if (text.startsWith('{')) {
                const auto value = nlohmann::json::parse(text.toStdString());
                out = value.contains("type") ? geocoin::nft::coordinates::geometry_from_geojson(value) :
                    geocoin::nft::coordinates::geometry_from_json(value);
            } else {
                out = {};
                auto& ring = out.polygons.front().exterior;
                const auto lines = text.split(QRegularExpression(QStringLiteral("[\\r\\n]+")), Qt::SkipEmptyParts);
                for (const auto& line : lines) {
                    const auto values = line.trimmed().split(QRegularExpression(QStringLiteral("[,\\s]+")), Qt::SkipEmptyParts);
                    if (values.size() != 2) throw geocoin::nft::coordinates::GeometryError({
                        "position_format", "Each line must contain latitude,longitude in degrees", 0, 0, ring.size(), {}, {}});
                    bool first = false, second = false;
                    const double lat = values[0].toDouble(&first), lon = values[1].toDouble(&second);
                    if (!first || !second || !std::isfinite(lat) || !std::isfinite(lon) || lat < -90 || lat > 90 || lon < -180 || lon > 180)
                        throw geocoin::nft::coordinates::GeometryError({
                            "coordinate_range", "Invalid WGS84 coordinate", 0, 0, ring.size(), {}, {}});
                    ring.push_back({degToNdeg(lat), degToNdeg(lon)});
                    if (ring.size() > geocoin::nft::coordinates::kMaxGeometryVertices)
                        throw std::invalid_argument("Geometry exceeds 256 vertices");
                }
                if (ring.size() > 1 && ring.front() == ring.back()) ring.pop_back();
                const auto validation = geocoin::nft::coordinates::validate_geometry(out);
                if (!validation) throw geocoin::nft::coordinates::GeometryError(*validation.issue);
                out = geocoin::nft::coordinates::canonicalize_geometry(std::move(out));
            }
            return true;
        } catch (const geocoin::nft::coordinates::GeometryError& issue) {
            const auto& problem = issue.issue;
            QStringList location{tr("part %1").arg(problem.part + 1), tr("ring %1").arg(problem.ring + 1)};
            if (problem.vertex) location << tr("vertex %1").arg(*problem.vertex + 1);
            if (problem.edge) location << tr("edge %1").arg(*problem.edge + 1);
            if (problem.other_edge) location << tr("edge %1").arg(*problem.other_edge + 1);
            error = QString::fromStdString(problem.message) + QStringLiteral(" — ") + location.join(QStringLiteral(", "));
        } catch (const std::exception& issue) { error = QString::fromUtf8(issue.what()); }
        return false;
    }

    void updateGeometryPreview() {
        geometryScene_->clear();
        geocoin::nft::NFTMetadata::Geometry geometry;
        QString error;
        if (!polygon(geometry, error)) { geometryStatus_->setText(error); return; }
        const auto metrics = geocoin::nft::coordinates::measure_geometry(geometry);
        size_t holes = 0;
        for (const auto& part : geometry.polygons) holes += part.holes.size();
        geometryStatus_->setText(tr("%1 parts, %2 holes | WGS84 area %3 m² | perimeter %4 m")
            .arg(geometry.polygons.size()).arg(holes).arg(metrics->area_sqm(), 0, 'f', 4).arg(metrics->perimeter_m, 0, 'f', 3));
        try {
            const auto display = geocoin::nft::coordinates::geometry_to_geojson(geometry, 10);
            const auto parts = display["type"] == "Polygon" ? nlohmann::json::array({display["coordinates"]}) : display["coordinates"];
            const double reference = geometry.polygons.front().exterior.front().lon_ndeg / 1e9;
            QPainterPath path;
            path.setFillRule(Qt::OddEvenFill);
            for (const auto& part : parts) for (const auto& ring : part) {
                bool first = true;
                for (const auto& position : ring) {
                    double lon = position[0].get<double>();
                    while (lon - reference > 180) lon -= 360;
                    while (lon - reference < -180) lon += 360;
                    const QPointF point(lon, -position[1].get<double>());
                    if (first) path.moveTo(point); else path.lineTo(point);
                    first = false;
                }
                path.closeSubpath();
            }
            QPen pen(QColor(40, 125, 190)); pen.setCosmetic(true);
            geometryScene_->addPath(path, pen, QBrush(QColor(40, 125, 190, 90)));
            // A cosmetic pen has pixel width, while item bounds may still
            // expand it in degree units. Fit the actual geometry so small
            // parcels do not collapse into a dot in a one-degree pen margin.
            const auto bounds = path.boundingRect();
            const double margin = std::max({bounds.width(), bounds.height(), 1e-12}) * 0.05;
            const auto extent = bounds.adjusted(-margin, -margin, margin, margin);
            geometryScene_->setSceneRect(extent);
            geometryPreview_->fitInView(extent, Qt::KeepAspectRatio);
        } catch (const std::exception& issue) {
            geometryStatus_->setText(geometryStatus_->text() + " | " + QString::fromUtf8(issue.what()));
        }
    }

    QString feeGc() const { return feeGc_->text().trimmed(); }
    int royaltyPercent() const { return royaltyPercent_->value(); }

private:
    void clearFieldErrors() {
        const QString clear = QString();
        for (QWidget* w : errorFields_) {
            if (w) {
                w->setStyleSheet(clear);
            }
        }
        errorFields_.clear();
    }

    void markFieldError(QWidget* w) {
        if (!w) return;
        w->setStyleSheet(QStringLiteral("border: 2px solid #B00020;"));
        errorFields_.push_back(w);
        w->setFocus();
    }

    void onSubmit() {
        clearFieldErrors();

        if (!walletModel_ || !walletModel_->isWalletLoaded()) {
            lightWarning(this, tr("NFT / Cadastr"), tr("Please load or create a wallet first."));
            return;
        }

        if (name().isEmpty()) {
            markFieldError(nameEdit_);
            lightWarning(this, tr("NFT / Cadastr"), tr("Name is required."));
            return;
        }

        qint64 feeAngles = 0;
        if (!GUIUtil::parseAmount(feeGc(), feeAngles) ||
            feeAngles < walletModel_->nftRelayFee()) {
            markFieldError(feeGc_);
            lightWarning(
                this,
                tr("NFT / Cadastr"),
                tr("Fee must be at least %1 GC.").arg(GUIUtil::formatAmount(
                    walletModel_->nftRelayFee()))
            );
            return;
        }

        geocoin::nft::NFTMetadata::Geometry polygonPoints;
        QString polygonError;
        if (!polygon(polygonPoints, polygonError)) {
            markFieldError(polygonEdit_);
            lightWarning(this, tr("NFT / Cadastr"), polygonError);
            return;
        }

        std::optional<geocoin::nft::NFTMetadata::CadastralData> cadastral;
        if (!cadastralNumber().isEmpty()) {
            if (landCategory().isEmpty() ||
                permittedUse().isEmpty() ||
                areaOfficial() <= 0.0 ||
                locationName().isEmpty() ||
                ownerName().isEmpty()) {
                if (ownerName().isEmpty()) markFieldError(ownerNameEdit_);
                if (landCategory().isEmpty()) markFieldError(landCategoryEdit_);
                if (permittedUse().isEmpty()) markFieldError(permittedUseEdit_);
                if (areaOfficial() <= 0.0) markFieldError(areaOfficialEdit_);
                if (locationName().isEmpty()) markFieldError(locationNameEdit_);
                lightWarning(this, tr("NFT / Cadastr"), tr("All cadastral fields are required."));
                return;
            }

            const QString templatePath = QUrl(certificateTemplatePath()).isLocalFile()
                ? QUrl(certificateTemplatePath()).toLocalFile()
                : certificateTemplatePath();
            if (templatePath.isEmpty() || !QFileInfo::exists(templatePath)) {
                markFieldError(certificateTemplateEdit_);
                lightWarning(this, tr("NFT / Cadastr"), tr("Certificate template is required. Please upload it."));
                return;
            }

            if (ownershipDocHash().isEmpty()) {
                markFieldError(ownershipHashEdit_);
                lightWarning(this, tr("NFT / Cadastr"), tr("Document hash is required. Upload a ZIP or enter it manually."));
                return;
            }

            geocoin::nft::NFTMetadata::CadastralData c;
            c.cadastral_number = cadastralNumber().toStdString();
            c.owner_name = ownerName().toStdString();
            c.land_category = landCategory().toStdString();
            c.permitted_use = permittedUse().toStdString();
            c.area_sqcm_official = static_cast<uint64_t>(std::llround(
                areaOfficial() * geocoin::nft::coordinates::kSquareCentimetresPerSquareMetre));

            if (!ownershipDocHash().isEmpty()) {
                geocoin::Hash256 h{};
                if (!hexToHash(ownershipDocHash(), h)) {
                    markFieldError(ownershipHashEdit_);
                    lightWarning(this, tr("NFT / Cadastr"), tr("Invalid ownership document hash."));
                    return;
                }
                c.ownership_document_hash = h;
            }
            if (!technicalPlanHash().isEmpty()) {
                geocoin::Hash256 h{};
                if (!hexToHash(technicalPlanHash(), h)) {
                    markFieldError(technicalPlanHashEdit_);
                    lightWarning(this, tr("NFT / Cadastr"), tr("Invalid technical plan hash."));
                    return;
                }
                c.technical_plan_hash = h;
            }

            if (!photoHashesCsv().isEmpty()) {
                const QStringList parts = photoHashesCsv().split(",", Qt::SkipEmptyParts);
                for (const QString& p : parts) {
                    geocoin::Hash256 h{};
                    if (!hexToHash(p.trimmed(), h)) {
                        markFieldError(photoHashesEdit_);
                        lightWarning(this, tr("NFT / Cadastr"), tr("Invalid photo hash."));
                        return;
                    }
                    c.photo_hashes.push_back(h);
                }
            }

            qint64 marketValueAngles = 0;
            qint64 cadastralValueAngles = 0;
            if (!GUIUtil::parseAmount(marketValue(), marketValueAngles)) {
                markFieldError(marketValueEdit_);
                lightWarning(this, tr("NFT / Cadastr"), tr("Invalid market value."));
                return;
            }
            if (!GUIUtil::parseAmount(cadastralValue(), cadastralValueAngles)) {
                markFieldError(cadastralValueEdit_);
                lightWarning(this, tr("NFT / Cadastr"), tr("Invalid cadastral value."));
                return;
            }
            c.market_value = marketValueAngles;
            c.cadastral_value = cadastralValueAngles;
            c.last_valuation_date = valuationDate();

            if (!restrictionsCsv().isEmpty()) {
                const QStringList parts = restrictionsCsv().split(",", Qt::SkipEmptyParts);
                for (const QString& p : parts) {
                    const QString r = p.trimmed();
                    if (!r.isEmpty()) {
                        c.restrictions.push_back(r.toStdString());
                    }
                }
            }
            c.has_buildings = hasBuildings();
            c.building_count = static_cast<uint32_t>(buildingCount());

            cadastral = std::move(c);
        }

        QString password = promptPasswordIfNeeded(this, walletModel_);
        GUIUtil::ScopedQStringCleanser passwordCleanser(password);
        if (walletModel_->isWalletEncrypted() && password.isEmpty()) {
            return;
        }

        auto status = walletModel_->createNft(
            name(),
            description(),
            imageUri(),
            locationName(),
            tagsCsv(),
            polygonPoints,
            cadastral,
            feeAngles,
            static_cast<quint32>(royaltyPercent()),
            password
        );
        GUIUtil::cleanseQString(password);

        if (status == WalletModel::OK) {
            submitted_ok_ = true;
            accept();
        } else {
            lightWarning(this, tr("NFT / Cadastr"), statusToMessage(status));
        }
    }
    void onAutoFillSquare() {
        const double lat0 = centerLatDeg_->value();
        const double lon0 = centerLonDeg_->value();
        const double meters = sizeMeters_->value();

        if (meters <= 0.0) {
            return;
        }

        const geocoin::nft::NFTMetadata::GeoPoint center{
            static_cast<int64_t>(std::llround(lat0 * 1e9)),
            static_cast<int64_t>(std::llround(lon0 * 1e9))};
        // Equal geodesic radii give a square in the centre's azimuthal
        // equidistant plane, including at poles and across the dateline.
        const double diagonal = meters * std::sqrt(2.0);
        QStringList lines;
        for (double azimuth : {-45.0, 45.0, 135.0, 225.0}) {
            const auto point = geocoin::nft::coordinates::geodesic_destination(center, azimuth, diagonal);
            if (!point) return;
            lines << QString::number(point->lat_ndeg / 1e9, 'f', 9) + "," +
                QString::number(point->lon_ndeg / 1e9, 'f', 9);
        }
        polygonEdit_->setPlainText(lines.join("\n"));
    }

    void onBrowseImage() {
        QString filter = tr("Images (*.png *.jpg *.jpeg *.webp *.bmp *.gif);;All files (*.*)");
        const QString srcPath = QFileDialog::getOpenFileName(this, tr("Select image"), QString(), filter);
        if (srcPath.isEmpty()) {
            return;
        }

        if (!walletModel_) {
            imageUriEdit_->setText(QUrl::fromLocalFile(srcPath).toString());
            return;
        }

        const QString dataDir = walletModel_->getDataDirectory();
        if (dataDir.isEmpty()) {
            imageUriEdit_->setText(QUrl::fromLocalFile(srcPath).toString());
            return;
        }

        const QDir base(dataDir);
        const QString relFolder = QStringLiteral("nft_images");
        const QString targetFolderPath = base.filePath(relFolder);
        QDir().mkpath(targetFolderPath);

        QFileInfo fi(srcPath);
        QString baseName = fi.completeBaseName();
        QString ext = fi.suffix();
        if (!ext.isEmpty()) {
            ext = QStringLiteral(".") + ext;
        }

        const QString unique = QString::number(QDateTime::currentMSecsSinceEpoch());
        const QString fileName = baseName + QStringLiteral("_") + unique + ext;
        const QString dstPath = QDir(targetFolderPath).filePath(fileName);

        if (!QFile::copy(srcPath, dstPath)) {
            lightWarning(this, tr("Create NFT / Cadastr"), tr("Failed to copy image into data directory."));
            imageUriEdit_->setText(QUrl::fromLocalFile(srcPath).toString());
            return;
        }

        imageUriEdit_->setText(QStringLiteral("local://") + relFolder + QStringLiteral("/") + fileName);
    }

    void onUploadCertificateTemplate() {
        QString filter = tr("Images (*.png *.jpg *.jpeg *.webp *.bmp *.gif);;All files (*.*)");
        const QString srcPath = QFileDialog::getOpenFileName(this, tr("Select certificate template"), QString(), filter);
        if (srcPath.isEmpty()) {
            return;
        }

        if (!walletModel_) {
            certificateTemplateEdit_->setText(QUrl::fromLocalFile(srcPath).toString());
            return;
        }

        const QString dataDir = walletModel_->getDataDirectory();
        if (dataDir.isEmpty()) {
            certificateTemplateEdit_->setText(QUrl::fromLocalFile(srcPath).toString());
            return;
        }

        const QDir base(dataDir);
        const QString targetFolder = base.filePath(QStringLiteral("cadastre_templates"));
        QDir().mkpath(targetFolder);

        QFileInfo fi(srcPath);
        QString baseName = fi.completeBaseName();
        QString ext = fi.suffix().toLower();
        if (ext.isEmpty()) {
            ext = QStringLiteral("png");
        }
        const QString unique = QString::number(QDateTime::currentMSecsSinceEpoch());
        const QString fileName = baseName + QStringLiteral("_") + unique + QStringLiteral(".") + ext;
        const QString targetPath = QDir(targetFolder).filePath(fileName);

        if (!QFile::copy(srcPath, targetPath)) {
                lightWarning(this, tr("Create NFT / Cadastr"), tr("Failed to copy certificate template into data directory."));
            certificateTemplateEdit_->setText(QUrl::fromLocalFile(srcPath).toString());
            return;
        }

        certificateTemplateEdit_->setText(QUrl::fromLocalFile(targetPath).toString());

        QSettings settings;
        settings.setValue(QStringLiteral("cadastre/template_path"), targetPath);
    }

    void onUploadDocsArchive() {
        QString filter = tr("Zip archive (*.zip);;All files (*.*)");
        const QString srcPath = QFileDialog::getOpenFileName(this, tr("Select documents archive"), QString(), filter);
        if (srcPath.isEmpty()) {
            return;
        }

        QFile f(srcPath);
        if (!f.open(QIODevice::ReadOnly)) {
            lightWarning(this, tr("Create NFT / Cadastr"), tr("Failed to open documents archive."));
            return;
        }

        QCryptographicHash hasher(QCryptographicHash::Sha256);
        const qint64 kChunk = 1 << 20;
        while (!f.atEnd()) {
            const QByteArray chunk = f.read(kChunk);
            if (chunk.isEmpty() && !f.atEnd()) {
                lightWarning(this, tr("Create NFT / Cadastr"), tr("Failed to read documents archive."));
                return;
            }
            hasher.addData(chunk);
        }
        const QString hex = hasher.result().toHex();
        if (hex.isEmpty()) {
            lightWarning(this, tr("Create NFT / Cadastr"), tr("Failed to compute archive hash."));
            return;
        }

        docsArchiveEdit_->setText(QUrl::fromLocalFile(srcPath).toString());
        ownershipHashEdit_->setText(hex);
    }

    static int64_t degToNdeg(double deg) { return static_cast<int64_t>(std::llround(deg * 1e9)); }

    WalletModel* walletModel_;
    QLineEdit* nameEdit_;
    QTextEdit* descriptionEdit_;
    QLineEdit* imageUriEdit_;
    QPushButton* browseImageButton_;
    QLineEdit* certificateTemplateEdit_;
    QPushButton* uploadCertificateButton_;
    QLineEdit* locationNameEdit_;
    QLineEdit* tagsEdit_;
    QLineEdit* cadastralNumberEdit_;
    QLineEdit* ownerNameEdit_;
    QLineEdit* landCategoryEdit_;
    QLineEdit* permittedUseEdit_;
    QDoubleSpinBox* areaOfficialEdit_;
    QLineEdit* ownershipHashEdit_;
    QLineEdit* technicalPlanHashEdit_;
    QLineEdit* photoHashesEdit_;
    QLineEdit* docsArchiveEdit_;
    QPushButton* uploadDocsArchiveButton_;
    QLineEdit* marketValueEdit_;
    QLineEdit* cadastralValueEdit_;
    QDateTimeEdit* valuationDateEdit_;
    QLineEdit* restrictionsEdit_;
    QCheckBox* hasBuildingsCheck_;
    QSpinBox* buildingCountEdit_;

    QDoubleSpinBox* centerLatDeg_;
    QDoubleSpinBox* centerLonDeg_;
    QDoubleSpinBox* sizeMeters_;
    QPushButton* autoFillSquareButton_;
    QTextEdit* polygonEdit_;
    QLabel* geometryStatus_;
    QGraphicsView* geometryPreview_;
    QGraphicsScene* geometryScene_;
    QLineEdit* feeGc_;
    QSpinBox* royaltyPercent_;
    bool submitted_ok_;
    std::vector<QWidget*> errorFields_;
};
void NFTPage::createNftClicked() {
    if (!walletModel || !walletModel->isWalletLoaded()) {
        lightWarning(this, tr("NFT / Cadastr"), tr("Please load or create a wallet first."));
        return;
    }

    CreateNftDialog dlg(this, walletModel);
    if (dlg.exec() != QDialog::Accepted) {
        return;
    }
    if (!dlg.submittedOk()) {
        return;
    }

    lightInfo(this, tr("NFT / Cadastr"), tr("NFT create transaction sent."));

        const QString txid = lastCreatedTxid_.trimmed();
        const QString dataDir = walletModel ? walletModel->getDataDirectory() : QString();
        const QString srcPath = resolveImageUriToLocalPath(dlg.imageUri(), dataDir);

        QString qrData;
        if (walletModel) {
            const auto wallet = walletModel->getWallet();
            qrData = buildNftQrDataFromWallet(wallet, txid, dlg.imageUri(), dlg.name());
        }

        if (!srcPath.isEmpty() && QFileInfo::exists(srcPath) && !txid.isEmpty()) {
            const QDir base(dataDir);
            const QString stampedFolder = QStringLiteral("nft_images_stamped");
            const QString targetFolderPath = base.filePath(stampedFolder);

            QFileInfo sfi(srcPath);
            QString ext = sfi.suffix().toLower();
            if (ext.isEmpty()) {
                ext = QStringLiteral("png");
            }
            const QString dstName = sfi.completeBaseName() + QStringLiteral("_stamped_") + txid.left(12) + QStringLiteral(".") + ext;
            const QString dstPath = QDir(targetFolderPath).filePath(dstName);

            QString stampBase = dlg.description();
            if (stampBase.trimmed().isEmpty()) {
                stampBase = dlg.name();
            }
            stampBase.replace('\n', ' ');
            stampBase.replace('\r', ' ');
            stampBase = stampBase.simplified();

            const int maxLen = 48;
            if (stampBase.size() > maxLen) {
                stampBase = stampBase.left(maxLen - 3) + QStringLiteral("...");
            }

            const QString stampText = stampBase;
            if (!createStampedCopy(srcPath, dstPath, stampText, qrData)) {
                lightWarning(this, tr("NFT / Cadastr"), tr("Failed to create stamped image copy."));
            } else {
#ifdef USE_QRCODE
                if (qrData.trimmed().isEmpty()) {
                    lightWarning(this, tr("NFT / Cadastr"), tr("Stamped image created, but QR data is empty (QR not added)."));
                }
#else
                lightWarning(this, tr("NFT / Cadastr"), tr("Stamped image created, but QR code support is disabled (qrencode not found at build time)."));
#endif
            }
        }

    refresh();
}
