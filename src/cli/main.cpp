#include "core/apt_update_service.hpp"
#include "core/ai_service.hpp"
#include "core/app_settings.hpp"
#include "core/background_updates.hpp"
#include "core/credential_store.hpp"
#include "core/deb_download_service.hpp"
#include "core/github_update_service.hpp"
#include "core/managed_package.hpp"
#include "core/payload_inspector.hpp"
#include "core/payload_review.hpp"
#include "core/package_artifact.hpp"
#include "core/pkgbuild_generator.hpp"
#include "core/process_services.hpp"
#include "core/project_store.hpp"
#include "core/repository_key_download_service.hpp"
#include "core/repository_trust.hpp"
#include "core/rpm_update_service.hpp"
#include "core/terminal_install_service.hpp"
#include "core/update_source.hpp"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonObject>
#include <QLocalSocket>
#include <QProcess>
#include <QRegularExpression>
#include <QTextStream>
#include <QSet>
#include <QTimer>
#include <QTemporaryDir>
#include <QUrl>

#include <algorithm>
#include <filesystem>
#include <termios.h>
#include <unistd.h>

namespace {

void printUsage(QTextStream &stream) {
    stream << "PacSmith - vendor artifact to local Arch package workbench\n\n"
              "Usage:\n"
              "  pacsmith add <artifact>\n"
              "  pacsmith add <github-url> [--asset-regex <regex>] [--prerelease]\n"
              "  pacsmith add apt <repo-url> <suite> <component|-> <architecture> <package> <key-url> [--trust-fingerprint <fingerprint>]\n"
              "  pacsmith add rpm <repo-url> <architecture> <package> <key-url> [--trust-fingerprint <fingerprint>]\n"
              "  pacsmith list\n"
              "  pacsmith versions <project>\n"
              "  pacsmith info <project>\n"
              "  pacsmith dependencies <project>\n"
              "  pacsmith scripts <project> [--acknowledge <script>]\n"
              "  pacsmith lifecycle <project> [--acknowledge <sha256>|--discard]\n"
              "  pacsmith payload <project> [--show <path>]\n"
              "  pacsmith pkgbuild <project>\n"
              "  pacsmith build <project>\n"
              "  pacsmith install <project> [package.pkg.tar.zst]\n"
              "  pacsmith rollback <project> <release-id|version>\n"
              "  pacsmith uninstall <project>\n"
              "  pacsmith check <project>|--all\n"
              "  pacsmith ai status\n"
              "  pacsmith ai resolve <project>\n"
              "  pacsmith gui\n";
}

std::optional<pacsmith::Project> requireProject(const pacsmith::ProjectStore &store, const QString &name,
                                                QTextStream &errorStream) {
    QString error;
    auto project = store.load(name, &error);
    if (!project) errorStream << "error: " << error << '\n';
    return project;
}

QString scriptFriendly(const QString &value) {
    QString result = value;
    result.replace(QLatin1Char('\n'), QStringLiteral("\\n"));
    result.replace(QLatin1Char('\t'), QStringLiteral("\\t"));
    return result;
}

QString readPassword(QTextStream &errorStream, const QString &prompt) {
    errorStream << prompt << Qt::flush;
    termios previous{};
    const bool terminal = ::isatty(STDIN_FILENO) != 0 && ::tcgetattr(STDIN_FILENO, &previous) == 0;
    if (terminal) {
        auto hidden = previous;
        hidden.c_lflag &= static_cast<tcflag_t>(~ECHO);
        static_cast<void>(::tcsetattr(STDIN_FILENO, TCSAFLUSH, &hidden));
    }
    QTextStream input(stdin);
    input.setEncoding(QStringConverter::Utf8);
    auto value = input.readLine();
    if (terminal) {
        static_cast<void>(::tcsetattr(STDIN_FILENO, TCSAFLUSH, &previous));
        errorStream << '\n';
    }
    return value;
}

bool askYesNo(QTextStream &errorStream, const QString &prompt) {
    if (::isatty(STDIN_FILENO) == 0) return false;
    errorStream << prompt << " [y/N] " << Qt::flush;
    QTextStream input(stdin);
    const auto answer = input.readLine().trimmed().toLower();
    return answer == QStringLiteral("y") || answer == QStringLiteral("yes");
}

int runRepositoryAdd(const QStringList &arguments, pacsmith::ProjectStore &store,
                     QTextStream &out, QTextStream &errorStream) {
    const bool rpm = arguments.value(2) == QStringLiteral("rpm");
    const int required = rpm ? 7 : 9;
    if (arguments.size() != required && arguments.size() != required + 2) {
        errorStream << (rpm
            ? "error: add rpm requires <repo-url> <architecture> <package> <key-url> [--trust-fingerprint <fingerprint>]\n"
            : "error: add apt requires <repo-url> <suite> <component|-> <architecture> <package> <key-url> [--trust-fingerprint <fingerprint>]\n");
        return 1;
    }
    QString trustedFingerprint;
    if (arguments.size() == required + 2) {
        if (arguments.at(required) != QStringLiteral("--trust-fingerprint")) {
            errorStream << "error: expected --trust-fingerprint as the final option\n";
            return 1;
        }
        trustedFingerprint = arguments.at(required + 1).trimmed().toUpper();
    }

    pacsmith::UpdateConfiguration update;
    update.strategy = rpm ? pacsmith::UpdateStrategy::RpmRepository
                          : pacsmith::UpdateStrategy::AptRepository;
    update.url = arguments.at(3);
    QString packageName;
    QString keyText;
    if (rpm) {
        update.rpmArchitecture = arguments.at(4);
        update.rpmPackageName = arguments.at(5);
        packageName = update.rpmPackageName;
        keyText = arguments.at(6);
    } else {
        update.aptSuite = arguments.at(4);
        update.aptComponent = arguments.at(5) == QStringLiteral("-")
            ? QString{} : arguments.at(5);
        update.aptArchitecture = arguments.at(6);
        update.aptPackageName = arguments.at(7);
        packageName = update.aptPackageName;
        keyText = arguments.at(8);
    }
    const QUrl repositoryUrl(update.url, QUrl::StrictMode);
    if (!repositoryUrl.isValid() || repositoryUrl.scheme() != QStringLiteral("https") ||
        repositoryUrl.host().isEmpty() || !repositoryUrl.userInfo().isEmpty() ||
        repositoryUrl.hasQuery() || repositoryUrl.hasFragment()) {
        errorStream << "error: repository URL must be an HTTPS base URL without credentials, query, or fragment\n";
        return 1;
    }
    const QUrl keyUrl(keyText, QUrl::StrictMode);
    if (!pacsmith::isAcceptableRepositoryKeyUrl(keyUrl)) {
        errorStream << "error: signing-key URL must be a valid HTTPS URL without credentials or a fragment\n";
        return 1;
    }

    pacsmith::RepositoryKeyDownloadService keyDownloader;
    QByteArray keyContents;
    QString keyError;
    QEventLoop keyLoop;
    QObject::connect(&keyDownloader, &pacsmith::RepositoryKeyDownloadService::progress,
                     [&errorStream](const qint64 received, const qint64 total) {
        errorStream << "signing key " << received
                    << (total > 0 ? QStringLiteral("/%1").arg(total) : QString{})
                    << " bytes\r" << Qt::flush;
    });
    QObject::connect(&keyDownloader, &pacsmith::RepositoryKeyDownloadService::finished,
                     [&keyContents, &keyLoop](const QByteArray &contents, const QUrl &,
                                               const QUrl &) {
        keyContents = contents;
        keyLoop.quit();
    });
    QObject::connect(&keyDownloader, &pacsmith::RepositoryKeyDownloadService::failed,
                     [&keyError, &keyLoop](const QString &message) {
        keyError = message;
        keyLoop.quit();
    });
    keyDownloader.start(keyUrl);
    if (keyDownloader.isRunning()) keyLoop.exec();
    errorStream << '\n';
    if (keyContents.isEmpty()) {
        errorStream << "error: " << keyError << '\n';
        return 1;
    }
    QString inspectionError;
    const auto inspection = pacsmith::RepositoryTrust::inspectKey(keyContents,
                                                                  &inspectionError);
    if (!inspection) {
        errorStream << "error: " << inspectionError << '\n';
        return 1;
    }
    errorStream << "repository signing-key SHA256: " << inspection->sha256 << '\n';
    for (const auto &fingerprint : inspection->fingerprints) {
        errorStream << "OpenPGP fingerprint: " << fingerprint << '\n';
    }
    const auto fingerprintMatches = [&](const QString &fingerprint) {
        return std::any_of(inspection->fingerprints.cbegin(),
                           inspection->fingerprints.cend(), [&](const auto &candidate) {
            return candidate.compare(fingerprint, Qt::CaseInsensitive) == 0;
        });
    };
    if (!trustedFingerprint.isEmpty() && !fingerprintMatches(trustedFingerprint)) {
        errorStream << "error: downloaded key does not contain the fingerprint supplied with --trust-fingerprint\n";
        return 1;
    }
    if (trustedFingerprint.isEmpty() &&
        !askYesNo(errorStream,
                  QStringLiteral("Trust this key and query %1?").arg(update.url))) {
        errorStream << "error: repository key was not trusted; non-interactive use requires --trust-fingerprint\n";
        return 1;
    }

    QTemporaryDir verificationDirectory;
    if (!verificationDirectory.isValid()) {
        errorStream << "error: could not create a temporary verification directory\n";
        return 1;
    }
    QString importKeyError;
    const auto key = pacsmith::RepositoryTrust::importUserKey(
        std::filesystem::path(verificationDirectory.path().toUtf8().constData()),
        keyContents, keyUrl.toString(), &importKeyError);
    if (!key || key->fingerprints.isEmpty()) {
        errorStream << "error: " << importKeyError << '\n';
        return 1;
    }
    update.signingKeys = {*key};
    update.aptSigningKeyring = key->relativePath;
    update.trustedSigningFingerprint = trustedFingerprint.isEmpty()
        ? key->fingerprints.first() : trustedFingerprint;
    if (rpm) {
        update.rpmCandidates.append(
            {update.url, update.rpmArchitecture, {keyUrl.toString()},
             QStringLiteral("command-line repository import")});
    } else {
        update.aptCandidates.append(
            {update.url, update.aptSuite,
             update.aptComponent.isEmpty() ? QStringList{} : QStringList{update.aptComponent},
             {update.aptArchitecture}, {}, QStringLiteral("command-line repository import")});
    }

    pacsmith::PackageRelease probe;
    probe.debian.package = packageName;
    probe.debian.version = QStringLiteral("0");
    probe.debian.architecture = rpm ? update.rpmArchitecture : update.aptArchitecture;
    probe.update = update;
    pacsmith::UpdateCheckResult checked;
    QEventLoop repositoryLoop;
    if (rpm) {
        pacsmith::RpmUpdateService service;
        QObject::connect(&service, &pacsmith::RpmUpdateService::progressChanged,
                         [&errorStream](const QString &message) {
            errorStream << message << '\n';
        });
        QObject::connect(&service, &pacsmith::RpmUpdateService::finished,
                         [&checked, &repositoryLoop](const pacsmith::UpdateCheckResult &result) {
            checked = result;
            repositoryLoop.quit();
        });
        service.start(probe, std::filesystem::path(
                                 verificationDirectory.path().toUtf8().constData()));
        if (service.isRunning()) repositoryLoop.exec();
    } else {
        pacsmith::AptUpdateService service;
        QObject::connect(&service, &pacsmith::AptUpdateService::progressChanged,
                         [&errorStream](const QString &message) {
            errorStream << message << '\n';
        });
        QObject::connect(&service, &pacsmith::AptUpdateService::finished,
                         [&checked, &repositoryLoop](const pacsmith::UpdateCheckResult &result) {
            checked = result;
            repositoryLoop.quit();
        });
        service.start(probe, std::filesystem::path(
                                 verificationDirectory.path().toUtf8().constData()));
        if (service.isRunning()) repositoryLoop.exec();
    }
    if (!checked.success || !checked.signatureVerified || checked.downloadUrl.isEmpty() ||
        checked.sha256.isEmpty()) {
        errorStream << "error: " << checked.message << '\n';
        return 1;
    }

    update.detectedVersion = checked.detectedVersion;
    update.detectedFilename = checked.filename;
    update.detectedSha256 = checked.sha256;
    update.detectedUrl = checked.downloadUrl;
    update.lastChecked = QDateTime::currentDateTimeUtc();
    update.lastCheckMessage = checked.message;
    update.signatureVerified = true;
    const auto filename = QFileInfo(QUrl(checked.downloadUrl).path()).fileName().isEmpty()
        ? QFileInfo(checked.filename).fileName()
        : QFileInfo(QUrl(checked.downloadUrl).path()).fileName();
    const auto target = pacsmith::defaultDownloadPath(
        pacsmith::PkgbuildGenerator::sanitizePackageName(packageName),
        pacsmith::PkgbuildGenerator::sanitizePackageName(checked.detectedVersion), filename);
    pacsmith::DebDownloadService artifactDownloader;
    QString downloadedPath;
    QString downloadError;
    QEventLoop downloadLoop;
    QObject::connect(&artifactDownloader, &pacsmith::DebDownloadService::progress,
                     [&errorStream](const qint64 received, const qint64 total) {
        errorStream << "artifact " << received
                    << (total > 0 ? QStringLiteral("/%1").arg(total) : QString{})
                    << " bytes\r" << Qt::flush;
    });
    QObject::connect(&artifactDownloader, &pacsmith::DebDownloadService::finished,
                     [&downloadedPath, &downloadLoop](const QString &path) {
        downloadedPath = path;
        downloadLoop.quit();
    });
    QObject::connect(&artifactDownloader, &pacsmith::DebDownloadService::failed,
                     [&downloadError, &downloadLoop](const QString &message) {
        downloadError = message;
        downloadLoop.quit();
    });
    artifactDownloader.start(QUrl(checked.downloadUrl), checked.sha256,
                             std::filesystem::path(target.toUtf8().constData()));
    if (artifactDownloader.isRunning()) downloadLoop.exec();
    errorStream << '\n';
    if (downloadedPath.isEmpty()) {
        errorStream << "error: " << downloadError << '\n';
        return 1;
    }

    pacsmith::ImportOptions options;
    options.version = checked.detectedVersion;
    options.initialUpdate = update;
    options.trustedSigningKey = keyContents;
    options.trustedSigningKeySource = keyUrl.toString();
    options.acquisition.kind = rpm ? pacsmith::AcquisitionKind::RpmRepository
                                   : pacsmith::AcquisitionKind::AptRepository;
    options.acquisition.canonicalIdentity = rpm
        ? QStringLiteral("rpm:%1:%2:%3")
              .arg(update.url.toLower(), update.rpmArchitecture.toLower(),
                   update.rpmPackageName.toLower())
        : QStringLiteral("apt:%1:%2:%3:%4:%5")
              .arg(update.url.toLower(), update.aptSuite.toLower(),
                   update.aptComponent.toLower(), update.aptArchitecture.toLower(),
                   update.aptPackageName.toLower());
    options.acquisition.originalUrl = checked.downloadUrl;
    options.acquisition.publisherDigest = checked.sha256;
    options.acquisition.publisherVerified = true;
    QString importError;
    const auto imported = store.importSource(
        std::filesystem::path(downloadedPath.toUtf8().constData()), options, &importError);
    static_cast<void>(QFile::remove(downloadedPath));
    if (!imported) {
        errorStream << "error: " << importError << '\n';
        return 1;
    }
    out << QString::fromUtf8(
               store.releasePath(imported->project.id, imported->releaseId).string().c_str())
        << '\n';
    return 0;
}

bool automaticallyResolvePreparedRelease(pacsmith::ProjectStore &store,
                                         pacsmith::Project &project,
                                         const QString &releaseId,
                                         const pacsmith::AiSettings &settings,
                                         QString *message) {
    if (!settings.automaticallyResolveReviewItems ||
        settings.provider == pacsmith::AiProviderKind::None || settings.model.isEmpty()) {
        if (message != nullptr) *message = QStringLiteral("Automatic AI review is disabled or incomplete");
        return false;
    }
    auto *release = project.release(releaseId);
    if (release == nullptr || release->state == pacsmith::ReleaseState::Discovered) return false;
    const auto provider = pacsmith::aiProviderName(settings.provider);
    const auto defaultSource = settings.provider == pacsmith::AiProviderKind::ChatGpt
        ? pacsmith::CredentialSource::Keyring : pacsmith::CredentialSource::Environment;
    const auto source = settings.credentialSources.value(provider, defaultSource);
    if (source == pacsmith::CredentialSource::Age) {
        if (message != nullptr) {
            *message = QStringLiteral("Automatic AI review is pending because the age credential store requires an interactive password");
        }
        return false;
    }
    pacsmith::AppSettingsStore settingsStore;
    pacsmith::CredentialStore credentials(settingsStore.ageSecretsPath());
    QString credentialError;
    const auto credential = credentials.load(provider, source, &credentialError);
    if (!credential) {
        if (message != nullptr) *message = QStringLiteral("Automatic AI review is pending: %1").arg(credentialError);
        return false;
    }
    pacsmith::AiAnalysisService service;
    pacsmith::AiResolution resolution;
    QEventLoop loop;
    QObject::connect(&service, &pacsmith::AiAnalysisService::credentialUpdated,
                     [&credentials, source, provider](const QString &serialized) {
        static_cast<void>(credentials.store(provider, source, serialized, {}, nullptr));
    });
    QObject::connect(&service, &pacsmith::AiAnalysisService::finished,
                     [&resolution, &loop](const pacsmith::AiResolution &result) {
        resolution = result;
        loop.quit();
    });
    service.start(*release, settings, *credential);
    if (service.isRunning()) loop.exec();
    if (!resolution.success) {
        if (message != nullptr) *message = QStringLiteral("Automatic AI review is pending: %1").arg(resolution.error);
        return false;
    }
    const auto applied = pacsmith::AiResolutionApplier::apply(*release, resolution, {});
    if (!release->lifecycleScript.contents.isEmpty()) {
        QString error;
        if (!store.saveLifecycle(project, *release, &error)) {
            if (message != nullptr) *message = error;
            return false;
        }
    }
    if (release->update.strategy == pacsmith::UpdateStrategy::Manual &&
        !release->update.url.isEmpty() && !release->update.aptSuite.isEmpty()) {
        release->update.strategy = pacsmith::UpdateStrategy::AptRepository;
    }
    release->generatedPkgbuild = pacsmith::PkgbuildGenerator::generate(*release);
    release->generatedPkgbuildSha256 = pacsmith::sha256Hex(release->generatedPkgbuild.toUtf8());
    QString saveError;
    const auto saved = release->pkgbuildManuallyModified
        ? store.save(project, &saveError)
        : store.savePkgbuild(project, *release, release->generatedPkgbuild, &saveError);
    if (!saved) {
        if (message != nullptr) *message = saveError;
        return false;
    }
    if (message != nullptr) {
        *message = applied.errors.isEmpty()
            ? QStringLiteral("Automatic AI review applied")
            : QStringLiteral("Automatic AI review applied with %1 blocked proposal(s)").arg(applied.errors.size());
    }
    return applied.errors.isEmpty();
}

int runCheck(pacsmith::ProjectStore &store, pacsmith::Project project, QTextStream &out,
             QTextStream &errorStream, pacsmith::BackgroundUpdateState *backgroundState = nullptr) {
    static_cast<void>(store.reconcileInstalled(project, nullptr));
    auto *release = project.activeTrackingRelease();
    if (release == nullptr) {
        const auto reason = project.externallyInstalled || !project.installedVersion.isEmpty()
            ? QStringLiteral("installed package does not match a PacSmith release")
            : QStringLiteral("project has no analyzed release to track");
        out << project.id << "\tpaused\t" << reason << '\n';
        if (backgroundState != nullptr) {
            backgroundState->message = QStringLiteral("Some projects have no eligible update tracker");
        }
        return 0;
    }
    pacsmith::UpdateCheckResult result;
    if (release->update.strategy == pacsmith::UpdateStrategy::AptRepository) {
        QEventLoop loop;
        pacsmith::AptUpdateService service;
        bool completedSynchronously = false;
        QObject::connect(&service, &pacsmith::AptUpdateService::progressChanged,
                         [&errorStream](const QString &message) { errorStream << message << '\n'; });
        QObject::connect(&service, &pacsmith::AptUpdateService::finished,
                         [&result, &loop, &completedSynchronously](const pacsmith::UpdateCheckResult &checked) {
                             result = checked;
                             completedSynchronously = true;
                             loop.quit();
                         });
        service.start(*release, store.releasePath(*release));
        if (service.isRunning() && !completedSynchronously) loop.exec();
    } else if (release->update.strategy == pacsmith::UpdateStrategy::RpmRepository) {
        QEventLoop loop;
        pacsmith::RpmUpdateService service;
        bool completedSynchronously = false;
        QObject::connect(&service, &pacsmith::RpmUpdateService::progressChanged,
                         [&errorStream](const QString &message) { errorStream << message << '\n'; });
        QObject::connect(&service, &pacsmith::RpmUpdateService::finished,
                         [&result, &loop, &completedSynchronously](const pacsmith::UpdateCheckResult &checked) {
                             result = checked;
                             completedSynchronously = true;
                             loop.quit();
                         });
        service.start(*release, store.releasePath(*release));
        if (service.isRunning() && !completedSynchronously) loop.exec();
    } else if (release->update.strategy == pacsmith::UpdateStrategy::GitHubRelease) {
        QEventLoop loop;
        pacsmith::GitHubUpdateService service;
        QString token;
        pacsmith::AppSettingsStore settingsStore;
        const auto settings = settingsStore.load();
        const auto source = settings.credentialSources.value(
            QStringLiteral("github"), pacsmith::CredentialSource::Environment);
        if (source != pacsmith::CredentialSource::Age) {
            pacsmith::CredentialStore credentials(settingsStore.ageSecretsPath());
            token = credentials.load(QStringLiteral("github"), source, nullptr).value_or(QString{});
        }
        QObject::connect(&service, &pacsmith::GitHubUpdateService::progressChanged,
                         [&errorStream](const QString &message) { errorStream << message << '\n'; });
        QObject::connect(&service, &pacsmith::GitHubUpdateService::finished,
                         [&result, &loop](const pacsmith::UpdateCheckResult &checked) {
                             result = checked;
                             loop.quit();
                         });
        service.start(*release, token);
        token.fill(QChar::Null);
        if (service.isRunning()) loop.exec();
    } else {
        const auto source = pacsmith::UpdateSourceFactory::create(release->update.strategy);
        result = source->check(*release);
    }
    release->update.lastChecked = QDateTime::currentDateTimeUtc();
    release->update.lastCheckMessage = result.message;
    release->update.signatureVerified = result.signatureVerified;
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
    QString discoveredId;
    if (result.success && result.updateAvailable) {
        if (auto *discovered = store.recordDiscoveredRelease(
                project, *release, result.detectedVersion, result.filename,
                result.sha256, result.downloadUrl, &saveError, result.releaseId,
                result.assetId, result.tag, result.publisherDigest, result.prerelease)) {
            discoveredId = discovered->id;
        } else if (!saveError.isEmpty()) {
            errorStream << "warning: " << saveError << '\n';
        }
        if (backgroundState != nullptr) {
            ++backgroundState->availableUpdates;
            backgroundState->projectsWithUpdates.append(project.id);
        }
    }
    pacsmith::AppSettingsStore settingsStore;
    const auto settings = settingsStore.load();
    if (result.success && result.updateAvailable && settings.updates.automaticallyPrepare &&
        !discoveredId.isEmpty()) {
        const auto *discovered = project.release(discoveredId);
        if (discovered != nullptr) {
            pacsmith::DebDownloadService downloader;
            QString downloadedPath;
            QString downloadError;
            QEventLoop downloadLoop;
            QObject::connect(&downloader, &pacsmith::DebDownloadService::finished,
                             [&downloadedPath, &downloadLoop](const QString &path) {
                                 downloadedPath = path;
                                 downloadLoop.quit();
                             });
            QObject::connect(&downloader, &pacsmith::DebDownloadService::failed,
                             [&downloadError, &downloadLoop](const QString &message) {
                                 downloadError = message;
                                 downloadLoop.quit();
                             });
            downloader.start(QUrl(discovered->sourceUrl), discovered->sourceSha256,
                             std::filesystem::path(
                                 pacsmith::defaultDownloadPath(project.id, discovered->id,
                                                               discovered->originalSourceFilename)
                                     .toUtf8().constData()));
            if (downloader.isRunning()) downloadLoop.exec();
            if (!downloadedPath.isEmpty()) {
                QString importError;
                pacsmith::ImportOptions importOptions;
                importOptions.version = discovered->debian.version;
                importOptions.acquisition = discovered->acquisition;
                importOptions.githubAssetRegex = discovered->update.githubAssetRegex;
                importOptions.githubIncludePrereleases =
                    discovered->update.githubIncludePrereleases;
                const auto imported = store.importSource(
                    std::filesystem::path(downloadedPath.toUtf8().constData()), importOptions,
                    &importError);
                static_cast<void>(QFile::remove(downloadedPath));
                if (!imported) errorStream << "warning: automatic preparation failed: " << importError << '\n';
                else {
                    auto preparedProject = imported->project;
                    QString aiMessage;
                    static_cast<void>(automaticallyResolvePreparedRelease(
                        store, preparedProject, imported->releaseId, settings, &aiMessage));
                    if (!aiMessage.isEmpty()) errorStream << aiMessage << '\n';
                    out << project.id << "\tprepared\t" << result.detectedVersion << '\n';
                }
            } else if (!downloadError.isEmpty()) {
                errorStream << "warning: automatic download failed: " << downloadError << '\n';
            }
        }
    }
    if (auto reloaded = store.load(project.id, nullptr)) project = std::move(*reloaded);
    const auto cleanup = store.cleanup(
        project, {settings.updates.retainedPackageVersions,
                  settings.updates.retainedCompleteReleases}, &saveError);
    if (!cleanup.message.isEmpty()) errorStream << cleanup.message << '\n';
    if (!store.save(project, &saveError)) errorStream << "warning: " << saveError << '\n';
    if (!result.success && backgroundState != nullptr) ++backgroundState->failedChecks;
    out << project.id << '\t' << (!result.success ? "error" : result.updateAvailable ? "update" : "no-update") << '\t'
        << result.message << '\n';
    return !result.supported ? 2 : result.success ? 0 : 1;
}

int runInstallSession(QCoreApplication &application, const QStringList &arguments,
                      QTextStream &out, QTextStream &errorStream) {
    if (arguments.size() != 8 || arguments.at(2) != QStringLiteral("--socket") ||
        arguments.at(4) != QStringLiteral("--token") ||
        arguments.at(6) != QStringLiteral("--package")) {
        errorStream << "error: invalid internal installation session\n";
        return 2;
    }
    const auto socketName = arguments.at(3);
    const auto token = arguments.at(5);
    const auto operation = arguments.at(6);
    const QFileInfo package(arguments.at(7));
    static const QRegularExpression validToken(QStringLiteral("^[0-9a-f]{64}$"));
    if (socketName.isEmpty() || socketName.size() > 200 ||
        !validToken.match(token).hasMatch() ||
        (operation != QStringLiteral("--package") && operation != QStringLiteral("--remove"))) {
        errorStream << "error: invalid internal installation session parameters\n";
        return 2;
    }
    static const QRegularExpression safePackageName(QStringLiteral("^[a-z0-9][a-z0-9@._+\\-]*$"));
    if ((operation == QStringLiteral("--package") &&
         (!package.isAbsolute() || !package.exists() || !package.isFile() ||
          !package.fileName().contains(QStringLiteral(".pkg.tar.")) ||
          package.fileName().endsWith(QStringLiteral(".sig")))) ||
        (operation == QStringLiteral("--remove") &&
         !safePackageName.match(arguments.at(7)).hasMatch())) {
        errorStream << "error: invalid internal package operation parameters\n";
        return 2;
    }

    QLocalSocket socket;
    socket.connectToServer(socketName, QIODevice::ReadWrite);
    if (!socket.waitForConnected(10000)) {
        errorStream << "error: could not connect to PacSmith: " << socket.errorString() << '\n';
        return 2;
    }
    bool channelAvailable = true;
    const auto sendEvent = [&socket, &channelAvailable](const pacsmith::InstallSessionEvent &event) {
        if (!channelAvailable || socket.state() != QLocalSocket::ConnectedState) return false;
        const auto message = pacsmith::InstallSessionProtocol::encode(event);
        if (socket.write(message) != message.size() || !socket.waitForBytesWritten(5000)) {
            channelAvailable = false;
            return false;
        }
        return true;
    };
    if (!sendEvent({QStringLiteral("started"), token, {}})) {
        errorStream << "error: could not authenticate the PacSmith installation session\n";
        return 2;
    }

    pacsmith::InstallService service;
    int exitCode = 1;
    bool completed = false;
    bool eventLoopRunning = false;
    const auto finishSession = [&](const pacsmith::ProcessResult &result) {
        if (completed) return;
        completed = true;
        exitCode = result.succeeded() ? 0 : 1;
        static_cast<void>(sendEvent({QStringLiteral("finished"), token, {}, result.exitCode,
                                     result.exitStatus, result.canceled}));
        if (socket.state() == QLocalSocket::ConnectedState) {
            socket.disconnectFromServer();
            if (socket.state() != QLocalSocket::UnconnectedState) socket.waitForDisconnected(2000);
        }
        out << '\n'
            << (result.succeeded() ? "PacSmith: package installation completed successfully.\n"
                                   : "PacSmith: package installation did not complete successfully.\n");
        if (!channelAvailable) {
            out << "PacSmith: the GUI connection was lost; reopen the project to refresh its status.\n";
        }
        out << "Press Enter to close this terminal." << Qt::flush;
        if (::isatty(STDIN_FILENO) != 0) {
            QTextStream input(stdin);
            static_cast<void>(input.readLine());
        } else {
            out << '\n';
        }
        if (eventLoopRunning) application.quit();
    };
    QObject::connect(&service, &pacsmith::InstallService::outputAvailable, &application,
                     [&](const QString &text) {
                         out << text << Qt::flush;
                         static_cast<void>(sendEvent({QStringLiteral("output"), token, text}));
                     });
    QObject::connect(&service, &pacsmith::InstallService::failedToStart, &application,
                     [&](const QString &message) {
                         errorStream << "error: " << message << '\n' << Qt::flush;
                         static_cast<void>(sendEvent({QStringLiteral("output"), token,
                                                      QStringLiteral("error: %1\n").arg(message)}));
                         pacsmith::ProcessResult result;
                         result.exitStatus = QProcess::CrashExit;
                         result.finishedAt = QDateTime::currentDateTimeUtc();
                         finishSession(result);
                     });
    QObject::connect(&service, &pacsmith::InstallService::finished, &application,
                     finishSession);
    if (operation == QStringLiteral("--remove")) service.startUninstall(arguments.at(7));
    else service.start(std::filesystem::path(package.absoluteFilePath().toUtf8().constData()));
    if (!completed && service.isRunning()) {
        eventLoopRunning = true;
        application.exec();
    }
    return exitCode;
}

} // namespace

