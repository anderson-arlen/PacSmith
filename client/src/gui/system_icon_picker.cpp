#include "gui/system_icons.hpp"

#include <QDialog>
#include <QDialogButtonBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>

namespace pacsmith::gui {

QString chooseSystemIcon(QWidget *parent) {
    QDialog dialog(parent);
    dialog.setObjectName(QStringLiteral("systemIconPicker"));
    dialog.setWindowTitle(QStringLiteral("Choose a system icon"));
    dialog.resize(780, 580);
    auto *layout = new QVBoxLayout(&dialog);
    auto *search = new QLineEdit(&dialog);
    search->setObjectName(QStringLiteral("systemIconSearch"));
    search->setPlaceholderText(QStringLiteral("Search installed icons, e.g. font, terminal, folder…"));
    search->setClearButtonEnabled(true);
    layout->addWidget(search);
    auto *list = new QListWidget(&dialog);
    list->setObjectName(QStringLiteral("systemIconList"));
    list->setViewMode(QListView::ListMode);
    list->setMovement(QListView::Static);
    list->setResizeMode(QListView::Adjust);
    list->setIconSize(QSize(48, 48));
    list->setWordWrap(false);
    list->setTextElideMode(Qt::ElideNone);
    list->setUniformItemSizes(true);
    for (const auto &choice : availableSystemIcons()) {
        auto *item = new QListWidgetItem(choice.icon, choice.label, list);
        item->setData(Qt::UserRole, choice.name);
        item->setToolTip(choice.name);
    }
    layout->addWidget(list, 1);
    auto *count = new QLabel(&dialog);
    layout->addWidget(count);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Use Icon"));
    buttons->button(QDialogButtonBox::Ok)->setEnabled(false);
    layout->addWidget(buttons);
    const auto filter = [=](const QString &text) {
        list->setCurrentItem(nullptr);
        list->clearSelection();
        int matches = 0;
        for (int row = 0; row < list->count(); ++row) {
            auto *item = list->item(row);
            const bool visible = item->text().contains(text.trimmed(), Qt::CaseInsensitive);
            item->setHidden(!visible);
            if (visible) ++matches;
        }
        count->setText(matches == 0 ? QStringLiteral("No matching icons")
                                    : QStringLiteral("%1 icons").arg(matches));
    };
    QObject::connect(search, &QLineEdit::textChanged, &dialog, filter);
    QObject::connect(list, &QListWidget::currentItemChanged, &dialog,
                     [buttons](QListWidgetItem *item) {
        buttons->button(QDialogButtonBox::Ok)->setEnabled(item != nullptr && !item->isHidden());
    });
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QObject::connect(list, &QListWidget::itemDoubleClicked, &dialog, [&dialog] { dialog.accept(); });
    filter({});
    search->setFocus();
    if (dialog.exec() != QDialog::Accepted || list->currentItem() == nullptr ||
        list->currentItem()->isHidden()) return {};
    return list->currentItem()->data(Qt::UserRole).toString();
}

} // namespace pacsmith::gui
