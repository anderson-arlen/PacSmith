#include "gui/acp_registry_dialog.hpp"
#include "core/acp_registry.hpp"
#include "core/acp_environment.hpp"
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFutureWatcher>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QStandardPaths>
#include <QVBoxLayout>
#include <QtConcurrent>

namespace pacsmith::gui {
namespace {
struct RegistryResult { std::optional<AcpRegistrySnapshot> snapshot; QString error; };
}
std::optional<HarnessProfile> chooseRegistryAgent(QWidget *parent) {
    QDialog dialog(parent);
    dialog.setWindowTitle(QStringLiteral("ACP Registry"));
    dialog.resize(650, 600);
    auto *layout = new QVBoxLayout(&dialog);
    auto *search = new QLineEdit(&dialog);
    search->setPlaceholderText(QStringLiteral("Search agents…"));
    layout->addWidget(search);
    auto *list = new QListWidget(&dialog);
    layout->addWidget(list, 1);
    auto *details = new QLabel(&dialog);
    details->setWordWrap(true);
    details->setTextFormat(Qt::PlainText);
    layout->addWidget(details);
    auto *status = new QLabel(QStringLiteral("Loading the official ACP registry…"), &dialog);
    status->setWordWrap(true);
    status->setTextFormat(Qt::PlainText);
    layout->addWidget(status);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, &dialog);
    auto *refresh = buttons->addButton(QStringLiteral("Refresh"), QDialogButtonBox::ActionRole);
    auto *use = buttons->addButton(QStringLiteral("Use agent"), QDialogButtonBox::AcceptRole);
    use->setEnabled(false);
    layout->addWidget(buttons);
    QList<AcpRegistryAgent> agents;
    std::optional<HarnessProfile> selected;
    const auto npx = QStandardPaths::findExecutable(QStringLiteral("npx"));
    const auto uvx = QStandardPaths::findExecutable(QStringLiteral("uvx"));
    const auto filter = [&] {
        for (int i = 0; i < list->count(); ++i) {
            auto *item = list->item(i);
            const auto &agent = agents.at(item->data(Qt::UserRole).toInt());
            item->setHidden(!(agent.name + QLatin1Char(' ') + agent.description).contains(search->text(), Qt::CaseInsensitive));
        }
    };
    QObject::connect(search, &QLineEdit::textChanged, &dialog, filter);
    QObject::connect(list, &QListWidget::currentRowChanged, &dialog, [&] {
        selected.reset();
        if (list->currentItem() == nullptr) { use->setEnabled(false); return; }
        const auto &agent = agents.at(list->currentItem()->data(Qt::UserRole).toInt());
        QString error;
        selected = acpRegistryProfile(agent, &error, npx, uvx);
        details->setText(QStringLiteral("%1 · %2\n%3\n\n%4").arg(agent.name, agent.version, agent.description,
            selected ? QStringLiteral("The agent’s declared package version will be downloaded by its package runner on first use.") : error));
        use->setEnabled(selected.has_value());
    });
    const auto load = [&](bool force) {
        refresh->setEnabled(false);
        auto *watcher = new QFutureWatcher<RegistryResult>(&dialog);
        QObject::connect(watcher, &QFutureWatcher<RegistryResult>::finished, &dialog, [&, watcher] {
            const auto result = watcher->result(); watcher->deleteLater();
            refresh->setEnabled(true);
            if (!result.snapshot) { status->setText(result.error); return; }
            list->clear();
            agents = result.snapshot->agents;
            for (qsizetype i = 0; i < agents.size(); ++i) {
                auto *item = new QListWidgetItem(agents.at(i).name + QStringLiteral(" · ") + agents.at(i).version, list);
                item->setData(Qt::UserRole, static_cast<int>(i));
            }
            filter();
            status->setText(result.snapshot->notice.isEmpty()
                ? QStringLiteral("Official ACP registry · %1 · %2").arg(result.snapshot->fromCache ? QStringLiteral("cached") : QStringLiteral("updated"), result.snapshot->fetchedAt.toLocalTime().toString(Qt::ISODate))
                : result.snapshot->notice);
        });
        watcher->setFuture(QtConcurrent::run([force] {
            RegistryResult result;
            result.snapshot = loadAcpRegistry(QDir(acpDataDirectory()).filePath(QStringLiteral("registry")), force, &result.error);
            return result;
        }));
    };
    QObject::connect(refresh, &QPushButton::clicked, &dialog, [&] { load(true); });
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    load(false);
    if (dialog.exec() != QDialog::Accepted) return std::nullopt;
    return selected;
}
}
