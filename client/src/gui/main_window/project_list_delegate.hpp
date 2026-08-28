#pragma once

#include "gui/main_window/support.hpp"

#include <QCompleter>
#include <QFontDatabase>
#include <QLineEdit>
#include <QPainter>
#include <QPainterPath>
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
        const bool checking = index.data(projectCheckingRole).toBool() || !activity.isEmpty();
        const auto state = checking ? ProjectVisualState::Preparing : visualState;
        int spinnerFrame = 0;
        if (option.widget != nullptr) {
            spinnerFrame = option.widget->property("pacsmithSpinnerFrame").toInt();
            if (spinnerFrame == 0 && option.widget->parentWidget() != nullptr) {
                spinnerFrame = option.widget->parentWidget()->property("pacsmithSpinnerFrame").toInt();
            }
        }
        const auto row = option.rect.adjusted(2, 2, -2, -2);

        if (selected) {
            QColor background;
            QColor border;
            switch (state) {
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
        paintStatusBadge(painter, iconRect, state, spinnerFrame);

        const auto textLeft = iconRect.right() + 10;
        const auto textWidth = std::max(0, row.right() - textLeft - 7);
        const QRect nameRect(textLeft, row.top() + 8, textWidth, 21);
        const QRect subtitleRect(textLeft, row.top() + 31, textWidth, 19);
        auto nameFont = option.font;
        nameFont.setBold(true);
        painter->setFont(nameFont);
        painter->setPen(option.palette.text().color());
        painter->drawText(nameRect, Qt::AlignLeft | Qt::AlignVCenter,
                          option.fontMetrics.elidedText(index.data(Qt::DisplayRole).toString(),
                                                        Qt::ElideRight, textWidth));

        auto subtitleFont = activity.isEmpty()
            ? QFontDatabase::systemFont(QFontDatabase::FixedFont) : option.font;
        subtitleFont.setPointSizeF(std::max(7.0, option.font.pointSizeF() - 0.5));
        subtitleFont.setBold(!activity.isEmpty());
        painter->setFont(subtitleFont);
        const auto secondary = !activity.isEmpty()
            ? option.palette.link().color()
            : selected ? option.palette.text().color()
                       : option.palette.placeholderText().color();
        painter->setPen(secondary);
        const QFontMetrics subtitleMetrics(subtitleFont);
        const auto subtitle = activity.isEmpty() ? index.data(projectSubtitleRole).toString()
                                                 : activity;
        const bool repositoryEnabled = index.data(projectRepositoryEnabledRole).toBool();
        const bool repositoryBusy = index.data(projectRepositoryBusyRole).toBool();
        const int indicatorWidth = repositoryEnabled || repositoryBusy ? 16 : 0;
        const auto subtitleWidth = std::max(0, textWidth - indicatorWidth);
        const auto displayedSubtitle = subtitleMetrics.elidedText(
            subtitle, Qt::ElideRight, subtitleWidth);
        painter->drawText(subtitleRect, Qt::AlignLeft | Qt::AlignVCenter,
                          displayedSubtitle);
        if (repositoryEnabled || repositoryBusy) {
            const auto indicatorLeft = std::min(
                subtitleRect.right() - indicatorWidth + 1,
                subtitleRect.left() + subtitleMetrics.horizontalAdvance(displayedSubtitle) + 4);
            paintRepositoryIndicator(
                painter, QRect(indicatorLeft, subtitleRect.top() + 2, 14, 14),
                repositoryBusy, spinnerFrame, secondary);
        }
        painter->restore();
    }

private:
    static void paintStatusBadge(QPainter *painter, const QRect &iconRect,
                                 const ProjectVisualState state, const int spinnerFrame) {
        const QRect badge(iconRect.right() - 15, iconRect.bottom() - 15, 17, 17);
        QColor color;
        switch (state) {
        case ProjectVisualState::Current: color = QColor(32, 145, 70); break;
        case ProjectVisualState::UpdateAvailable: color = QColor(196, 126, 0); break;
        case ProjectVisualState::Attention: color = QColor(190, 48, 48); break;
        case ProjectVisualState::Preparing: color = QColor(35, 125, 185); break;
        case ProjectVisualState::NotInstalled: color = QColor(105, 112, 120); break;
        }
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        painter->setPen(Qt::NoPen);
        painter->setBrush(Qt::white);
        painter->drawEllipse(badge);
        const auto inner = badge.adjusted(2, 2, -2, -2);
        painter->setBrush(color);
        painter->drawEllipse(inner);
        auto glyphPen = QPen(Qt::white, 1.7);
        glyphPen.setCapStyle(Qt::RoundCap);
        glyphPen.setJoinStyle(Qt::RoundJoin);
        painter->setPen(glyphPen);
        painter->setBrush(Qt::NoBrush);
        const auto center = inner.center();
        switch (state) {
        case ProjectVisualState::Current:
            painter->drawPolyline(QPolygon({QPoint(center.x() - 3, center.y()),
                                             QPoint(center.x() - 1, center.y() + 2),
                                             QPoint(center.x() + 4, center.y() - 3)}));
            break;
        case ProjectVisualState::UpdateAvailable:
            painter->drawLine(center.x(), center.y() + 4, center.x(), center.y() - 4);
            painter->drawLine(center.x(), center.y() - 4, center.x() - 3, center.y() - 1);
            painter->drawLine(center.x(), center.y() - 4, center.x() + 3, center.y() - 1);
            break;
        case ProjectVisualState::Attention:
            painter->drawLine(center.x(), center.y() - 4, center.x(), center.y() + 1);
            painter->drawPoint(center.x(), center.y() + 4);
            break;
        case ProjectVisualState::Preparing:
            painter->drawArc(inner.adjusted(2, 2, -2, -2),
                             (spinnerFrame % 4) * 90 * 16, 250 * 16);
            break;
        case ProjectVisualState::NotInstalled:
            painter->drawLine(center.x() - 3, center.y(), center.x() + 3, center.y());
            break;
        }
        painter->restore();
    }

    static void paintRepositoryIndicator(QPainter *painter, const QRect &rect,
                                         const bool busy,
                                         const int spinnerFrame, const QColor &secondary) {
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        auto color = busy ? QColor(35, 125, 185) : secondary;
        auto pen = QPen(color, 1.1);
        pen.setCapStyle(Qt::RoundCap);
        pen.setJoinStyle(Qt::RoundJoin);
        painter->setPen(pen);
        painter->setBrush(color);
        const QPointF transmitter(rect.center().x(), rect.top() + 8);
        painter->drawEllipse(transmitter, 1.1, 1.1);
        painter->drawLine(transmitter + QPointF(0, 1), QPointF(transmitter.x(), rect.bottom() - 2));
        painter->drawLine(QPointF(transmitter.x() - 3, rect.bottom() - 2),
                          QPointF(transmitter.x() + 3, rect.bottom() - 2));
        const auto drawWave = [&](const qreal distance, const qreal rise) {
            QPainterPath left;
            left.moveTo(transmitter + QPointF(-distance, 0));
            left.cubicTo(transmitter + QPointF(-distance, -rise / 2),
                         transmitter + QPointF(-distance / 2, -rise),
                         transmitter + QPointF(0, -rise));
            painter->drawPath(left);
            QPainterPath right;
            right.moveTo(transmitter + QPointF(distance, 0));
            right.cubicTo(transmitter + QPointF(distance, -rise / 2),
                          transmitter + QPointF(distance / 2, -rise),
                          transmitter + QPointF(0, -rise));
            painter->drawPath(right);
        };
        if (!busy || spinnerFrame % 2 == 0) drawWave(3.5, 3.5);
        if (!busy || spinnerFrame % 4 >= 2) drawWave(6.0, 6.0);
        painter->restore();
    }
};

} // namespace pacsmith::gui
