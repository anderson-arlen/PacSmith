#include "core/automatic_update_review.hpp"
#include "core/harness_launcher.hpp"
#include "core/acp_environment.hpp"
#include <QDir>
#include <QFile>
#include <QSaveFile>

namespace pacsmith {

QList<AutomaticReviewRequest> claimPendingUpdateReviews(const LibraryClient &client, const QList<Project> &summaries,
                                const AppSettings &settings, const QString &reviewDirectory, bool recoverInterrupted) {
    const auto directory = reviewDirectory.isEmpty()
        ? QDir(acpDataDirectory()).filePath(QStringLiteral("reviews")) : reviewDirectory;
    QList<AutomaticReviewRequest> requests;
    for (const auto &summary : summaries) {
        if (summary.autoBuildPolicy != AutoBuildPolicy::Ai) continue;
        for (const auto &candidate : summary.releases) {
            const auto *profile = settings.configuredHarness();
            const auto marker = profile == nullptr ? QString{} : QDir(directory).filePath(
                acpConversationKey(client.config(), *profile, summary.id, candidate.id));
            const bool recovering = recoverInterrupted && !marker.isEmpty() && QFile::exists(marker) &&
                candidate.update.lastAutomaticStatus == QStringLiteral("ai-reviewing");
            if (candidate.update.lastAutomaticStatus != QStringLiteral("ai-pending") && !recovering) continue;
            const auto project = client.load(summary.id);
            if (!project || project->autoBuildPolicy != AutoBuildPolicy::Ai) continue;
            const auto *release = project->release(candidate.id);
            if (release == nullptr || release->update.lastAutomaticStatus != (recovering ? QStringLiteral("ai-reviewing") : QStringLiteral("ai-pending")) ||
                release->buildStatus == BuildStatus::Succeeded || release->buildStatus == BuildStatus::Building) continue;
            const auto prompt = HarnessLauncher::automaticUpdatePrompt(
                project->id, release->id, release->pkgbuildManuallyModified);
            if (profile == nullptr || profile->executable.trimmed().isEmpty() ||
                profile->arguments.join(QLatin1Char(' ')).contains(QStringLiteral("{prompt}"))) {
                const auto message = QStringLiteral(
                    "Waiting for AI review: configure an ACP agent with an executable in Settings → AI Harness. Keep the PacSmith desktop session running for automatic review.");
                if (release->update.lastAutomaticMessage != message) {
                    static_cast<void>(client.setAutomaticUpdateStatus(
                        *release, QStringLiteral("ai-pending"), message));
                }
                continue;
            }
            // The revision check claims this release before opening an ACP conversation, so
            // simultaneous clients and repeated update checks cannot launch it twice.
            const auto claimed = client.setAutomaticUpdateStatus(*release,
                QStringLiteral("ai-reviewing"),
                QStringLiteral("AI review started with %1").arg(profile->name));
            if (!claimed) continue;
            QSaveFile receipt(marker);
            if (!QDir().mkpath(directory) || !receipt.open(QIODevice::WriteOnly) ||
                !receipt.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner) ||
                receipt.write("PacSmith ACP review\n") < 0 || !receipt.commit()) {
                static_cast<void>(client.setAutomaticUpdateStatus(*claimed, QStringLiteral("paused"),
                    QStringLiteral("Could not save the AI review recovery record.")));
                continue;
            }
            requests.append({client.config(), *profile, project->id, release->id,
                project->displayName.isEmpty() ? project->archPackageName : project->displayName, prompt});
        }
    }
    return requests;
}

} // namespace pacsmith
