#include "gui/age_unlock.hpp"

#include "core/credential_store.hpp"

#include <QInputDialog>
#include <QLineEdit>
#include <QMessageBox>

namespace pacsmith::gui {

bool promptUnlockAge(CredentialStore &store, const QString &agePath, QWidget *parent,
                     const bool allowCreate) {
    if (store.ageUnlocked()) return true;
    const bool creating = !store.hasAgeFile();
    if (creating && !allowCreate) return false;
    bool accepted = false;
    auto password = QInputDialog::getText(
        parent,
        creating ? QStringLiteral("Create PacSmith credential store")
                 : QStringLiteral("Unlock PacSmith credentials"),
        creating ? QStringLiteral("Set a password for PacSmith's encrypted credential store:")
                 : QStringLiteral("Password for %1:").arg(agePath),
        QLineEdit::Password, {}, &accepted);
    if (!accepted || password.isEmpty()) {
        password.fill(QChar::Null);
        return false;
    }
    if (creating) {
        bool confirmationAccepted = false;
        auto confirmation = QInputDialog::getText(
            parent, QStringLiteral("Confirm credential-store password"),
            QStringLiteral("Enter the same password again:"), QLineEdit::Password, {},
            &confirmationAccepted);
        const bool matches = confirmationAccepted && confirmation == password;
        confirmation.fill(QChar::Null);
        if (!matches) {
            password.fill(QChar::Null);
            if (confirmationAccepted) {
                QMessageBox::warning(parent, QStringLiteral("Passwords do not match"),
                                     QStringLiteral("The encrypted credential store was not created."));
            }
            return false;
        }
    }
    QString error;
    const bool ready = creating ? store.createAge(password, &error)
                                : store.unlockAge(password, &error);
    password.fill(QChar::Null);
    if (ready) return true;
    QMessageBox::critical(parent,
                          creating ? QStringLiteral("Could not create credential store")
                                   : QStringLiteral("Could not unlock credentials"),
                          error);
    return false;
}

} // namespace pacsmith::gui
