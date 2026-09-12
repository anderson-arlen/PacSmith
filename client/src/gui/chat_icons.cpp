#include "gui/chat_icons.hpp"
#include <QApplication>
#include <QIconEngine>
#include <QPainter>
#include <QPalette>

namespace pacsmith::gui {
namespace {
class ComposerIcon final : public QIconEngine {
public:
    using Kind = ChatIcon;
    explicit ComposerIcon(Kind kind = Kind::Settings) : kind_(kind) {}
    QIconEngine *clone() const override { return new ComposerIcon(kind_); }
    void paint(QPainter *painter, const QRect &rect, QIcon::Mode mode, QIcon::State) override {
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        painter->translate(rect.topLeft());
        painter->scale(rect.width() / 20.0, rect.height() / 20.0);
        const auto palette = QApplication::palette();
        painter->setPen(QPen(palette.color(mode == QIcon::Disabled ? QPalette::Disabled : QPalette::Active, QPalette::ButtonText), 1.5));
        painter->setBrush(palette.color(QPalette::Button));
        if (kind_ == Kind::Send || kind_ == Kind::Stop) {
            painter->setPen(QPen(palette.color(QPalette::HighlightedText), 2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            if (kind_ == Kind::Stop) {
                painter->setBrush(palette.color(QPalette::HighlightedText));
                painter->drawRoundedRect(QRectF(5, 5, 10, 10), 1, 1);
            } else {
                painter->drawLine(3, 10, 17, 10);
                painter->drawPolyline(QPolygonF{{11, 4}, {17, 10}, {11, 16}});
            }
        } else if (kind_ == Kind::Image) {
            painter->setBrush(Qt::NoBrush);
            painter->drawPolyline(QPolygonF{{17, 10}, {17, 18}, {2, 18}, {2, 3}, {10, 3}});
            painter->drawPolyline(QPolygonF{{3, 16}, {7, 11}, {10, 14}, {13, 10}, {17, 15}});
            painter->drawEllipse(QPointF(6, 7), 1.5, 1.5);
            painter->drawLine(15, 1, 15, 7);
            painter->drawLine(12, 4, 18, 4);
        } else for (int i = 0; i < 3; ++i) {
            const auto x = 4 + i * 6;
            const auto y = i == 1 ? 13 : 7;
            painter->drawLine(x, 2, x, 18);
            painter->drawRoundedRect(QRectF(x - 2, y - 2, 4, 4), 1, 1);
        }
        painter->restore();
    }
    QPixmap pixmap(const QSize &size, QIcon::Mode mode, QIcon::State state) override {
        QPixmap result(size); result.fill(Qt::transparent);
        QPainter painter(&result); paint(&painter, QRect(QPoint(), size), mode, state);
        return result;
    }
private:
    Kind kind_;
};
}
QIcon chatIcon(ChatIcon kind) { return QIcon(new ComposerIcon(kind)); }
}
