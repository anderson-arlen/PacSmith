#pragma once

#include "gui/main_window/support.hpp"

#include <QCompleter>
#include <QLineEdit>
#include <QPainter>
#include <QPen>
#include <QStyledItemDelegate>
#include <QTableWidget>

namespace pacsmith::gui {

class PackageNameDelegate final : public QStyledItemDelegate {
public:
    explicit PackageNameDelegate(QTableWidget *table) : QStyledItemDelegate(table), table_(table) {}

    QWidget *createEditor(QWidget *parent, const QStyleOptionViewItem &option,
                          const QModelIndex &index) const override {
        auto *editor = qobject_cast<QLineEdit *>(
            QStyledItemDelegate::createEditor(parent, option, index));
        if (editor == nullptr) return nullptr;
        auto *completer = new QCompleter(
            table_->property("pacsmithRepositoryPackages").toStringList(), editor);
        completer->setCaseSensitivity(Qt::CaseInsensitive);
        completer->setCompletionMode(QCompleter::PopupCompletion);
        completer->setFilterMode(Qt::MatchStartsWith);
        editor->setCompleter(completer);
        return editor;
    }

private:
    QTableWidget *table_;
};

class ProjectListDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    [[nodiscard]] QSize sizeHint(const QStyleOptionViewItem &option,
                                 const QModelIndex &index) const override {
        auto size = QStyledItemDelegate::sizeHint(option, index);
        size.setHeight(std::max(size.height(), 60));
        return size;
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override {
        painter->save();
        const auto selected = option.state.testFlag(QStyle::State_Selected);
        const auto darkTheme = option.palette.color(QPalette::Base).lightness() < 128;
        const auto activity = index.data(projectActivityRole).toString();
        const auto visualState =
            static_cast<ProjectVisualState>(index.data(projectVisualStateRole).toInt());
        const auto state = activity.isEmpty() ? visualState : ProjectVisualState::Preparing;
        const auto row = option.rect.adjusted(2, 2, -2, -2);

        if (selected) {
            QColor background;
            QColor border;
            switch (visualState) {
            case ProjectVisualState::Current:
                background = darkTheme ? QColor(24, 70, 42) : QColor(219, 244, 226);
                border = darkTheme ? QColor(70, 205, 108) : QColor(31, 145, 66);
                break;
            case ProjectVisualState::UpdateAvailable:
            case ProjectVisualState::Attention:
                background = darkTheme ? QColor(107, 75, 0) : QColor(255, 235, 184);
                border = darkTheme ? QColor(255, 190, 48) : QColor(180, 112, 0);
                break;
            case ProjectVisualState::Preparing:
                background = darkTheme ? QColor(27, 63, 86) : QColor(216, 238, 252);
                border = option.palette.link().color();
                break;
            case ProjectVisualState::NotInstalled:
                background = darkTheme ? QColor(53, 57, 61) : QColor(228, 231, 234);
                border = darkTheme ? QColor(139, 145, 151) : QColor(112, 119, 126);
                break;
            }
            painter->setRenderHint(QPainter::Antialiasing, true);
            painter->setBrush(background);
            painter->setPen(QPen(border, 1.5));
            painter->drawRoundedRect(row, 5, 5);
        }

        const auto iconSize = 44;
        const QRect iconRect(row.left() + 7, row.center().y() - iconSize / 2,
                             iconSize, iconSize);
        const auto icon = qvariant_cast<QIcon>(index.data(Qt::DecorationRole));
        icon.paint(painter, iconRect, Qt::AlignCenter,
                   option.state.testFlag(QStyle::State_Enabled) ? QIcon::Normal
                                                                : QIcon::Disabled);

        const auto textLeft = iconRect.right() + 10;
        const bool checking = index.data(projectCheckingRole).toBool() || !activity.isEmpty();
        const auto textWidth = std::max(0, row.right() - textLeft - 7 - (checking ? 24 : 0));
        const QRect nameRect(textLeft, row.top() + 8, textWidth, 21);
        const QRect subtitleRect(textLeft, row.top() + 31, textWidth, 19);
        auto nameFont = option.font;
        nameFont.setBold(true);
        painter->setFont(nameFont);
        painter->setPen(option.palette.text().color());
        painter->drawText(nameRect, Qt::AlignLeft | Qt::AlignVCenter,
                          option.fontMetrics.elidedText(index.data(Qt::DisplayRole).toString(),
                                                        Qt::ElideRight, textWidth));

        auto subtitleFont = option.font;
        subtitleFont.setPointSizeF(std::max(7.0, subtitleFont.pointSizeF() - 0.5));
        subtitleFont.setBold(state == ProjectVisualState::UpdateAvailable ||
                             state == ProjectVisualState::Preparing);
        painter->setFont(subtitleFont);
        QColor secondary;
        switch (state) {
        case ProjectVisualState::Current:
            secondary = darkTheme ? QColor(92, 214, 126) : QColor(24, 125, 55);
            break;
        case ProjectVisualState::UpdateAvailable:
        case ProjectVisualState::Attention:
            secondary = darkTheme ? QColor(255, 218, 128) : QColor(105, 61, 0);
            break;
        case ProjectVisualState::Preparing:
            secondary = option.palette.link().color();
            break;
        case ProjectVisualState::NotInstalled:
            secondary = selected
                ? (darkTheme ? QColor(205, 209, 213) : QColor(72, 78, 84))
                : option.palette.placeholderText().color();
            break;
        }
        painter->setPen(secondary);
        const QFontMetrics subtitleMetrics(subtitleFont);
        const auto subtitle = activity.isEmpty() ? index.data(projectSubtitleRole).toString()
                                                 : activity;
        painter->drawText(subtitleRect, Qt::AlignLeft | Qt::AlignVCenter,
                          subtitleMetrics.elidedText(subtitle, Qt::ElideRight, textWidth));
        if (checking) {
            int spinnerFrame = 0;
            if (option.widget != nullptr) {
                spinnerFrame = option.widget->property("pacsmithSpinnerFrame").toInt();
                if (spinnerFrame == 0 && option.widget->parentWidget() != nullptr) {
                    spinnerFrame = option.widget->parentWidget()->property("pacsmithSpinnerFrame").toInt();
                }
            }
            const auto spinnerSize = 16;
            const QRect spinnerRect(row.right() - spinnerSize - 6,
                                    row.center().y() - spinnerSize / 2, spinnerSize, spinnerSize);
            painter->setRenderHint(QPainter::Antialiasing, true);
            painter->setBrush(Qt::NoBrush);
            auto spinnerPen = QPen(option.palette.link().color(), 2.25);
            spinnerPen.setCapStyle(Qt::RoundCap);
            painter->setPen(spinnerPen);
            painter->drawArc(spinnerRect, (spinnerFrame % 4) * 90 * 16, 270 * 16);
        }
        painter->restore();
    }
};

} // namespace pacsmith::gui
