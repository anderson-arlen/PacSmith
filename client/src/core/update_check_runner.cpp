#include "core/update_check_runner.hpp"

#include "core/apt_update_service.hpp"
#include "core/app_settings.hpp"
#include "core/credential_store.hpp"
#include "core/deb_download_service.hpp"
#include "core/direct_url_update_service.hpp"
#include "core/github_update_service.hpp"
#include "core/release_review.hpp"
#include "core/rpm_update_service.hpp"
#include "core/system_package_query.hpp"
#include "core/update_source.hpp"

#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QTextStream>
#include <QUrl>

#include <algorithm>
#include <filesystem>

namespace pacsmith {
namespace {

QString projectActivityName(const Project &project) {
    return project.displayName.isEmpty() ? project.archPackageName : project.displayName;
}

QString downloadStatusMessage(const QString &name, const QString &phase, const qint64 received,
                              const qint64 total) {
    if (phase == QStringLiteral("Building")) {
        return QStringLiteral("Building package %1…").arg(name);
    }
    if (!phase.isEmpty() && phase != QStringLiteral("Downloading")) {
        return QStringLiteral("Preparing %1: %2").arg(name, phase);
    }
    if (total > 0) {
        return QStringLiteral("Downloading %1 update… %2 / %3 MiB")
            .arg(name)
            .arg(received / (1024 * 1024))
            .arg(total / (1024 * 1024));
    }
    if (received > 0) {
        return QStringLiteral("Downloading %1 update… %2 MiB")
            .arg(name)
            .arg(received / (1024 * 1024));
    }
    return QStringLiteral("Downloading %1 update…").arg(name);
}

void savePreparation(BackgroundUpdateState *state, const QString &projectId,
                     const QString &projectName, const QString &phase, const qint64 received,
                     const qint64 total) {
    if (state == nullptr) return;
    BackgroundUpdateStateStore::claimActivity(*state);
    state->preparingProjectId = projectId;
    state->preparingProjectName = projectName;
    state->preparationPhase = phase;
    state->preparationBytesReceived = received;
    state->preparationBytesTotal = total;
    state->message = downloadStatusMessage(projectName, phase, received, total);
    static_cast<void>(BackgroundUpdateStateStore::save(*state));
}

void clearPreparation(BackgroundUpdateState *state) {
    if (state == nullptr) return;
    if (state->preparingProjectId.isEmpty() && state->preparingProjectName.isEmpty()) return;
    state->preparingProjectId.clear();
    state->preparingProjectName.clear();
    state->preparationPhase.clear();
    state->preparationBytesReceived = 0;
    state->preparationBytesTotal = -1;
    static_cast<void>(BackgroundUpdateStateStore::save(*state));
}

void publishCheckProgress(BackgroundUpdateState &state, const LibraryClient &library) {
    BackgroundUpdateStateStore::claimActivity(state);
    applyAvailableUpdateCensus(state, library.list());
    static_cast<void>(BackgroundUpdateStateStore::save(state));
}

bool buildPreparedUpdate(LibraryClient &library, const QString &projectId,
                         const QString &previousReleaseId, const QString &preparedReleaseId,
                         QTextStream &diagnostics, BackgroundUpdateState *activity) {
    QString error;
    auto project = library.load(projectId, &error);
    if (!project) {
        diagnostics << "warning: automatic build could not load the prepared project: "
                    << error << '\n';
        return false;
    }
    const auto *previous = project->release(previousReleaseId);
    auto *prepared = project->release(preparedReleaseId);
    if (previous == nullptr || prepared == nullptr) {
        diagnostics << "warning: automatic build could not identify the previous and prepared releases\n";
        return false;
    }
    QStringList blockers = automaticUpdateBuildBlockers(*previous, *prepared);
    if (!project->repository.publish) {
        blockers.append(QStringLiteral("Repository publishing is not enabled for this project."));
    }
    QString repoError;
    const auto repo = library.repoSettings(&repoError);
    if (!repo) {
        blockers.append(repoError.isEmpty()
                            ? QStringLiteral("Repository settings are unavailable.")
                            : QStringLiteral("Repository settings are unavailable: %1").arg(repoError));
    } else {
        if (!repo->enabled) {
            blockers.append(QStringLiteral("The PacSmith package repository is not enabled."));
        }
        if (!repo->signingInitialized) {
            blockers.append(QStringLiteral("Repository signing is not initialized."));
        }
    }
    for (const auto &dependency : prepared->dependencies) {
        const bool repositoryPackage = !dependency.ignored && !dependency.bundled &&
                                       !dependency.provided &&
                                       dependency.status != MappingStatus::Ignored &&
                                       dependency.status != MappingStatus::Bundled &&
                                       dependency.status != MappingStatus::Provided &&
                                       !dependency.archPackage.isEmpty();
        if (repositoryPackage &&
            !SystemPackageQuery::repositoryPackageAvailable(dependency.archPackage)) {
            blockers.append(QStringLiteral("Required Arch dependency %1 is unavailable.")
                                .arg(dependency.archPackage));
        }
    }
    if (!blockers.isEmpty()) {
        const auto detail = QStringLiteral("Automatic build paused: %1")
                                .arg(blockers.join(QStringLiteral("; ")));
        prepared->history.append({QDateTime::currentDateTimeUtc(),
                                  QStringLiteral("automatic-build-paused"), detail});
        QString saveError;
        if (!library.save(*project, &saveError)) {
            diagnostics << "warning: automatic build pause could not be recorded: "
                        << saveError << '\n';
        }
        diagnostics << detail << '\n';
        return false;
    }

    const auto name = projectActivityName(*project);
    savePreparation(activity, project->id, name, QStringLiteral("Building"), 0, -1);
    const auto started = library.startBuild(prepared->id, &error, true);
    if (!started || started->id.isEmpty()) {
        diagnostics << "warning: automatic build could not start: " << error << '\n';
        clearPreparation(activity);
        return false;
    }
    const auto job = library.waitForJob(started->id, &error);
    clearPreparation(activity);
    if (!job || job->status != QStringLiteral("succeeded")) {
        const auto message = !error.isEmpty() ? error
            : job && !job->error.isEmpty() ? job->error
                                           : QStringLiteral("the build failed");
        diagnostics << "warning: automatic build failed: " << message << '\n';
        return false;
    }
    diagnostics << "built " << name << " and published it to the unstable repository channel\n";
    return true;
}

UpdateCheckResult checkRelease(LibraryClient &library, const PackageRelease &release,
                               QTextStream &diagnostics, const bool forceFullContentCheck) {
    UpdateCheckResult result;
    if (release.update.strategy == UpdateStrategy::DirectUrl) {
        QEventLoop loop;
        DirectUrlUpdateService service;
        QObject::connect(&service, &DirectUrlUpdateService::progressChanged,
                         [&diagnostics](const QString &message) { diagnostics << message << '\n'; });
        QObject::connect(&service, &DirectUrlUpdateService::downloadProgress,
                         [&diagnostics](const qint64 received, const qint64 total) {
                             diagnostics << "direct artifact " << received
                                         << (total > 0 ? QStringLiteral("/%1").arg(total)
                                                       : QString{})
                                         << " bytes\r" << Qt::flush;
                         });
        QObject::connect(&service, &DirectUrlUpdateService::finished,
                         [&result, &loop](const UpdateCheckResult &checked) {
                             result = checked;
                             loop.quit();
                         });
        service.start(release, forceFullContentCheck);
        if (service.isRunning()) loop.exec();
    } else if (release.update.strategy == UpdateStrategy::AptRepository) {
        QEventLoop loop;
        AptUpdateService service;
        bool completedSynchronously = false;
        QObject::connect(&service, &AptUpdateService::progressChanged,
                         [&diagnostics](const QString &message) { diagnostics << message << '\n'; });
        QObject::connect(&service, &AptUpdateService::finished,
                         [&result, &loop, &completedSynchronously](const UpdateCheckResult &checked) {
                             result = checked;
                             completedSynchronously = true;
                             loop.quit();
                         });
        service.start(release, library.releasePath(release));
        if (service.isRunning() && !completedSynchronously) loop.exec();
    } else if (release.update.strategy == UpdateStrategy::RpmRepository) {
        QEventLoop loop;
        RpmUpdateService service;
        bool completedSynchronously = false;
        QObject::connect(&service, &RpmUpdateService::progressChanged,
                         [&diagnostics](const QString &message) { diagnostics << message << '\n'; });
        QObject::connect(&service, &RpmUpdateService::finished,
                         [&result, &loop, &completedSynchronously](const UpdateCheckResult &checked) {
                             result = checked;
                             completedSynchronously = true;
                             loop.quit();
                         });
        service.start(release, library.releasePath(release));
        if (service.isRunning() && !completedSynchronously) loop.exec();
    } else if (release.update.strategy == UpdateStrategy::GitHubRelease) {
        QEventLoop loop;
        GitHubUpdateService service;
        AppSettingsStore settingsStore;
        const auto settings = settingsStore.load();
        const auto source = settings.credentialSources.value(
            QStringLiteral("github"), CredentialSource::Environment);
        CredentialStore credentials(settingsStore.ageSecretsPath());
        auto token = credentials.load(QStringLiteral("github"), source, nullptr).value_or(QString{});
        QObject::connect(&service, &GitHubUpdateService::progressChanged,
                         [&diagnostics](const QString &message) { diagnostics << message << '\n'; });
        QObject::connect(&service, &GitHubUpdateService::finished,
                         [&result, &loop](const UpdateCheckResult &checked) {
                             result = checked;
                             loop.quit();
                         });
        service.start(release, token);
        token.fill(QChar::Null);
        if (service.isRunning()) loop.exec();
    } else {
        const auto source = UpdateSourceFactory::create(release.update.strategy);
        result = source->check(release);
    }
    return result;
}

} // namespace

std::optional<ImportResult> UpdateCheckRunner::prepareDiscovered(
    LibraryClient &library, const Project &project, const QString &releaseId,
    QTextStream &diagnostics, QString *error, BackgroundUpdateState *backgroundState) {
    const auto *discovered = project.release(releaseId);
    if (discovered == nullptr || discovered->state != ReleaseState::Discovered) {
        if (error != nullptr) *error = QStringLiteral("release is not a discovered release awaiting preparation");
        return std::nullopt;
    }
    if (discovered->sourceUrl.trimmed().isEmpty()) {
        if (error != nullptr) *error = QStringLiteral("discovered release has no download URL");
        return std::nullopt;
    }
    BackgroundUpdateState ownedActivity;
    auto *activity = backgroundState;
    if (activity == nullptr) {
        ownedActivity = BackgroundUpdateStateStore::load();
        activity = &ownedActivity;
    }
    const auto projectName = projectActivityName(project);
    savePreparation(activity, project.id, projectName, QStringLiteral("Downloading"), 0, -1);
    DebDownloadService downloader;
    QString downloadedPath;
    QString downloadError;
    QEventLoop downloadLoop;
    QObject::connect(&downloader, &DebDownloadService::finished,
                     [&downloadedPath, &downloadLoop](const QString &path) {
                         downloadedPath = path;
                         downloadLoop.quit();
                     });
    QObject::connect(&downloader, &DebDownloadService::failed,
                     [&downloadError, &downloadLoop](const QString &message) {
                         downloadError = message;
                         downloadLoop.quit();
                     });
    downloader.start(QUrl(discovered->sourceUrl), discovered->sourceSha256,
                     std::filesystem::path(defaultDownloadPath(
                         project.id, discovered->id, discovered->originalSourceFilename)
                                               .toUtf8().constData()));
    if (downloader.isRunning()) downloadLoop.exec();
    if (downloadedPath.isEmpty()) {
        clearPreparation(activity);
        if (error != nullptr) *error = downloadError.isEmpty()
            ? QStringLiteral("release download did not produce an artifact") : downloadError;
        return std::nullopt;
    }
    savePreparation(activity, project.id, projectName, QStringLiteral("Inspecting"), 0, -1);
    ImportOptions options;
    options.version = discovered->debian.version;
    options.acquisition = discovered->acquisition;
    options.githubAssetRegex = discovered->update.githubAssetRegex;
    options.githubIncludePrereleases = discovered->update.githubIncludePrereleases;
    options.existingProjectId = project.id;
    auto imported = library.importSource(
        std::filesystem::path(downloadedPath.toUtf8().constData()), options, error);
    if (!QFile::remove(downloadedPath)) diagnostics << "warning: could not remove downloaded preparation artifact\n";
    clearPreparation(activity);
    return imported;
}

UpdateCheckRunResult UpdateCheckRunner::run(LibraryClient &library, Project project,
                                            QTextStream &diagnostics,
                                            const bool forceFullContentCheck,
                                            BackgroundUpdateState *backgroundState) {
    UpdateCheckRunResult runResult;
    runResult.projectId = project.id;
    static_cast<void>(library.reconcileInstalled(project, nullptr));
    auto *release = project.activeTrackingRelease();
    if (release == nullptr) {
        runResult.status = QStringLiteral("paused");
        runResult.message = QStringLiteral("project has no analyzed release to track");
        if (backgroundState != nullptr) {
            backgroundState->message = QStringLiteral("Some projects have no eligible update tracker");
        }
        return runResult;
    }
    const auto checkedReleaseId = release->id;
    const auto result = checkRelease(library, *release, diagnostics, forceFullContentCheck);
    release->update.lastChecked = QDateTime::currentDateTimeUtc();
    release->update.lastCheckMessage = result.message;
    release->update.signatureVerified = result.signatureVerified;
    if (release->update.strategy == UpdateStrategy::DirectUrl && result.success) {
        release->update.directUrlEtag = result.directUrlEtag;
        release->update.directUrlLastModified = result.directUrlLastModified;
        release->update.directUrlContentLength = result.directUrlContentLength;
        release->update.directUrlVendorValidatorName = result.directUrlVendorValidatorName;
        release->update.directUrlVendorValidator = result.directUrlVendorValidator;
        if (!result.directUrlLastSha256.isEmpty()) {
            release->update.directUrlLastSha256 = result.directUrlLastSha256;
        }
        if (result.directUrlLastFullCheck.isValid()) {
            release->update.directUrlLastFullCheck = result.directUrlLastFullCheck;
        }
    }
    if (result.success && !result.detectedVersion.isEmpty()) {
        release->update.detectedVersion = result.detectedVersion;
        release->update.detectedFilename = result.filename;
        release->update.detectedSha256 = result.sha256;
        release->update.detectedUrl = result.downloadUrl;
        if (!result.etag.isEmpty()) release->update.githubEtag = result.etag;
        if (result.releaseId > 0) release->update.githubReleaseId = result.releaseId;
        if (result.assetId > 0) release->update.githubAssetId = result.assetId;
        if (!result.tag.isEmpty()) release->update.githubTag = result.tag;
        if (!result.publisherDigest.isEmpty()) {
            release->update.githubPublisherDigest = result.publisherDigest;
        }
    }
    project.history.append({release->update.lastChecked, QStringLiteral("update-check"), result.message});
    release->history.append({release->update.lastChecked, QStringLiteral("update-check"), result.message});
    QString saveError;
    // Discovery, preparation, and building reload project state from the library. Persist the
    // observation first so those reloads cannot discard a completed check that found no update.
    if (!library.save(project, &saveError)) {
        diagnostics << "warning: could not save update-check result: " << saveError << '\n';
    }
    release = project.release(checkedReleaseId);
    QString discoveredId;
    if (result.success && result.updateAvailable && release != nullptr) {
        if (auto *discovered = library.recordDiscoveredRelease(
                project, *release, result.detectedVersion, result.filename,
                result.sha256, result.downloadUrl, &saveError, result.releaseId,
                result.assetId, result.tag, result.publisherDigest, result.prerelease)) {
            discoveredId = discovered->id;
            if (!result.localArtifactPath.isEmpty()) {
                QString retainError;
                static_cast<void>(retainDirectUrlArtifact(
                    result, project.id, discoveredId, &retainError));
                if (!retainError.isEmpty()) {
                    diagnostics << "warning: " << retainError << '\n';
                }
            }
        } else if (!saveError.isEmpty()) {
            diagnostics << "warning: " << saveError << '\n';
            if (!result.localArtifactPath.isEmpty()) {
                static_cast<void>(QFile::remove(result.localArtifactPath));
            }
        }
    }

    QString settingsError;
    const auto settings = library.librarySettings(&settingsError);
    if (!settings && !settingsError.isEmpty()) diagnostics << "warning: " << settingsError << '\n';
    const bool automaticallyPrepare = settings && settings->automaticallyPrepare;
    if (result.success && result.updateAvailable && automaticallyPrepare &&
        !discoveredId.isEmpty()) {
        const auto *discovered = project.release(discoveredId);
        if (discovered != nullptr && discovered->state == ReleaseState::Discovered) {
            BackgroundUpdateState ownedActivity;
            auto *activity = backgroundState;
            if (activity == nullptr) {
                ownedActivity = BackgroundUpdateStateStore::load();
                activity = &ownedActivity;
            }
            const auto projectName = projectActivityName(project);
            savePreparation(activity, project.id, projectName, QStringLiteral("Downloading"), 0, -1);
            DebDownloadService downloader;
            QString downloadedPath;
            QString downloadError;
            QEventLoop downloadLoop;
            QElapsedTimer lastSave;
            lastSave.start();
            QObject::connect(&downloader, &DebDownloadService::progress,
                             [activity, &lastSave, projectId = project.id, projectName](
                                 const qint64 received, const qint64 total) {
                                 activity->preparingProjectId = projectId;
                                 activity->preparingProjectName = projectName;
                                 activity->preparationPhase = QStringLiteral("Downloading");
                                 activity->preparationBytesReceived = received;
                                 activity->preparationBytesTotal = total;
                                 activity->message = downloadStatusMessage(
                                     projectName, QStringLiteral("Downloading"), received, total);
                                 if (lastSave.elapsed() >= 150) {
                                     static_cast<void>(BackgroundUpdateStateStore::save(*activity));
                                     lastSave.restart();
                                 }
                             });
            QObject::connect(&downloader, &DebDownloadService::finished,
                             [&downloadedPath, &downloadLoop](const QString &path) {
                                 downloadedPath = path;
                                 downloadLoop.quit();
                             });
            QObject::connect(&downloader, &DebDownloadService::failed,
                             [&downloadError, &downloadLoop](const QString &message) {
                                 downloadError = message;
                                 downloadLoop.quit();
                             });
            downloader.start(QUrl(discovered->sourceUrl), discovered->sourceSha256,
                             std::filesystem::path(defaultDownloadPath(
                                 project.id, discovered->id, discovered->originalSourceFilename)
                                                       .toUtf8().constData()));
            if (downloader.isRunning()) downloadLoop.exec();
            if (!downloadedPath.isEmpty()) {
                savePreparation(activity, project.id, projectName, QStringLiteral("Inspecting"), 0, -1);
                QString importError;
                ImportOptions importOptions;
                importOptions.version = discovered->debian.version;
                importOptions.acquisition = discovered->acquisition;
                importOptions.githubAssetRegex = discovered->update.githubAssetRegex;
                importOptions.githubIncludePrereleases = discovered->update.githubIncludePrereleases;
                importOptions.existingProjectId = project.id;
                const auto imported = library.importSource(
                    std::filesystem::path(downloadedPath.toUtf8().constData()), importOptions,
                    &importError);
                static_cast<void>(QFile::remove(downloadedPath));
                if (!imported) diagnostics << "warning: automatic preparation failed: " << importError << '\n';
                else {
                    runResult.prepared = true;
                    runResult.detectedVersion = result.detectedVersion;
                }
            } else if (!downloadError.isEmpty()) {
                diagnostics << "warning: automatic download failed: " << downloadError << '\n';
            }
            clearPreparation(activity);
        }
    }
    if (automaticallyPrepare) {
        BackgroundUpdateState ownedBuildActivity;
        auto *buildActivity = backgroundState;
        if (buildActivity == nullptr) {
            ownedBuildActivity = BackgroundUpdateStateStore::load();
            buildActivity = &ownedBuildActivity;
        }
        if (const auto preparedProject = library.load(project.id, &saveError)) {
            if (const auto selection = automaticUpdateBuildSelection(*preparedProject)) {
                const auto *candidate = preparedProject->release(selection->preparedReleaseId);
                runResult.built = buildPreparedUpdate(
                    library, project.id, selection->previousReleaseId,
                    selection->preparedReleaseId, diagnostics, buildActivity);
                if (runResult.built && candidate != nullptr) {
                    runResult.detectedVersion = candidate->debian.version;
                }
            }
        } else if (!saveError.isEmpty()) {
            diagnostics << "warning: automatic build retry could not load the project: "
                        << saveError << '\n';
        }
    }
    if (auto reloaded = library.load(project.id, nullptr)) project = std::move(*reloaded);
    QString cleanupError;
    const auto cleanup = library.cleanup(&cleanupError);
    if (!cleanup.message.isEmpty()) diagnostics << cleanup.message << '\n';
    if (!cleanupError.isEmpty()) diagnostics << "warning: cleanup failed: " << cleanupError << '\n';
    if (!result.success && backgroundState != nullptr) ++backgroundState->failedChecks;
    runResult.status = !result.success ? QStringLiteral("error")
                           : result.fullContentCheckDeferred ? QStringLiteral("deferred")
                                      : result.updateAvailable ? QStringLiteral("update")
                                                               : QStringLiteral("no-update");
    runResult.message = result.message;
    runResult.detectedVersion = result.detectedVersion;
    runResult.exitCode = !result.supported ? 2 : result.success ? 0 : 1;
    return runResult;
}

UpdateCheckBatchResult UpdateCheckRunner::runAll(LibraryClient &library,
                                                 QTextStream &diagnostics) {
    UpdateCheckBatchResult batch;
    BackgroundUpdateState state;
    state.checking = true;
    state.lastRun = QDateTime::currentDateTimeUtc();
    state.message = QStringLiteral("Checking PacSmith project update trackers");
    QString listError;
    const auto projects = library.list(&listError);
    if (!listError.isEmpty()) {
        batch.error = listError;
        batch.exitCode = 1;
        state.checking = false;
        BackgroundUpdateStateStore::clearActivityOwner(state);
        state.message = QStringLiteral("Could not load PacSmith projects for update checking");
        static_cast<void>(BackgroundUpdateStateStore::save(state));
        diagnostics << "error: " << listError << '\n';
        return batch;
    }
    publishCheckProgress(state, library);
    for (auto project : projects) {
        state.checkingProjectId = project.id;
        state.checkingProjectName = projectActivityName(project);
        state.message = QStringLiteral("Checking %1 for updates").arg(state.checkingProjectName);
        publishCheckProgress(state, library);
        const auto result = run(library, project, diagnostics, false, &state);
        batch.exitCode = std::max(batch.exitCode, result.exitCode);
        batch.checks.append(result);
        publishCheckProgress(state, library);
    }
    state.checking = false;
    BackgroundUpdateStateStore::clearActivityOwner(state);
    state.checkingProjectId.clear();
    state.checkingProjectName.clear();
    state.preparingProjectId.clear();
    state.preparingProjectName.clear();
    state.preparationPhase.clear();
    state.preparationBytesReceived = 0;
    state.preparationBytesTotal = -1;
    state.lastRun = QDateTime::currentDateTimeUtc();
    applyAvailableUpdateCensus(state, library.list());
    state.message = state.availableUpdates > 0
        ? QStringLiteral("%1 update(s) available").arg(state.availableUpdates)
        : state.failedChecks > 0 ? QStringLiteral("Update checks completed with failures")
                                 : QStringLiteral("All eligible project trackers are current");
    static_cast<void>(BackgroundUpdateStateStore::save(state));
    return batch;
}

} // namespace pacsmith