int main(int argc, char *argv[]) {
    QCoreApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("pacsmith"));
    QCoreApplication::setApplicationVersion(QStringLiteral(PACSMITH_VERSION));
    QTextStream out(stdout);
    QTextStream errorStream(stderr);
    out.setEncoding(QStringConverter::Utf8);
    errorStream.setEncoding(QStringConverter::Utf8);

    const auto arguments = application.arguments();
    if (arguments.size() < 2 || arguments.at(1) == QStringLiteral("help") ||
        arguments.at(1) == QStringLiteral("--help") || arguments.at(1) == QStringLiteral("-h")) {
        printUsage(arguments.size() < 2 ? errorStream : out);
        return arguments.size() < 2 ? 1 : 0;
    }

    const auto command = arguments.at(1);
    if (command == QStringLiteral("_install-session")) {
        return runInstallSession(application, arguments, out, errorStream);
    }
    pacsmith::ProjectStore store;
    if (command == QStringLiteral("add")) {
        if (arguments.size() < 3) {
            errorStream << "error: add requires a supported artifact path or GitHub URL\n";
            return 1;
        }
        if (arguments.at(2) == QStringLiteral("apt") ||
            arguments.at(2) == QStringLiteral("rpm")) {
            return runRepositoryAdd(arguments, store, out, errorStream);
        }
        const QUrl remote(arguments.at(2));
        const bool github = remote.isValid() && remote.scheme() == QStringLiteral("https") &&
                            remote.host().compare(QStringLiteral("github.com"), Qt::CaseInsensitive) == 0;
        if (github) {
            const auto parts = remote.path().split(QLatin1Char('/'), Qt::SkipEmptyParts);
            if (parts.size() < 2) {
                errorStream << "error: expected a GitHub repository, release, or asset URL\n";
                return 1;
            }
            QString assetRegex;
            QString requestedTag;
            bool prerelease = false;
            for (int index = 3; index < arguments.size(); ++index) {
                if (arguments.at(index) == QStringLiteral("--prerelease")) {
                    prerelease = true;
                } else if (arguments.at(index) == QStringLiteral("--asset-regex") &&
                           index + 1 < arguments.size()) {
                    assetRegex = arguments.at(++index);
                } else {
                    errorStream << "error: unknown or incomplete add option: " << arguments.at(index) << '\n';
                    return 1;
                }
            }
            if (parts.size() >= 6 && parts.at(2) == QStringLiteral("releases") &&
                parts.at(3) == QStringLiteral("download")) {
                requestedTag = parts.at(4);
                if (assetRegex.isEmpty()) {
                    assetRegex = QRegularExpression::escape(parts.mid(5).join(QLatin1Char('/')));
                }
            } else if (parts.size() >= 5 && parts.at(2) == QStringLiteral("releases") &&
                       parts.at(3) == QStringLiteral("tag")) {
                requestedTag = parts.mid(4).join(QLatin1Char('/'));
            }
            auto repositoryName = parts.at(1);
            if (repositoryName.endsWith(QStringLiteral(".git"))) repositoryName.chop(4);
            if (assetRegex.isEmpty() &&
                parts.at(0).compare(QStringLiteral("anderson-arlen"), Qt::CaseInsensitive) == 0 &&
                repositoryName.compare(QStringLiteral("pacsmith"), Qt::CaseInsensitive) == 0) {
                assetRegex = QStringLiteral(
                    R"(pacsmith-[0-9][A-Za-z0-9._+-]*-[0-9]+-x86_64\.pkg\.tar\.zst)");
            }
            const QRegularExpression expression(assetRegex);
            if (assetRegex.isEmpty() || !expression.isValid()) {
                errorStream << "error: GitHub imports require --asset-regex <regex> matching exactly one release asset"
                            << (expression.isValid() ? QString{} : QStringLiteral(": %1").arg(expression.errorString()))
                            << '\n';
                return 1;
            }
            pacsmith::PackageRelease probe;
            probe.debian.version = QStringLiteral("0");
            probe.update.strategy = pacsmith::UpdateStrategy::GitHubRelease;
            probe.update.githubOwner = parts.at(0);
            probe.update.githubRepository = parts.at(1);
            if (probe.update.githubRepository.endsWith(QStringLiteral(".git"))) {
                probe.update.githubRepository.chop(4);
            }
            probe.update.githubAssetRegex = assetRegex;
            probe.update.githubIncludePrereleases = prerelease;
            pacsmith::AppSettingsStore settingsStore;
            const auto settings = settingsStore.load();
            const auto source = settings.credentialSources.value(
                QStringLiteral("github"), pacsmith::CredentialSource::Environment);
            if (source == pacsmith::CredentialSource::Age) {
                errorStream << "error: an age-stored GitHub PAT cannot be unlocked by non-interactive add; use the GUI or environment source\n";
                return 1;
            }
            pacsmith::CredentialStore credentials(settingsStore.ageSecretsPath());
            auto token = credentials.load(QStringLiteral("github"), source, nullptr).value_or(QString{});
            pacsmith::GitHubUpdateService githubService;
            pacsmith::UpdateCheckResult githubResult;
            QEventLoop githubLoop;
            QObject::connect(&githubService, &pacsmith::GitHubUpdateService::progressChanged,
                             [&errorStream](const QString &message) { errorStream << message << '\n'; });
            QObject::connect(&githubService, &pacsmith::GitHubUpdateService::finished,
                             [&githubResult, &githubLoop](const pacsmith::UpdateCheckResult &result) {
                githubResult = result;
                githubLoop.quit();
            });
            githubService.start(probe, token, requestedTag);
            token.fill(QChar::Null);
            if (githubService.isRunning()) githubLoop.exec();
            if (!githubResult.success || githubResult.downloadUrl.isEmpty()) {
                errorStream << "error: " << githubResult.message << '\n';
                if (!githubResult.availableAssets.isEmpty()) {
                    errorStream << "release assets:\n";
                    for (const auto &asset : githubResult.availableAssets) errorStream << "  " << asset << '\n';
                }
                return 1;
            }
            const auto projectId = pacsmith::PkgbuildGenerator::sanitizePackageName(
                probe.update.githubRepository);
            const auto releaseId = QStringLiteral("%1-%2")
                .arg(githubResult.detectedVersion).arg(githubResult.assetId);
            const auto target = pacsmith::defaultDownloadPath(
                projectId, releaseId, githubResult.filename);
            pacsmith::DebDownloadService downloader;
            QString downloadedPath;
            QString downloadError;
            QEventLoop downloadLoop;
            QObject::connect(&downloader, &pacsmith::DebDownloadService::progress,
                             [&errorStream](const qint64 received, const qint64 total) {
                errorStream << "downloaded " << received << (total > 0 ? QStringLiteral("/%1").arg(total) : QString{})
                            << " bytes\r" << Qt::flush;
            });
            QObject::connect(&downloader, &pacsmith::DebDownloadService::finished,
                             [&downloadedPath, &downloadLoop](const QString &path) {
                downloadedPath = path;
                downloadLoop.quit();
            });
            QObject::connect(&downloader, &pacsmith::DebDownloadService::failed,
                             [&downloadError, &downloadLoop](const QString &message) {
                downloadError = message;
                downloadLoop.quit();
            });
            downloader.start(QUrl(githubResult.downloadUrl), githubResult.sha256,
                             std::filesystem::path(target.toUtf8().constData()));
            if (downloader.isRunning()) downloadLoop.exec();
            errorStream << '\n';
            if (downloadedPath.isEmpty()) {
                errorStream << "error: " << downloadError << '\n';
                return 1;
            }
            pacsmith::ImportOptions options;
            options.version = githubResult.detectedVersion;
            options.githubAssetRegex = assetRegex;
            options.githubIncludePrereleases = prerelease;
            options.acquisition.kind = pacsmith::AcquisitionKind::GitHubRelease;
            options.acquisition.canonicalIdentity = QStringLiteral("github:%1/%2")
                .arg(probe.update.githubOwner, probe.update.githubRepository);
            options.acquisition.originalUrl = githubResult.downloadUrl;
            options.acquisition.githubOwner = probe.update.githubOwner;
            options.acquisition.githubRepository = probe.update.githubRepository;
            options.acquisition.githubReleaseId = githubResult.releaseId;
            options.acquisition.githubPrerelease = githubResult.prerelease;
            options.acquisition.githubAssetId = githubResult.assetId;
            options.acquisition.githubTag = githubResult.tag;
            options.acquisition.githubAssetName = githubResult.filename;
            options.acquisition.publisherDigest = githubResult.publisherDigest;
            QString importError;
            const auto imported = store.importSource(
                std::filesystem::path(downloadedPath.toUtf8().constData()), options, &importError);
            static_cast<void>(QFile::remove(downloadedPath));
            if (!imported) {
                errorStream << "error: " << importError << '\n';
                return 1;
            }
            out << QString::fromUtf8(store.releasePath(imported->project.id, imported->releaseId).string().c_str()) << '\n';
            return 0;
        }
        if (remote.isValid() && remote.scheme() == QStringLiteral("https")) {
            if (arguments.size() != 3) {
                errorStream << "error: direct URL imports do not accept GitHub asset options\n";
                return 1;
            }
            const auto filename = QFileInfo(remote.path()).fileName().isEmpty()
                ? QStringLiteral("vendor-artifact") : QFileInfo(remote.path()).fileName();
            const auto target = pacsmith::defaultDownloadPath(
                pacsmith::PkgbuildGenerator::sanitizePackageName(QFileInfo(filename).completeBaseName()),
                QStringLiteral("direct"), filename);
            pacsmith::DebDownloadService downloader;
            QString downloadedPath;
            QString downloadError;
            QEventLoop downloadLoop;
            QObject::connect(&downloader, &pacsmith::DebDownloadService::finished,
                             [&downloadedPath, &downloadLoop](const QString &path) {
                downloadedPath = path;
                downloadLoop.quit();
            });
            QObject::connect(&downloader, &pacsmith::DebDownloadService::failed,
                             [&downloadError, &downloadLoop](const QString &message) {
                downloadError = message;
                downloadLoop.quit();
            });
            downloader.start(remote, {}, std::filesystem::path(target.toUtf8().constData()));
            if (downloader.isRunning()) downloadLoop.exec();
            if (downloadedPath.isEmpty()) {
                errorStream << "error: " << downloadError << '\n';
                return 1;
            }
            pacsmith::ImportOptions options;
            options.acquisition.kind = pacsmith::AcquisitionKind::DirectUrl;
            options.acquisition.canonicalIdentity =
                remote.adjusted(QUrl::RemoveQuery | QUrl::RemoveFragment).toString();
            options.acquisition.originalUrl = remote.toString();
            QString importError;
            const auto imported = store.importSource(
                std::filesystem::path(downloadedPath.toUtf8().constData()), options, &importError);
            static_cast<void>(QFile::remove(downloadedPath));
            if (!imported) {
                errorStream << "error: " << importError << '\n';
                return 1;
            }
            out << QString::fromUtf8(
                       store.releasePath(imported->project.id, imported->releaseId).string().c_str())
                << '\n';
            return 0;
        }
        if (arguments.size() != 3) {
            errorStream << "error: local artifact imports do not accept additional options\n";
            return 1;
        }
        QString error;
        const auto absolute = QFileInfo(arguments.at(2)).absoluteFilePath();
        const auto project = store.importSource(
            std::filesystem::path(absolute.toUtf8().constData()), {}, &error);
        if (!project) {
            errorStream << "error: " << error << '\n';
            return 1;
        }
        out << QString::fromUtf8(store.releasePath(project->project.id, project->releaseId).string().c_str()) << '\n';
        return 0;
    }
    if (command == QStringLiteral("list")) {
        QString error;
        const auto projects = store.list(&error);
        QSet<QString> projectIds;
        for (const auto &project : projects) {
            projectIds.insert(project.id);
            const auto *release = project.newestRelease();
            out << project.id << '\t' << project.archPackageName << '\t'
                << (release == nullptr ? QString{} : release->debian.version) << '\t'
                << (release == nullptr ? QStringLiteral("no-releases")
                                       : pacsmith::buildStatusName(release->buildStatus)) << '\n';
        }
        QString managedError;
        for (const auto &managed : pacsmith::ManagedPackageRegistry::installed(&managedError)) {
            if (!projectIds.contains(managed.projectId())) {
                out << managed.projectId() << '\t' << managed.packageName << '\t'
                    << managed.packageVersion << "\torphaned-project-files\n";
            }
        }
        if (!error.isEmpty()) errorStream << "warning: " << error << '\n';
        if (!managedError.isEmpty()) errorStream << "warning: " << managedError << '\n';
        return 0;
    }
    if (command == QStringLiteral("gui")) {
        QString program = QCoreApplication::applicationDirPath() + QStringLiteral("/pacsmith-gui");
        if (!QFileInfo::exists(program)) program = QStringLiteral("pacsmith-gui");
        return QProcess::startDetached(program, {}) ? 0 : 1;
    }
    if (command == QStringLiteral("check") && arguments.size() == 3 && arguments.at(2) == QStringLiteral("--all")) {
        pacsmith::BackgroundUpdateState state;
        state.checking = true;
        state.lastRun = QDateTime::currentDateTimeUtc();
        state.message = QStringLiteral("Checking PacSmith project update trackers");
        static_cast<void>(pacsmith::BackgroundUpdateStateStore::save(state));
        int exitCode = 0;
        for (auto project : store.list()) {
            exitCode = std::max(exitCode, runCheck(store, project, out, errorStream, &state));
        }
        state.checking = false;
        state.lastRun = QDateTime::currentDateTimeUtc();
        state.message = state.availableUpdates > 0
            ? QStringLiteral("%1 update(s) available").arg(state.availableUpdates)
            : state.failedChecks > 0 ? QStringLiteral("Update checks completed with failures")
                                     : QStringLiteral("All eligible project trackers are current");
        static_cast<void>(pacsmith::BackgroundUpdateStateStore::save(state));
        return exitCode;
    }
    if (command == QStringLiteral("ai") && arguments.size() >= 3) {
        pacsmith::AppSettingsStore settingsStore;
        QString settingsError;
        const auto settings = settingsStore.load(&settingsError);
        if (!settingsError.isEmpty()) {
            errorStream << "error: " << settingsError << '\n';
            return 1;
        }
        if (arguments.at(2) == QStringLiteral("status")) {
            out << "provider\t" << pacsmith::aiProviderName(settings.provider) << '\n'
                << "model\t" << settings.model << '\n'
                << "automatic\t" << (settings.automaticallyResolveReviewItems ? "yes" : "no") << '\n';
            if (settings.provider == pacsmith::AiProviderKind::ChatGpt ||
                settings.provider == pacsmith::AiProviderKind::OpenAi ||
                settings.provider == pacsmith::AiProviderKind::Xai) {
                const auto name = pacsmith::aiProviderName(settings.provider);
                const auto defaultSource = settings.provider == pacsmith::AiProviderKind::ChatGpt
                                               ? pacsmith::CredentialSource::Keyring
                                               : pacsmith::CredentialSource::Environment;
                out << "credential-source\t"
                    << pacsmith::credentialSourceName(
                           settings.credentialSources.value(name, defaultSource))
                    << '\n';
            }
            return settings.provider == pacsmith::AiProviderKind::None ? 1 : 0;
        }
        if (arguments.at(2) != QStringLiteral("resolve") || arguments.size() != 4) {
            errorStream << "error: use 'pacsmith ai status' or 'pacsmith ai resolve <project>'\n";
            return 1;
        }
        if (settings.provider == pacsmith::AiProviderKind::None || settings.model.isEmpty()) {
            errorStream << "error: configure an AI provider and model in pacsmith-gui Settings first\n";
            return 1;
        }
        auto aiProject = requireProject(store, arguments.at(3), errorStream);
        if (!aiProject) return 1;
        auto *aiRelease = aiProject->newestRelease();
        if (aiRelease == nullptr) {
            errorStream << "error: project has no prepared releases\n";
            return 1;
        }
        QString credential;
        QString credentialPassword;
        pacsmith::CredentialStore credentials(settingsStore.ageSecretsPath());
        if (settings.provider == pacsmith::AiProviderKind::ChatGpt ||
            settings.provider == pacsmith::AiProviderKind::OpenAi ||
            settings.provider == pacsmith::AiProviderKind::Xai) {
            const auto name = pacsmith::aiProviderName(settings.provider);
            const auto defaultSource = settings.provider == pacsmith::AiProviderKind::ChatGpt
                                           ? pacsmith::CredentialSource::Keyring
                                           : pacsmith::CredentialSource::Environment;
            const auto source = settings.credentialSources.value(name, defaultSource);
            if (source == pacsmith::CredentialSource::Age) {
                credentialPassword = readPassword(errorStream, QStringLiteral("Age credential password: "));
                QString unlockError;
                if (!credentials.unlockAge(credentialPassword, &unlockError)) {
                    credentialPassword.fill(QChar::Null);
                    errorStream << "error: " << unlockError << '\n';
                    return 1;
                }
            }
            QString credentialError;
            const auto loaded = credentials.load(name, source, &credentialError);
            if (!loaded) {
                errorStream << "error: " << credentialError << '\n';
                return 1;
            }
            credential = *loaded;
        }
        pacsmith::AiAnalysisService service;
        pacsmith::AiResolution resolution;
        QEventLoop loop;
        QObject::connect(&service, &pacsmith::AiAnalysisService::progressChanged,
                         [&errorStream](const QString &message) { errorStream << message << '\n'; });
        QObject::connect(&service, &pacsmith::AiAnalysisService::credentialUpdated,
                         [&credentials, &settings, &credentialPassword, &errorStream](
                             const QString &serialized) {
            const auto source = settings.credentialSources.value(
                QStringLiteral("chatgpt"), pacsmith::CredentialSource::Keyring);
            QString error;
            if (!credentials.store(QStringLiteral("chatgpt"), source, serialized,
                                   source == pacsmith::CredentialSource::Age
                                       ? credentialPassword
                                       : QString{},
                                   &error)) {
                errorStream << "warning: could not persist refreshed ChatGPT session: "
                            << error << '\n';
            }
        });
        QObject::connect(&service, &pacsmith::AiAnalysisService::finished,
                         [&resolution, &loop](const pacsmith::AiResolution &value) {
                             resolution = value;
                             loop.quit();
                         });
        service.start(*aiRelease, settings, credential);
        credential.fill(QChar::Null);
        if (service.isRunning()) loop.exec();
        credentialPassword.fill(QChar::Null);
        if (!resolution.success) {
            errorStream << "error: " << resolution.error << '\n';
            if (!resolution.errorDetails.isEmpty()) {
                errorStream << resolution.errorDetails << '\n';
            }
            return 1;
        }
        QSet<QString> approvedUserFields;
        const auto conflicts = pacsmith::AiResolutionApplier::manualConflicts(*aiRelease, resolution);
        if (!conflicts.isEmpty() && askYesNo(errorStream,
                QStringLiteral("AI wants to replace user-owned fields %1. Approve?")
                    .arg(conflicts.join(QStringLiteral(", "))))) {
            approvedUserFields = QSet<QString>(conflicts.cbegin(), conflicts.cend());
        }
        const auto applied = pacsmith::AiResolutionApplier::apply(*aiRelease, resolution, approvedUserFields);
        if (!aiRelease->lifecycleScript.contents.isEmpty()) {
            QString lifecycleError;
            if (!store.saveLifecycle(*aiProject, *aiRelease, &lifecycleError)) {
                errorStream << "error: " << lifecycleError << '\n';
                return 1;
            }
        }
        if (aiRelease->update.strategy == pacsmith::UpdateStrategy::Manual &&
            !aiRelease->update.url.isEmpty() && !aiRelease->update.aptSuite.isEmpty()) {
            aiRelease->update.strategy = pacsmith::UpdateStrategy::AptRepository;
        }
        aiRelease->history.append(
            {QDateTime::currentDateTimeUtc(), QStringLiteral("ai-resolution"),
             QStringLiteral("Applied %1 change(s) from %2/%3")
                 .arg(resolution.changes.size()).arg(resolution.provider, resolution.model)});
        aiRelease->generatedPkgbuild = pacsmith::PkgbuildGenerator::generate(*aiRelease);
        aiRelease->generatedPkgbuildSha256 = pacsmith::sha256Hex(aiRelease->generatedPkgbuild.toUtf8());
        QString saveError;
        if (!aiRelease->pkgbuildManuallyModified) {
            if (!store.savePkgbuild(*aiProject, *aiRelease, aiRelease->generatedPkgbuild, &saveError)) {
                errorStream << "error: " << saveError << '\n';
                return 1;
            }
        } else if (!store.save(*aiProject, &saveError)) {
            errorStream << "error: " << saveError << '\n';
            return 1;
        }
        for (const auto &error : applied.errors) errorStream << "warning: " << error << '\n';
        out << aiProject->id << "\tapplied\t" << resolution.changes.size() << " changes\n";
        return 0;
    }
    if (arguments.size() < 3) {
        errorStream << "error: " << command << " requires a project ID or name\n";
        return 1;
    }
    auto project = requireProject(store, arguments.at(2), errorStream);
    if (!project) return 1;
    auto *release = project->newestRelease();
    if (release == nullptr) {
        errorStream << "error: project has no releases\n";
        return 1;
    }

    if (command == QStringLiteral("info")) {
        out << "id\t" << project->id << '\n'
            << "name\t" << scriptFriendly(project->displayName) << '\n'
            << "arch-package\t" << project->archPackageName << '\n'
            << "artifact-type\t" << pacsmith::sourcePackageTypeName(release->sourceType) << '\n'
            << "acquisition\t" << pacsmith::acquisitionKindName(release->acquisition.kind) << '\n'
            << "source-package\t" << release->debian.package << '\n'
            << "version\t" << release->debian.version << '\n'
            << "architecture\t" << release->debian.architecture << '\n'
            << "maintainer\t" << scriptFriendly(release->debian.maintainer) << '\n'
            << "homepage\t" << release->debian.homepage << '\n'
            << "source\t" << release->originalSourceFilename << '\n'
            << "sha256\t" << release->sourceSha256 << '\n'
            << "scripts\t" << release->maintainerScripts.size() << '\n'
            << "payload-entries\t" << release->payload.size() << '\n'
            << "pkgbuild-modified\t" << (release->pkgbuildManuallyModified ? "yes" : "no") << '\n';
        return 0;
    }
    if (command == QStringLiteral("versions")) {
        for (const auto &candidate : project->releases) {
            out << candidate.id << '\t' << candidate.debian.version << '\t'
                << pacsmith::releaseStateName(candidate.state) << '\t'
                << (candidate.id == project->installedReleaseId ? "installed" : "-") << '\t'
                << candidate.builds.size() << " build(s)\n";
        }
        if (project->externallyInstalled) {
            out << "external\t" << project->installedVersion << "\texternally-installed\tinstalled\t0 build(s)\n";
        }
        return 0;
    }
    if (command == QStringLiteral("dependencies")) {
        for (const auto &dependency : release->dependencies) {
            out << dependency.rawExpression << '\t' << dependency.archPackage << '\t'
                << pacsmith::mappingStatusName(dependency.status) << '\t' << dependency.mappingSource << '\n';
        }
        return 0;
    }
    if (command == QStringLiteral("scripts")) {
        if (arguments.size() == 5 && arguments.at(3) == QStringLiteral("--acknowledge")) {
            const auto scriptName = arguments.at(4);
            const auto iterator = std::find_if(release->maintainerScripts.begin(), release->maintainerScripts.end(),
                                               [&scriptName](const auto &script) { return script.name == scriptName; });
            if (iterator == release->maintainerScripts.end()) {
                errorStream << "error: maintainer script not found: " << scriptName << '\n';
                return 1;
            }
            iterator->acknowledge();
            release->history.append({QDateTime::currentDateTimeUtc(), QStringLiteral("script-review"),
                                     QStringLiteral("Acknowledged maintainer script %1 (%2)")
                                         .arg(iterator->name, iterator->acknowledgedFingerprint)});
            QString saveError;
            if (!store.save(*project, &saveError)) {
                errorStream << "error: " << saveError << '\n';
                return 1;
            }
            out << project->id << '\t' << iterator->name << "\tacknowledged\n";
            return 0;
        }
        for (const auto &finding : release->scriptFindings) {
            out << "RESPONSIBILITY\t" << finding.scriptName << '\t' << finding.kind << '\t'
                << pacsmith::scriptDispositionName(finding.disposition) << '\t'
                << pacsmith::valueOriginName(finding.provenance.origin) << '\t'
                << scriptFriendly(finding.summary) << '\n';
        }
        for (const auto &script : release->maintainerScripts) {
            out << "===== " << script.name << (script.requiresReview() ? " (REVIEW REQUIRED)" : " (ACKNOWLEDGED)")
                << " =====\n" << script.contents;
            if (!script.contents.endsWith(QLatin1Char('\n'))) out << '\n';
        }
        return 0;
    }
    if (command == QStringLiteral("lifecycle")) {
        if (arguments.size() == 4 && arguments.at(3) == QStringLiteral("--discard")) {
            QString saveError;
            if (!store.removeLifecycle(*project, *release, &saveError)) {
                errorStream << "error: " << saveError << '\n';
                return 1;
            }
            release->generatedPkgbuild = pacsmith::PkgbuildGenerator::generate(*release);
            release->generatedPkgbuildSha256 = pacsmith::sha256Hex(release->generatedPkgbuild.toUtf8());
            const auto saved = !release->pkgbuildManuallyModified
                                   ? store.savePkgbuild(*project, *release, release->generatedPkgbuild, &saveError)
                                   : store.save(*project, &saveError);
            if (!saved) {
                errorStream << "error: " << saveError << '\n';
                return 1;
            }
            out << project->id << "\tlifecycle-discarded\n";
            return 0;
        }
        if (arguments.size() == 5 && arguments.at(3) == QStringLiteral("--acknowledge")) {
            if (release->lifecycleScript.contents.isEmpty()) {
                errorStream << "error: this project has no Arch lifecycle script\n";
                return 1;
            }
            const auto fingerprint = release->lifecycleScript.contentFingerprint();
            if (arguments.at(4).compare(fingerprint, Qt::CaseInsensitive) != 0) {
                errorStream << "error: fingerprint does not match the exact current lifecycle content\n"
                            << "current: " << fingerprint << '\n';
                return 1;
            }
            if (!release->lifecycleScript.validationPassed) {
                errorStream << "error: lifecycle script failed validation: "
                            << release->lifecycleScript.validationMessage << '\n';
                return 1;
            }
            release->lifecycleScript.acknowledge();
            release->history.append({QDateTime::currentDateTimeUtc(), QStringLiteral("lifecycle-review"),
                                     QStringLiteral("Acknowledged Arch lifecycle script %1").arg(fingerprint)});
            QString saveError;
            if (!store.save(*project, &saveError)) {
                errorStream << "error: " << saveError << '\n';
                return 1;
            }
            out << project->id << "\tlifecycle\tacknowledged\t" << fingerprint << '\n';
            return 0;
        }
        if (release->lifecycleScript.contents.isEmpty()) {
            out << "No Arch lifecycle script is configured.\n";
            return 0;
        }
        out << "file\t" << release->lifecycleScript.fileName << '\n'
            << "fingerprint\t" << release->lifecycleScript.contentFingerprint() << '\n'
            << "validation\t" << (release->lifecycleScript.validationPassed ? "passed" : "failed") << '\n'
            << "acknowledgement\t"
            << (release->lifecycleScript.requiresAcknowledgement() ? "required" : "acknowledged") << '\n'
            << "provenance\t" << pacsmith::valueOriginName(release->lifecycleScript.provenance.origin) << '\n'
            << "===== CONTENT =====\n" << release->lifecycleScript.contents;
        if (!release->lifecycleScript.contents.endsWith(QLatin1Char('\n'))) out << '\n';
        return 0;
    }
    if (command == QStringLiteral("payload")) {
        if (arguments.size() == 5 && arguments.at(3) == QStringLiteral("--show")) {
            const auto path = arguments.at(4);
            const auto entry = std::find_if(release->payload.cbegin(), release->payload.cend(),
                                            [&path](const auto &candidate) { return candidate.path == path; });
            if (entry == release->payload.cend()) {
                errorStream << "error: payload path not found: " << path << '\n';
                return 1;
            }
            QString inspectionError;
            const auto inspection = pacsmith::PayloadInspector::inspectFile(
                store.sourcePath(*release), path, &inspectionError);
            if (!inspection) {
                errorStream << "error: " << inspectionError << '\n';
                return 1;
            }
            out << "path\t" << path << '\n'
                << "sha256\t" << inspection->contentSha256 << '\n';
            if (inspection->binary) out << "[binary or non-UTF-8 content]\n";
            else out << inspection->textPreview;
            if (!inspection->textPreview.endsWith(QLatin1Char('\n'))) out << '\n';
            if (inspection->previewTruncated) out << "[preview truncated at 1 MiB]\n";
            return 0;
        }
        for (const auto &entry : release->payload) {
            out << entry.type << '\t' << entry.size << '\t' << entry.path;
            if (!entry.symlinkTarget.isEmpty()) out << " -> " << entry.symlinkTarget;
            if (entry.requiresReview) {
                const auto review = pacsmith::PayloadReview::state(*release, entry);
                if (review.needsReview) out << "\tREVIEW: choose keep or exclude — " << entry.reviewReason;
                else if (review.disposition == pacsmith::PayloadDisposition::Excluded) out << "\tEXCLUDED (acknowledged)";
                else out << "\tKEPT (acknowledged)";
            }
            out << '\n';
        }
        return 0;
    }
    if (command == QStringLiteral("pkgbuild")) {
        QString error;
        const auto contents = store.readPkgbuild(*release, &error);
        if (!contents) {
            errorStream << "error: " << error << '\n';
            return 1;
        }
        out << *contents;
        return 0;
    }
    if (command == QStringLiteral("check")) return runCheck(store, *project, out, errorStream);

    if (command == QStringLiteral("build")) {
        pacsmith::BuildService service;
        int exitCode = 1;
        QObject::connect(&service, &pacsmith::BuildService::outputAvailable, &application,
                         [&out](const QString &text) { out << text << Qt::flush; });
        QObject::connect(&service, &pacsmith::BuildService::failedToStart, &application,
                         [&errorStream, &exitCode](const QString &message) {
                             errorStream << "error: " << message << '\n';
                             exitCode = 1;
                             QCoreApplication::exit(exitCode);
                         });
        QObject::connect(&service, &pacsmith::BuildService::finished, &application,
                         [&store, &project, release, &errorStream, &exitCode](const pacsmith::ProcessResult &result) {
                             release->buildStatus = result.succeeded() ? pacsmith::BuildStatus::Succeeded
                                                                       : pacsmith::BuildStatus::Failed;
                             release->lastBuildLog = result.output + result.errorOutput;
                             release->producedPackages = result.producedPackages;
                             release->builds.append(pacsmith::buildRecordFromResult(
                                 QStringLiteral("build-%1").arg(result.startedAt.toMSecsSinceEpoch()),
                                 release->buildStatus, release->lastBuildLog,
                                 result.producedPackages, store.releasePath(*release),
                                 result.startedAt, result.finishedAt));
                             if (result.succeeded()) release->state = pacsmith::ReleaseState::Built;
                             release->history.append({result.finishedAt, QStringLiteral("build"),
                                                      result.succeeded() ? QStringLiteral("Build succeeded")
                                                                         : QStringLiteral("Build failed")});
                             QString error;
                             if (!store.save(*project, &error)) errorStream << "warning: " << error << '\n';
                             exitCode = result.succeeded() ? 0 : 1;
                             QCoreApplication::exit(exitCode);
        });
        service.start(store.releasePath(*release));
        if (!service.isRunning()) return exitCode;
        application.exec();
        return exitCode;
    }
    if (command == QStringLiteral("install")) {
        if (!release->lifecycleScript.contents.isEmpty() &&
            (!release->lifecycleScript.validationPassed ||
             release->lifecycleScript.requiresAcknowledgement())) {
            errorStream << "error: installation is blocked until the exact validated Arch lifecycle script is acknowledged\n"
                        << "review: pacsmith lifecycle " << project->id << '\n'
                        << "acknowledge: pacsmith lifecycle " << project->id << " --acknowledge "
                        << release->lifecycleScript.contentFingerprint() << '\n';
            return 1;
        }
        QString packagePath;
        if (arguments.size() >= 4) packagePath = QFileInfo(arguments.at(3)).absoluteFilePath();
        else if (!release->producedPackages.isEmpty()) packagePath = release->producedPackages.first();
        if (packagePath.isEmpty()) {
            errorStream << "error: no built package is recorded; run pacsmith build first\n";
            return 1;
        }
        pacsmith::InstallService service;
        int exitCode = 1;
        QObject::connect(&service, &pacsmith::InstallService::outputAvailable, &application,
                         [&out](const QString &text) { out << text << Qt::flush; });
        QObject::connect(&service, &pacsmith::InstallService::failedToStart, &application,
                         [&errorStream](const QString &message) {
                             errorStream << "error: " << message << '\n';
                             QCoreApplication::exit(1);
                         });
        QObject::connect(&service, &pacsmith::InstallService::finished, &application,
                         [&store, &project, release, &errorStream, &exitCode](const pacsmith::ProcessResult &result) {
                             if (result.succeeded()) static_cast<void>(store.reconcileInstalled(*project, nullptr));
                             release->history.append({result.finishedAt, QStringLiteral("install"),
                                                      result.succeeded() ? QStringLiteral("Installation succeeded")
                                                                         : QStringLiteral("Installation failed")});
                             QString error;
                             if (!store.save(*project, &error)) errorStream << "warning: " << error << '\n';
                             exitCode = result.succeeded() ? 0 : 1;
                             QCoreApplication::exit(exitCode);
        });
        service.start(std::filesystem::path(packagePath.toUtf8().constData()));
        if (!service.isRunning()) return exitCode;
        application.exec();
        return exitCode;
    }
    if (command == QStringLiteral("rollback")) {
        if (arguments.size() != 4) {
            errorStream << "error: rollback requires a release ID or version\n";
            return 1;
        }
        const auto selected = std::find_if(project->releases.begin(), project->releases.end(),
                                           [&](const auto &candidate) {
                                               return candidate.id == arguments.at(3) ||
                                                      candidate.debian.version == arguments.at(3);
                                           });
        if (selected == project->releases.end()) {
            errorStream << "error: retained release not found: " << arguments.at(3) << '\n';
            return 1;
        }
        QString packagePath;
        for (auto build = selected->builds.crbegin(); build != selected->builds.crend() && packagePath.isEmpty(); ++build) {
            for (const auto &artifact : build->artifacts) {
                const auto candidate = store.releasePath(*selected) /
                    std::filesystem::path(artifact.relativePath.toUtf8().constData());
                if (QFileInfo::exists(QString::fromUtf8(candidate.string().c_str()))) {
                    packagePath = QString::fromUtf8(candidate.string().c_str());
                    break;
                }
            }
        }
        if (packagePath.isEmpty()) {
            for (const auto &candidate : selected->producedPackages) {
                if (QFileInfo::exists(candidate)) { packagePath = candidate; break; }
            }
        }
        if (packagePath.isEmpty()) {
            errorStream << "error: this release has no retained Arch package artifact\n";
            return 1;
        }
        pacsmith::InstallService service;
        int exitCode = 1;
        QObject::connect(&service, &pacsmith::InstallService::outputAvailable, &application,
                         [&out](const QString &text) { out << text << Qt::flush; });
        QObject::connect(&service, &pacsmith::InstallService::failedToStart, &application,
                         [&errorStream](const QString &message) {
                             errorStream << "error: " << message << '\n';
                             QCoreApplication::exit(1);
                         });
        QObject::connect(&service, &pacsmith::InstallService::finished, &application,
                         [&store, &project, &errorStream, &exitCode](const pacsmith::ProcessResult &result) {
                             if (result.succeeded()) static_cast<void>(store.reconcileInstalled(*project, nullptr));
                             project->history.append({result.finishedAt, QStringLiteral("rollback"),
                                 result.succeeded() ? QStringLiteral("Rollback installed successfully")
                                                    : QStringLiteral("Rollback failed")});
                             QString saveError;
                             if (!store.save(*project, &saveError)) errorStream << "warning: " << saveError << '\n';
                             exitCode = result.succeeded() ? 0 : 1;
                             QCoreApplication::exit(exitCode);
                         });
        service.start(std::filesystem::path(packagePath.toUtf8().constData()));
        if (service.isRunning()) application.exec();
        return exitCode;
    }
    if (command == QStringLiteral("uninstall")) {
        if (project->installedVersion.isEmpty()) {
            out << project->id << "\tnot-installed\n";
            return 0;
        }
        pacsmith::InstallService service;
        int exitCode = 1;
        QObject::connect(&service, &pacsmith::InstallService::outputAvailable, &application,
                         [&out](const QString &text) { out << text << Qt::flush; });
        QObject::connect(&service, &pacsmith::InstallService::failedToStart, &application,
                         [&errorStream](const QString &message) {
                             errorStream << "error: " << message << '\n';
                             QCoreApplication::exit(1);
                         });
        QObject::connect(&service, &pacsmith::InstallService::finished, &application,
                         [&store, &project, &errorStream, &exitCode](const pacsmith::ProcessResult &result) {
                             if (result.succeeded()) static_cast<void>(store.reconcileInstalled(*project, nullptr));
                             project->history.append({result.finishedAt, QStringLiteral("uninstall"),
                                 result.succeeded() ? QStringLiteral("Package removed")
                                                    : QStringLiteral("Package removal failed")});
                             QString saveError;
                             if (!store.save(*project, &saveError)) errorStream << "warning: " << saveError << '\n';
                             exitCode = result.succeeded() ? 0 : 1;
                             QCoreApplication::exit(exitCode);
                         });
        service.startUninstall(project->archPackageName);
        if (service.isRunning()) application.exec();
        return exitCode;
    }

    errorStream << "error: unknown command: " << command << '\n';
    printUsage(errorStream);
    return 1;
}
