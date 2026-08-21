#pragma once

#include <QString>

class QWidget;

namespace pacsmith {

class CredentialStore;

}

namespace pacsmith::gui {

[[nodiscard]] bool promptUnlockAge(CredentialStore &store, const QString &agePath,
                                   QWidget *parent, bool allowCreate = true);

} // namespace pacsmith::gui
