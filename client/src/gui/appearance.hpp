#pragma once

#include "core/app_settings.hpp"

#include <QColor>
#include <QIcon>
#include <QPixmap>

namespace pacsmith::gui {

void applyInterfaceTheme(AppearanceMode mode);
[[nodiscard]] QColor trayIconColor(AppearanceMode mode);
[[nodiscard]] QIcon renderTrayStatusIcon(const QPixmap &mask, int availableUpdates,
                                         const QColor &foreground, int activityFrame = -1);

} // namespace pacsmith::gui
