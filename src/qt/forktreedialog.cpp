#include "forktreedialog.h"

#include "clientmodel.h"
#include "guiutil.h"
#include "geocoin/core/blockchain.hpp"
#include "geocoin/storage/block_storage.hpp"

#include <QApplication>
#include <QClipboard>
#include <QDialog>
#include <QFormLayout>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

#include <iomanip>
#include <sstream>
#include <vector>

namespace {

QString hashToHexString(const geocoin::Hash256& hash)
{
    std::ostringstream oss;
    for (auto byte : hash) {
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(byte);
    }
    return QString::fromStdString(oss.str());
}

bool parseHash256(const QString& hex, geocoin::Hash256& out)
{
    const QString text = hex.trimmed().toLower();
    if (text.size() != 64) {
        return false;
    }
    for (int i = 0; i < 32; ++i) {
        bool ok = false;
        const auto part = text.mid(i * 2, 2);
        const int value = part.toInt(&ok, 16);
        if (!ok || value < 0 || value > 255) {
            return false;
        }
        out[static_cast<size_t>(i)] = static_cast<geocoin::byte>(value);
    }
    return true;
}

class ForkTreeDialog final : public QDialog
{
public:
    explicit ForkTreeDialog(ClientModel* clientModel, QWidget* parent = nullptr)
        : QDialog(parent)
        , clientModel_(clientModel)
        , rootEdit_(new QLineEdit(this))
        , depthSpin_(new QSpinBox(this))
        , maxNodesSpin_(new QSpinBox(this))
        , tree_(new QTreeWidget(this))
        , statusLabel_(new QLabel(this))
        , loadButton_(new QPushButton(tr("Load tree"), this))
        , useTipButton_(new QPushButton(tr("Use active tip"), this))
        , copyJsonButton_(new QPushButton(tr("Copy JSON"), this))
    {
        setWindowTitle(tr("Engineering: Fork Tree"));
        setModal(true);
        setMinimumSize(720, 520);

        setStyleSheet(
            "QDialog { background-color: #F5F5DC; }"
            "QLabel { color: #000000; background-color: transparent; }"
            "QLineEdit { background-color: white; color: black; border: 1px solid #999999; padding: 5px; border-radius: 3px; }"
            "QSpinBox { background-color: white; color: black; border: 1px solid #999999; padding: 3px; border-radius: 3px; }"
            "QPushButton { background-color: #E0E0E0; color: #000000; border: 1px solid #999999; padding: 5px 15px; border-radius: 3px; min-width: 80px; }"
            "QPushButton:hover { background-color: #D0D0D0; }"
        );

        depthSpin_->setRange(0, 10);
        depthSpin_->setValue(2);
        maxNodesSpin_->setRange(1, 100000);
        maxNodesSpin_->setValue(1000);

        tree_->setHeaderLabels({tr("Block hash")});
        tree_->header()->setSectionResizeMode(QHeaderView::Stretch);

        QFormLayout* form = new QFormLayout();
        QWidget* rootRow = new QWidget(this);
        QHBoxLayout* rootLayout = new QHBoxLayout(rootRow);
        rootLayout->setContentsMargins(0, 0, 0, 0);
        rootLayout->addWidget(rootEdit_);
        rootLayout->addWidget(useTipButton_);
        rootRow->setLayout(rootLayout);
        form->addRow(tr("Root hash"), rootRow);
        form->addRow(tr("Max depth"), depthSpin_);
        form->addRow(tr("Max nodes"), maxNodesSpin_);

        QHBoxLayout* buttons = new QHBoxLayout();
        buttons->addWidget(loadButton_);
        buttons->addWidget(copyJsonButton_);
        buttons->addStretch();
        QPushButton* closeButton = new QPushButton(tr("Close"), this);
        buttons->addWidget(closeButton);

        QVBoxLayout* layout = new QVBoxLayout(this);
        layout->addLayout(form);
        layout->addWidget(tree_);
        layout->addWidget(statusLabel_);
        layout->addLayout(buttons);
        setLayout(layout);

        connect(loadButton_, &QPushButton::clicked, this, &ForkTreeDialog::loadTree);
        connect(useTipButton_, &QPushButton::clicked, this, &ForkTreeDialog::fillWithActiveTip);
        connect(copyJsonButton_, &QPushButton::clicked, this, &ForkTreeDialog::copyJson);
        connect(closeButton, &QPushButton::clicked, this, &QDialog::reject);
    }

private:
    void showWarn(const QString& text) const
    {
        GUIUtil::showStyledMessageBox(const_cast<ForkTreeDialog*>(this), QMessageBox::Warning, tr("Fork Tree"), text);
    }

    std::shared_ptr<geocoin::core::Blockchain> blockchain() const
    {
        if (!clientModel_) return nullptr;
        return clientModel_->getNodeComponents().blockchain;
    }

    std::shared_ptr<geocoin::storage::BlockStorage> storage() const
    {
        auto bc = blockchain();
        if (!bc) return nullptr;
        return bc->get_block_storage();
    }

    bool resolveHeight(const geocoin::Hash256& hash, geocoin::Height& outHeight) const
    {
        auto bc = blockchain();
        if (!bc) {
            return false;
        }
        auto* idx = bc->find_block_index(hash);
        if (!idx) {
            return false;
        }
        outHeight = idx->height;
        return true;
    }

