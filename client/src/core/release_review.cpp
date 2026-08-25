#include "core/release_review.hpp"

#include "core/payload_review.hpp"

#include <QFileInfo>
#include <QSet>

#include <algorithm>

namespace pacsmith {
namespace {

void appendIssue(QList<ReleaseReviewIssue> &issues, const QString &code,
                 const QString &category, const QString &summary,
                 const QString &subject, const QString &remediation,
                 const bool blocksBuild = false) {
    issues.append({code, category, summary, subject, remediation, blocksBuild});
}

} // namespace

bool archiveDesktopCommandUnmapped(const PackageRelease &release) {
    if (release.sourceType != SourcePackageType::Archive) return false;
    QSet<QString> exposed;
    for (const auto &launcher : release.installMapping.launchers) {
        if (!launcher.enabled || launcher.missing || launcher.commandName.isEmpty()) continue;
        exposed.insert(launcher.commandName.toLower());
        if (!launcher.destination.isEmpty()) {
            exposed.insert(QFileInfo(launcher.destination).fileName().toLower());
        }
    }
    for (const auto &desktop : release.installMapping.desktopEntries) {
        if (!desktop.enabled) continue;
        const auto command = desktopEntryCommand(desktop.contents);
        if (!command.isEmpty() && !exposed.contains(command.toLower())) return true;
    }
    return false;
}

QList<ReleaseReviewIssue> releaseReviewIssues(const PackageRelease &release) {
    QList<ReleaseReviewIssue> issues;
    if (release.installMapping.appRun.requiresReview()) {
        appendIssue(issues, QStringLiteral("apprun-review-required"),
                    QStringLiteral("apprun"),
                    QStringLiteral("The extracted AppImage AppRun has not been reviewed."),
                    QStringLiteral("AppRun"),
                    QStringLiteral("Inspect the original runtime behavior, then edit or acknowledge AppRun."));
    }
    if (release.installMapping.icon.missing) {
        appendIssue(issues, QStringLiteral("icon-missing"), QStringLiteral("icon"),
                    QStringLiteral("The configured application icon is missing."),
                    release.installMapping.icon.sourcePath,
                    QStringLiteral("Select an available payload icon or provide an ordinary PacSmith icon."));
    }
    if (archiveDesktopCommandUnmapped(release)) {
        appendIssue(issues, QStringLiteral("desktop-command-unmapped"),
                    QStringLiteral("launcher"),
                    QStringLiteral("An enabled desktop entry invokes a command that PacSmith does not expose."),
                    QString{},
                    QStringLiteral("Expose or rename the matching command, or edit the desktop entry."));
    }
    for (const auto &launcher : release.installMapping.launchers) {
        if (!launcher.enabled || !launcher.missing) continue;
        appendIssue(issues, QStringLiteral("launcher-source-missing"),
                    QStringLiteral("launcher"),
                    QStringLiteral("An enabled launcher source is missing from the inspected payload."),
                    launcher.sourcePath,
                    QStringLiteral("Choose an existing source command, edit the launcher, or disable it."),
                    true);
    }
    for (const auto &desktop : release.installMapping.desktopEntries) {
        if (!desktop.enabled || !desktop.missing) continue;
        appendIssue(issues, QStringLiteral("desktop-entry-missing"),
                    QStringLiteral("desktop-entry"),
                    QStringLiteral("An enabled desktop-entry source is missing from the inspected payload."),
                    desktop.sourcePath,
                    QStringLiteral("Choose an existing desktop entry, provide visible contents, or disable it."),
                    true);
    }
    for (const auto &dependency : release.dependencies) {
        if (dependency.status != MappingStatus::Unresolved) continue;
        appendIssue(issues, QStringLiteral("dependency-unresolved"),
                    QStringLiteral("dependency"),
                    QStringLiteral("A vendor dependency has no reviewed Arch treatment."),
                    dependency.rawExpression,
                    QStringLiteral("Map it to an Arch package or explicitly mark it ignored, bundled, or provided."));
    }
    if (release.sourceType != SourcePackageType::AppImage) {
        for (const auto &entry : release.payload) {
            if (!entry.requiresReview || !PayloadReview::state(release, entry).needsReview) continue;
            appendIssue(issues, QStringLiteral("payload-decision-required"),
                        QStringLiteral("payload"),
                        entry.reviewReason.isEmpty()
                            ? QStringLiteral("A payload item needs an explicit keep/exclude decision.")
                            : entry.reviewReason,
                        entry.path,
                        QStringLiteral("Keep or exclude the current inspected payload item."));
        }
    }
    if (!release.lifecycleScript.contents.isEmpty()) {
        if (!release.lifecycleScript.validationPassed) {
            appendIssue(issues, QStringLiteral("lifecycle-script-invalid"),
                        QStringLiteral("lifecycle"),
                        release.lifecycleScript.validationMessage.isEmpty()
                            ? QStringLiteral("The Arch lifecycle script failed validation.")
                            : release.lifecycleScript.validationMessage,
                        release.lifecycleScript.fileName,
                        QStringLiteral("Repair or remove the lifecycle script before building."),
                        true);
        } else if (release.lifecycleScript.requiresAcknowledgement()) {
            appendIssue(issues, QStringLiteral("lifecycle-script-acknowledgement-required"),
                        QStringLiteral("lifecycle"),
                        QStringLiteral("The current Arch lifecycle script has not been acknowledged."),
                        release.lifecycleScript.fileName,
                        QStringLiteral("Review the current content and acknowledge its exact fingerprint."));
        }
    }
    for (const auto &finding : release.scriptFindings) {
        const auto script = std::find_if(release.maintainerScripts.cbegin(),
                                         release.maintainerScripts.cend(),
                                         [&](const auto &candidate) {
                                             return candidate.name == finding.scriptName;
                                         });
        if (script != release.maintainerScripts.cend() && !script->requiresReview()) continue;
        if (finding.disposition == ScriptDisposition::Unresolved) {
            appendIssue(issues, QStringLiteral("vendor-script-finding-unresolved"),
                        QStringLiteral("lifecycle"), finding.summary,
                        finding.scriptName,
                        QStringLiteral("Choose a supported disposition and independently verify the required Arch behavior."));
        } else if (finding.disposition == ScriptDisposition::LifecycleRequired &&
                   (!release.lifecycleScript.validationPassed ||
                    !release.lifecycleScript.sourceFingerprints.contains(
                        finding.evidenceFingerprint))) {
            appendIssue(issues, QStringLiteral("vendor-script-lifecycle-unrepresented"),
                        QStringLiteral("lifecycle"), finding.summary,
                        finding.scriptName,
                        QStringLiteral("Represent this responsibility in a valid acknowledged lifecycle script."));
        }
    }
    return issues;
}

} // namespace pacsmith
