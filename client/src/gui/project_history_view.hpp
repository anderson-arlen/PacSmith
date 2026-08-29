#pragma once

class QListWidget;

namespace pacsmith {
struct Project;
}

namespace pacsmith::gui {

void populateProjectHistory(QListWidget *list, const Project *project);

} // namespace pacsmith::gui
