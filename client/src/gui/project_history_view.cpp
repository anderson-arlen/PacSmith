#include "gui/project_history_view.hpp"

#include "core/model.hpp"

#include <QListWidget>

namespace pacsmith::gui {

void populateProjectHistory(QListWidget *list, const Project *project) {
    if (list == nullptr) return;
    list->clear();
    if (project == nullptr) return;
    for (auto iterator = project->history.crbegin(); iterator != project->history.crend(); ++iterator) {
        list->addItem(QStringLiteral("%1  ·  %2  ·  %3")
                          .arg(iterator->timestamp.toLocalTime().toString(
                                   QStringLiteral("yyyy-MM-dd HH:mm:ss")),
                               iterator->event, iterator->detail));
    }
}

} // namespace pacsmith::gui
