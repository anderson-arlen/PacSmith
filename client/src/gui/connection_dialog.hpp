#pragma once

class QWidget;

namespace pacsmith::gui {

[[nodiscard]] bool runConnectionDialog(QWidget *parent);
void restartPacsmithGui();

} // namespace pacsmith::gui
