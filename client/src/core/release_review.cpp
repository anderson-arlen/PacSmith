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

QStringList dependencySurface(const PackageRelease &release) {
    QStringList result;
    result.reserve(release.dependencies.size());
    for (const auto &dependency : release.dependencies) {
        result.append(dependency.rawExpression.trimmed());
    }
    result.sort(Qt::CaseInsensitive);
    return result;
}

QStringList maintainerScriptSurface(const PackageRelease &release) {
    QStringList result;
    result.reserve(release.maintainerScripts.size());
    for (const auto &script : release.maintainerScripts) {
        result.append(script.name + QLatin1Char(':') + script.contentFingerprint());
    }
    result.sort(Qt::CaseSensitive);
    return result;
}

QString lifecycleSurface(const PackageRelease &release) {
    if (release.lifecycleScript.contents.isEmpty()) return {};
    auto sources = release.lifecycleScript.sourceFingerprints;
    sources.sort(Qt::CaseSensitive);
    return release.lifecycleScript.fileName + QLatin1Char('\n') +
           release.lifecycleScript.contentFingerprint() + QLatin1Char('\n') +
           sources.join(QLatin1Char('\n'));
}

void appendBlocker(QStringList &blockers, const QString &blocker) {
    if (!blockers.contains(blocker)) blockers.append(blocker);
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

QStringList automaticUpdateBuildBlockers(const PackageRelease &previous,
                                         const PackageRelease &next) {
    QStringList blockers;
    if (previous.buildStatus != BuildStatus::Succeeded && previous.builtArtifactIds.isEmpty()) {
        blockers.append(QStringLiteral("The previous package configuration has no successful build."));
    }
    if (!releaseReviewIssues(previous).isEmpty()) {
        blockers.append(QStringLiteral("The previous package configuration still has review issues."));
    }
    if (previous.sourceType != next.sourceType) {
        blockers.append(QStringLiteral("The vendor package format changed."));
    }
    if (dependencySurface(previous) != dependencySurface(next)) {
        blockers.append(QStringLiteral("The vendor dependency declarations changed."));
    }
    if (maintainerScriptSurface(previous) != maintainerScriptSurface(next)) {
        blockers.append(QStringLiteral("The vendor lifecycle scripts changed."));
    }
    if (lifecycleSurface(previous) != lifecycleSurface(next)) {
        blockers.append(QStringLiteral("The generated Arch lifecycle behavior changed."));
    }
    for (const auto &issue : releaseReviewIssues(next)) {
        appendBlocker(blockers, issue.summary);
    }
    return blockers;
}

std::optional<AutomaticUpdateBuildSelection>
automaticUpdateBuildSelection(const Project &project) {
    const PackageRelease *previous = nullptr;
    const PackageRelease *prepared = nullptr;
    for (const auto &release : project.releases) {
        const bool successfullyBuilt = release.buildStatus == BuildStatus::Succeeded ||
                                       !release.builtArtifactIds.isEmpty();
        if (successfullyBuilt &&
            (previous == nullptr || compareReleaseVersions(release, *previous) > 0)) {
            previous = &release;
        }
        const bool preparedState = release.state == ReleaseState::NeedsReview ||
                                   release.state == ReleaseState::Ready;
        if (preparedState && !successfullyBuilt &&
            (prepared == nullptr || compareReleaseVersions(release, *prepared) > 0)) {
            prepared = &release;
        }
    }
    if (previous == nullptr || prepared == nullptr ||
        compareReleaseVersions(*prepared, *previous) <= 0) {
        return std::nullopt;
    }
    return AutomaticUpdateBuildSelection{previous->id, prepared->id};
}

} // namespace pacsmith
