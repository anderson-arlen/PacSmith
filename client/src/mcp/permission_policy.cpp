#include "mcp/permission_policy.hpp"

#include <QSet>

namespace pacsmith::mcp {

PermissionLevel PermissionPolicy::level(const QString &toolName) {
    static const QSet<QString> mandatory{
        QStringLiteral("delete_project"),
        QStringLiteral("delete_release"),
        QStringLiteral("reanalyze_release"),
        QStringLiteral("configure_project_repository"),
        QStringLiteral("promote_repository_package"),
        QStringLiteral("set_library_settings"),
        QStringLiteral("set_client_preferences"),
        QStringLiteral("set_repository_configuration"),
        QStringLiteral("initialize_repository_signing"),
        QStringLiteral("upload_repository_root_key"),
        QStringLiteral("upload_repository_certified_key"),
        QStringLiteral("set_remote_listening"),
        QStringLiteral("approve_remote_registration"),
        QStringLiteral("reject_remote_registration"),
        QStringLiteral("revoke_remote_client"),
        QStringLiteral("set_github_credential"),
        QStringLiteral("delete_github_credential"),
        QStringLiteral("import_repository_signing_key"),
    };
    return mandatory.contains(toolName) ? PermissionLevel::MandatoryConfirmation
                                        : PermissionLevel::Routine;
}

QString PermissionPolicy::confirmationMessage(const QString &toolName, const QString &target) {
    if (toolName == QStringLiteral("delete_project")) {
        return QStringLiteral("Permanently delete PacSmith project %1 and all of its releases?").arg(target);
    }
    if (toolName == QStringLiteral("delete_release")) {
        return QStringLiteral("Permanently delete PacSmith release %1?").arg(target);
    }
    if (toolName == QStringLiteral("reanalyze_release")) {
        return QStringLiteral("Reset the maintained setup for PacSmith release %1 and reanalyze its stored artifact?").arg(target);
    }
    if (toolName == QStringLiteral("configure_project_repository")) {
        return QStringLiteral("Change published repository state for PacSmith project %1?").arg(target);
    }
    if (toolName == QStringLiteral("promote_repository_package")) {
        return QStringLiteral("Promote the current package for project %1 into the published stable repository?").arg(target);
    }
    if (toolName == QStringLiteral("set_library_settings")) {
        return QStringLiteral("Change PacSmith's automatic update, preparation, and retention policy?");
    }
    if (toolName == QStringLiteral("set_client_preferences")) {
        return QStringLiteral("Change PacSmith's tray behavior and desktop-session autostart entry?");
    }
    if (toolName == QStringLiteral("set_repository_configuration")) {
        return QStringLiteral("Change global PacSmith repository listener or trust configuration?");
    }
    if (toolName == QStringLiteral("initialize_repository_signing")) {
        return QStringLiteral("Initialize signing material for the global PacSmith repository?");
    }
    if (toolName == QStringLiteral("upload_repository_root_key") ||
        toolName == QStringLiteral("upload_repository_certified_key")) {
        return QStringLiteral("Install new public-key trust material into the PacSmith repository configuration?");
    }
    if (toolName == QStringLiteral("set_remote_listening")) {
        return QStringLiteral("Change whether pacsmithd accepts authenticated remote network clients?");
    }
    if (toolName == QStringLiteral("approve_remote_registration")) {
        return QStringLiteral("Approve remote PacSmith client registration %1?").arg(target);
    }
    if (toolName == QStringLiteral("reject_remote_registration")) {
        return QStringLiteral("Reject remote PacSmith client registration %1?").arg(target);
    }
    if (toolName == QStringLiteral("revoke_remote_client")) {
        return QStringLiteral("Revoke remote PacSmith client %1?").arg(target);
    }
    if (toolName == QStringLiteral("set_github_credential")) {
        return QStringLiteral("Store or replace pacsmithd's GitHub credential in its configured secret backend?");
    }
    if (toolName == QStringLiteral("delete_github_credential")) {
        return QStringLiteral("Delete pacsmithd's stored GitHub credential?");
    }
    if (toolName == QStringLiteral("import_repository_signing_key")) {
        return QStringLiteral("Trust and pin a downloaded repository signing key for PacSmith release %1?")
            .arg(target);
    }
    return QStringLiteral("Approve sensitive PacSmith operation %1 on %2?").arg(toolName, target);
}

bool PermissionPolicy::canProceedToConfirmation(const QString &toolName,
                                                const bool elicitationSupported,
                                                QString *error) {
    if (level(toolName) == PermissionLevel::Routine || elicitationSupported) return true;
    if (error != nullptr) {
        *error = QStringLiteral("This operation requires explicit PacSmith confirmation, but the connected MCP client did not advertise form elicitation. Use a compatible client or perform the operation directly through PacSmith.");
    }
    return false;
}

} // namespace pacsmith::mcp