    std::vector<geocoin::Hash256> getChildren(const geocoin::Hash256& parentHash, geocoin::Height parentHeight) const
    {
        std::vector<geocoin::Hash256> children;
        if (!storage_) {
            return children;
        }
        const geocoin::Height childHeight = parentHeight + 1;
        const auto candidates = storage_->get_block_hashes_by_height(childHeight);
        for (const auto& h : candidates) {
            geocoin::core::Block childBlock;
            if (!storage_->get_block(h, childBlock, false)) {
                continue;
            }
            if (childBlock.header.prev_block_hash == parentHash) {
                children.push_back(h);
            }
        }
        return children;
    }

    QJsonObject buildJsonNode(const geocoin::Hash256& hash,
                              geocoin::Height height,
                              int depth,
                              int maxDepth,
                              size_t maxNodes,
                              size_t& visited) const
    {
        QJsonObject obj;
        if (visited >= maxNodes) {
            return obj;
        }
        ++visited;

        obj.insert(QStringLiteral("hash"), hashToHexString(hash));
        obj.insert(QStringLiteral("height"), static_cast<int>(height));

        if (depth >= maxDepth) {
            obj.insert(QStringLiteral("children"), QJsonArray());
            return obj;
        }

        QJsonArray childrenArr;
        const auto children = getChildren(hash, height);
        for (const auto& ch : children) {
            if (visited >= maxNodes) {
                break;
            }
            childrenArr.append(buildJsonNode(ch, height + 1, depth + 1, maxDepth, maxNodes, visited));
        }
        obj.insert(QStringLiteral("children"), childrenArr);
        return obj;
    }

    void addTreeNode(const geocoin::Hash256& hash,
                     geocoin::Height height,
                     int depth,
                     int maxDepth,
                     size_t maxNodes,
                     size_t& visited,
                     QTreeWidgetItem* parent)
    {
        if (visited >= maxNodes) {
            return;
        }
        ++visited;

        QTreeWidgetItem* item = new QTreeWidgetItem();
        item->setText(0, hashToHexString(hash));
        item->setToolTip(0, tr("Height: %1").arg(static_cast<int>(height)));
        if (parent) {
            parent->addChild(item);
        } else {
            tree_->addTopLevelItem(item);
        }

        if (depth >= maxDepth) {
            return;
        }

        const auto children = getChildren(hash, height);
        for (const auto& ch : children) {
            if (visited >= maxNodes) {
                break;
            }
            addTreeNode(ch, height + 1, depth + 1, maxDepth, maxNodes, visited, item);
        }
    }

    void fillWithActiveTip()
    {
        auto bc = blockchain();
        if (!bc) {
            showWarn(tr("Blockchain is not available."));
            return;
        }
        auto* tip = bc->get_tip();
        if (!tip) {
            showWarn(tr("Active tip is not available."));
            return;
        }
        rootEdit_->setText(hashToHexString(tip->get_block_hash()));
    }

    void loadTree()
    {
        tree_->clear();
        statusLabel_->clear();
        hasTree_ = false;

        storage_ = storage();
        if (!storage_) {
            showWarn(tr("BlockStorage is not available."));
            return;
        }

        geocoin::Hash256 root{};
        if (!parseHash256(rootEdit_->text(), root)) {
            showWarn(tr("Invalid root hash."));
            return;
        }

        geocoin::Height rootHeight = 0;
        if (!resolveHeight(root, rootHeight)) {
            showWarn(tr("Root hash is not found in block index."));
            return;
        }

        const int maxDepth = depthSpin_->value();
        const size_t maxNodes = static_cast<size_t>(maxNodesSpin_->value());

        size_t visitedTree = 0;
        addTreeNode(root, rootHeight, 0, maxDepth, maxNodes, visitedTree, nullptr);
        tree_->expandToDepth(1);

        size_t visitedJson = 0;
        QJsonObject payload;
        payload.insert(QStringLiteral("root"), buildJsonNode(root, rootHeight, 0, maxDepth, maxNodes, visitedJson));
        payload.insert(QStringLiteral("max_depth"), maxDepth);
        payload.insert(QStringLiteral("max_nodes"), static_cast<int>(maxNodes));
        payload.insert(QStringLiteral("nodes_returned"), static_cast<int>(visitedJson));
        lastTreeDoc_ = QJsonDocument(payload);
        hasTree_ = true;

        QString status = tr("Nodes: %1").arg(static_cast<int>(visitedTree));
        if (visitedTree >= maxNodes) {
            status += tr(" (truncated)");
        }
        statusLabel_->setText(status);
    }

    void copyJson()
    {
        if (!hasTree_) {
            showWarn(tr("No tree loaded yet."));
            return;
        }
        QClipboard* clipboard = QApplication::clipboard();
        clipboard->setText(QString::fromUtf8(lastTreeDoc_.toJson(QJsonDocument::Indented)));
    }

    ClientModel* clientModel_;
    QLineEdit* rootEdit_;
    QSpinBox* depthSpin_;
    QSpinBox* maxNodesSpin_;
    QTreeWidget* tree_;
    QLabel* statusLabel_;
    QPushButton* loadButton_;
    QPushButton* useTipButton_;
    QPushButton* copyJsonButton_;
    std::shared_ptr<geocoin::storage::BlockStorage> storage_;
    QJsonDocument lastTreeDoc_;
    bool hasTree_{false};
};

} // namespace

void showForkTreeDialog(ClientModel* clientModel, QWidget* parent)
{
    ForkTreeDialog dialog(clientModel, parent);
    dialog.exec();
}
