#include "gui/appearance.hpp"

#include <QApplication>
#include <QGuiApplication>
#include <QPalette>
#include <QPainter>
#include <QStyle>
#include <QStyleHints>

namespace pacsmith::gui {

void applyInterfaceTheme(const AppearanceMode mode) {
    auto *application = qobject_cast<QApplication *>(QCoreApplication::instance());
    if (application == nullptr) return;
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    auto scheme = Qt::ColorScheme::Unknown;
    if (mode == AppearanceMode::Light) scheme = Qt::ColorScheme::Light;
    else if (mode == AppearanceMode::Dark) scheme = Qt::ColorScheme::Dark;
    QGuiApplication::styleHints()->setColorScheme(scheme);
#endif
    static bool explicitlyThemed = false;
    if (mode == AppearanceMode::Auto) {
        if (explicitlyThemed) application->setPalette(application->style()->standardPalette());
        explicitlyThemed = false;
        return;
    }

    QPalette palette;
    if (mode == AppearanceMode::Dark) {
        palette.setColor(QPalette::Window, QColor(53, 53, 53));
        palette.setColor(QPalette::WindowText, Qt::white);
        palette.setColor(QPalette::Base, QColor(35, 35, 35));
        palette.setColor(QPalette::AlternateBase, QColor(53, 53, 53));
        palette.setColor(QPalette::ToolTipBase, QColor(35, 35, 35));
        palette.setColor(QPalette::ToolTipText, Qt::white);
        palette.setColor(QPalette::Text, Qt::white);
        palette.setColor(QPalette::Button, QColor(53, 53, 53));
        palette.setColor(QPalette::ButtonText, Qt::white);
        palette.setColor(QPalette::BrightText, Qt::red);
        palette.setColor(QPalette::Link, QColor(66, 165, 245));
        palette.setColor(QPalette::LinkVisited, QColor(179, 157, 219));
        palette.setColor(QPalette::PlaceholderText, QColor(170, 170, 170));
        palette.setColor(QPalette::Light, QColor(88, 88, 88));
        palette.setColor(QPalette::Midlight, QColor(75, 75, 75));
        palette.setColor(QPalette::Mid, QColor(63, 63, 63));
        palette.setColor(QPalette::Dark, QColor(32, 32, 32));
        palette.setColor(QPalette::Shadow, Qt::black);
        palette.setColor(QPalette::Highlight, QColor(42, 130, 218));
        palette.setColor(QPalette::HighlightedText, Qt::black);
        const QColor disabledText(154, 154, 154);
        palette.setColor(QPalette::Disabled, QPalette::WindowText, disabledText);
        palette.setColor(QPalette::Disabled, QPalette::Text, disabledText);
        palette.setColor(QPalette::Disabled, QPalette::ButtonText, disabledText);
        palette.setColor(QPalette::Disabled, QPalette::PlaceholderText, QColor(130, 130, 130));
        palette.setColor(QPalette::Disabled, QPalette::Highlight, QColor(75, 75, 75));
        palette.setColor(QPalette::Disabled, QPalette::HighlightedText, disabledText);
    } else {
        palette.setColor(QPalette::Window, QColor(239, 239, 239));
        palette.setColor(QPalette::WindowText, Qt::black);
        palette.setColor(QPalette::Base, Qt::white);
        palette.setColor(QPalette::AlternateBase, QColor(247, 247, 247));
        palette.setColor(QPalette::ToolTipBase, QColor(255, 255, 220));
        palette.setColor(QPalette::ToolTipText, Qt::black);
        palette.setColor(QPalette::Text, Qt::black);
        palette.setColor(QPalette::Button, QColor(239, 239, 239));
        palette.setColor(QPalette::ButtonText, Qt::black);
        palette.setColor(QPalette::BrightText, Qt::red);
        palette.setColor(QPalette::Link, QColor(0, 102, 204));
        palette.setColor(QPalette::LinkVisited, QColor(91, 66, 145));
        palette.setColor(QPalette::PlaceholderText, QColor(92, 92, 92));
        palette.setColor(QPalette::Light, Qt::white);
        palette.setColor(QPalette::Midlight, QColor(224, 224, 224));
        palette.setColor(QPalette::Mid, QColor(160, 160, 160));
        palette.setColor(QPalette::Dark, QColor(105, 105, 105));
        palette.setColor(QPalette::Shadow, QColor(48, 48, 48));
        palette.setColor(QPalette::Highlight, QColor(48, 140, 198));
        palette.setColor(QPalette::HighlightedText, Qt::white);
        const QColor disabledText(105, 105, 105);
        palette.setColor(QPalette::Disabled, QPalette::WindowText, disabledText);
        palette.setColor(QPalette::Disabled, QPalette::Text, disabledText);
        palette.setColor(QPalette::Disabled, QPalette::ButtonText, disabledText);
        palette.setColor(QPalette::Disabled, QPalette::PlaceholderText, QColor(125, 125, 125));
        palette.setColor(QPalette::Disabled, QPalette::Highlight, QColor(190, 190, 190));
        palette.setColor(QPalette::Disabled, QPalette::HighlightedText, disabledText);
    }
    application->setPalette(palette);
    explicitlyThemed = true;
}

QColor trayIconColor(const AppearanceMode mode) {
    if (mode == AppearanceMode::Light) return Qt::black;
    if (mode == AppearanceMode::Dark) return Qt::white;
    return QGuiApplication::palette().color(QPalette::WindowText);
}

QIcon renderTrayStatusIcon(const QPixmap &mask, const int availableUpdates,
                           const QColor &foreground, const int activityFrame) {
    QPixmap pixmap(mask.size());
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    painter.drawPixmap(0, 0, mask);
    painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
    painter.fillRect(pixmap.rect(), foreground);
    if (availableUpdates > 0) {
        painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
        const QRect badgeRect(17, 17, 15, 15);
        painter.setBrush(QColor(210, 50, 50));
        painter.setPen(Qt::white);
        painter.drawEllipse(badgeRect);
        auto font = painter.font();
        font.setBold(true);
        font.setPixelSize(10);
        painter.setFont(font);
        painter.drawText(badgeRect, Qt::AlignCenter,
                         availableUpdates > 9 ? QStringLiteral("9+")
                                              : QString::number(availableUpdates));
    }
    if (activityFrame >= 0) {
        painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
        const QRect spinnerRect(18, 18, 12, 12);
        painter.setBrush(QColor(35, 35, 35, 210));
        painter.setPen(Qt::NoPen);
        painter.drawEllipse(spinnerRect.adjusted(-2, -2, 2, 2));
        QPen spinnerPen(QColor(82, 190, 255), 2.5, Qt::SolidLine, Qt::RoundCap);
        painter.setPen(spinnerPen);
        painter.setBrush(Qt::NoBrush);
        painter.drawArc(spinnerRect, (activityFrame % 8) * -45 * 16, 250 * 16);
    }
    return QIcon(pixmap);
}

} // namespace pacsmith::gui
