#include "core/project_store/internal.hpp"

#include "core/apt_repository.hpp"
#include "core/apt_sources.hpp"
#include "core/deb_analyzer.hpp"
#include "core/dependency_parser.hpp"
#include "core/lifecycle_validator.hpp"
#include "core/managed_package.hpp"
#include "core/path_safety.hpp"
#include "core/payload_inspector.hpp"
#include "core/payload_review.hpp"
#include "core/pkgbuild_generator.hpp"
#include "core/repository_trust.hpp"
#include "core/script_evidence.hpp"
#include "core/source_analyzer.hpp"

#include <QCryptographicHash>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>

#include <algorithm>
#include <array>

namespace pacsmith::project_store_internal {

std::filesystem::path pathFromQString(const QString &value) {
    return std::filesystem::path(value.toUtf8().constData());
}

QString qStringFromPath(const std::filesystem::path &value) {
    return QString::fromUtf8(value.string().c_str());
}

bool validId(const QString &id) {
    static const QRegularExpression expression(QStringLiteral("^[a-z0-9][a-z0-9@._+-]*$"));
    return expression.match(id).hasMatch();
}

bool payloadPathCovers(const QString &parent, const QString &child) {
    return child == parent || child.startsWith(parent + QLatin1Char('/'));
}

bool releaseWasBuilt(const PackageRelease &release) {
    return release.state == ReleaseState::Built ||
           release.buildStatus == BuildStatus::Succeeded ||
           !release.producedPackages.isEmpty();
}

bool writeBytes(const std::filesystem::path &path, const QByteArray &contents, QString *error) {
    QSaveFile file(qStringFromPath(path));
    if (!file.open(QIODevice::WriteOnly)) {
        if (error != nullptr) *error = file.errorString();
        return false;
    }
    if (file.write(contents) != contents.size() || !file.commit()) {
        if (error != nullptr) *error = file.errorString();
        file.cancelWriting();
        return false;
    }
    return true;
}

bool writeImportedLifecycle(const std::filesystem::path &releaseDirectory,
                            PackageRelease &release, QString *error) {
    if (release.lifecycleScript.contents.isEmpty()) return true;
    static const QRegularExpression safeName(QStringLiteral("^[a-z0-9@._+\\-]+\\.install$"));
    if (!safeName.match(release.lifecycleScript.fileName).hasMatch()) {
        if (error != nullptr) *error = QStringLiteral("Unsafe Arch lifecycle filename");
        return false;
    }
    const auto validation = LifecycleValidator::validate(release.lifecycleScript.contents);
    release.lifecycleScript.validationPassed = validation.passed;
    release.lifecycleScript.validationMessage = validation.message();
    return writeBytes(releaseDirectory / pathFromQString(release.lifecycleScript.fileName),
                      release.lifecycleScript.contents.toUtf8(), error);
}

void applyInitialUpdateConfiguration(PackageRelease &release,
                                     const ImportOptions &options) {
    if (!options.initialUpdate) return;
    const auto detectedCandidates = release.update.detectedCandidates;
    const auto aptCandidates = release.update.aptCandidates;
    const auto rpmCandidates = release.update.rpmCandidates;
    release.update = *options.initialUpdate;
    release.update.detectedCandidates.append(detectedCandidates);
    release.update.detectedCandidates.removeDuplicates();
    for (const auto &candidate : aptCandidates) {
        if (std::none_of(release.update.aptCandidates.cbegin(),
                         release.update.aptCandidates.cend(),
                         [&](const auto &existing) {
                             return existing.displayText() == candidate.displayText();
                         })) {
            release.update.aptCandidates.append(candidate);
        }
    }
    for (const auto &candidate : rpmCandidates) {
        if (std::none_of(release.update.rpmCandidates.cbegin(),
                         release.update.rpmCandidates.cend(),
                         [&](const auto &existing) {
                             return existing.displayText() == candidate.displayText();
                         })) {
            release.update.rpmCandidates.append(candidate);
        }
    }
}

bool storeImportSigningKeys(const std::filesystem::path &directory,
                            const QList<ExtractedSigningKey> &detectedKeys,
                            const ImportOptions &options,
                            UpdateConfiguration &update, QString *error) {
    const auto appendUnique = [&update](const RepositorySigningKey &key) {
        const auto duplicate = std::any_of(
            update.signingKeys.cbegin(), update.signingKeys.cend(),
            [&](const auto &existing) { return existing.sha256 == key.sha256; });
        if (!duplicate) update.signingKeys.append(key);
    };

    if (!options.trustedSigningKey.isEmpty()) {
        QString keyError;
        const auto imported = RepositoryTrust::importUserKey(
            directory, options.trustedSigningKey,
            options.trustedSigningKeySource.isEmpty()
                ? QStringLiteral("repository-first import")
                : options.trustedSigningKeySource,
            &keyError);
        if (!imported) {
            if (error != nullptr) {
                *error = QStringLiteral("Could not persist the trusted repository key: %1")
                             .arg(keyError);
            }
            return false;
        }
        appendUnique(*imported);
        update.aptSigningKeyring = imported->relativePath;
        if (!imported->fingerprints.isEmpty()) {
            update.trustedSigningFingerprint = imported->fingerprints.first();
        }
    }

    for (const auto &candidate : detectedKeys) {
        QString keyError;
        const auto stored = RepositoryTrust::storeVendorKey(directory, candidate, &keyError);
        if (stored) appendUnique(*stored);
    }
    if (update.aptSigningKeyring.isEmpty() && !update.signingKeys.isEmpty()) {
        const auto &key = update.signingKeys.first();
        update.aptSigningKeyring = key.relativePath;
        if (!key.fingerprints.isEmpty()) {
            update.trustedSigningFingerprint = key.fingerprints.first();
        }
    }
    return true;
}

std::optional<SourceAnalysis> analyzeArtifact(
    const std::filesystem::path &path, QString *error,
    const ImportProgressCallback &progress) {
    const auto type = SourceAnalyzer::detect(path, error);
    if (!type) return std::nullopt;
    if (*type != SourcePackageType::Debian) {
        return SourceAnalyzer::analyze(path, error, progress);
    }

    AnalysisError analysisError;
    const auto deb = DebAnalyzer{}.analyze(path, analysisError, progress);
    if (!deb) {
        if (error != nullptr) *error = analysisError.message;
        return std::nullopt;
    }
    SourceAnalysis result;
    result.type = SourcePackageType::Debian;
    result.metadata = deb->metadata;
    result.dependencies = deb->dependencies;
    result.maintainerScripts = deb->maintainerScripts;
    result.scriptFindings = deb->scriptFindings;
    result.payload = deb->payload;
    result.payloadRules = deb->payloadRules;
    result.updateCandidates = deb->updateCandidates;
    result.aptCandidates = deb->aptCandidates;
    result.rpmCandidates = deb->rpmCandidates;
    result.signingKeys = deb->signingKeys;
    result.installMapping = deb->installMapping;
    if (deb->icon) {
        result.icon = ExtractedSourceIcon{deb->icon->sourcePath, deb->icon->contents};
    }
    return result;
}

bool copyFileAtomically(const std::filesystem::path &source,
                        const std::filesystem::path &destination, QString *error) {
    QFile input(qStringFromPath(source));
    QSaveFile output(qStringFromPath(destination));
    if (!input.open(QIODevice::ReadOnly) || !output.open(QIODevice::WriteOnly)) {
        if (error != nullptr) *error = input.isOpen() ? output.errorString() : input.errorString();
        return false;
    }
    std::array<char, 1024 * 1024> buffer{};
    for (;;) {
        const auto read = input.read(buffer.data(), static_cast<qint64>(buffer.size()));
        if (read == 0) break;
        if (read < 0 || output.write(buffer.data(), read) != read) {
            if (error != nullptr) *error = read < 0 ? input.errorString() : output.errorString();
            output.cancelWriting();
            return false;
        }
    }
    if (!output.commit()) {
        if (error != nullptr) *error = output.errorString();
        return false;
    }
    return true;
}

bool materializeIntegrationIcon(const ProjectStore &store, Project &project,
                                PackageRelease &release,
                                const std::filesystem::path &artifactPath,
                                const QString &fallbackSourcePath,
                                const QByteArray &fallbackContents,
                                QString *error) {
    auto &icon = release.installMapping.icon;
    QByteArray contents;
    if (icon.sourceKind == IconSourceKind::Payload && !icon.sourcePath.isEmpty()) {
        QString readError;
        const auto selected = PayloadInspector::readFileBytes(
            artifactPath, icon.sourcePath, 4 * 1024 * 1024, &readError);
        if (selected) contents = *selected;
        else if (icon.sourcePath == fallbackSourcePath && !fallbackContents.isEmpty()) {
            // The analyzer already captured and validated this exact payload
            // member. Reuse those bounded bytes if reopening the nested source
            // is unavailable in the current libarchive build.
            contents = fallbackContents;
        } else {
            icon.missing = true;
        }
    } else if ((icon.sourceKind == IconSourceKind::LocalFile ||
                icon.sourceKind == IconSourceKind::RemoteUrl) && !icon.sha256.isEmpty()) {
        for (const auto &prior : project.releases) {
            const auto &priorIcon = prior.installMapping.icon;
            if (priorIcon.sha256 != icon.sha256 || priorIcon.projectPath.isEmpty()) continue;
            const auto safe = PathSafety::normalizedArchivePath(priorIcon.projectPath);
            if (!safe || !safe->startsWith(QStringLiteral("files/"))) continue;
            QFile file(qStringFromPath(store.releasePath(prior) / pathFromQString(*safe)));
            if (!file.open(QIODevice::ReadOnly) || file.size() <= 0 ||
                file.size() > 4 * 1024 * 1024) continue;
            const auto candidate = file.readAll();
            if (sha256Hex(candidate) == icon.sha256) {
                contents = candidate;
                break;
            }
        }
        if (contents.isEmpty()) icon.missing = true;
    } else if (!fallbackContents.isEmpty()) {
        contents = fallbackContents;
        if (icon.sourceKind == IconSourceKind::None) {
            icon.sourceKind = IconSourceKind::Payload;
            icon.sourcePath = fallbackSourcePath;
            icon.provenance.origin = ValueOrigin::Deterministic;
        }
    }
    if (contents.isEmpty()) return true;

    auto suffix = icon.format.toLower();
    if (suffix.isEmpty()) {
        suffix = QFileInfo(icon.sourcePath.isEmpty() ? fallbackSourcePath
                                                     : icon.sourcePath).suffix().toLower();
    }
    if (suffix != QStringLiteral("png") && suffix != QStringLiteral("svg") &&
        suffix != QStringLiteral("xpm")) {
        icon.missing = true;
        return true;
    }
    const auto relative = QStringLiteral("files/icon.%1").arg(suffix);
    if (!writeBytes(store.releasePath(release) / pathFromQString(relative), contents, error)) {
        return false;
    }
    icon.projectPath = relative;
    icon.sha256 = sha256Hex(contents);
    icon.format = suffix;
    icon.missing = false;
    if (icon.iconName.isEmpty()) icon.iconName = release.archPackageName;
    release.iconPath = relative;
    release.iconSourcePath = icon.sourcePath.isEmpty() ? icon.sourceUrl : icon.sourcePath;
    release.iconSha256 = icon.sha256;
    project.iconPath = QStringLiteral("releases/%1/%2").arg(release.id, relative);
    project.iconSourcePath = release.iconSourcePath;
    project.iconSha256 = icon.sha256;
    return true;
}

QString sourceDisplayName(const DebianMetadata &metadata,
                          const QList<DesktopEntryConfiguration> &desktops) {
    return preferredDisplayName(metadata, desktops);
}

QString sourceIdentity(const DebianMetadata &metadata) {
    return QStringLiteral("deb:%1:%2").arg(metadata.package.toLower(), metadata.architecture.toLower());
}

QString githubSourceIdentity(const QString &owner, const QString &repository) {
    return QStringLiteral("github:%1/%2").arg(owner.toLower(), repository.toLower());
}

QString normalizedSourceIdentity(const QString &identity) {
    const auto trimmed = identity.trimmed();
    if (trimmed.startsWith(QStringLiteral("github:"), Qt::CaseInsensitive)) {
        const auto rest = trimmed.mid(7);
        const auto slash = rest.indexOf(QLatin1Char('/'));
        if (slash > 0) {
            return githubSourceIdentity(rest.left(slash), rest.mid(slash + 1));
        }
    }
    return trimmed;
}

QString githubIdentityFromProject(const Project &project) {
    const auto fromSource = normalizedSourceIdentity(project.sourceIdentity);
    if (fromSource.startsWith(QStringLiteral("github:"))) return fromSource;
    for (const auto &release : project.releases) {
        if (!release.update.githubOwner.isEmpty() && !release.update.githubRepository.isEmpty()) {
            return githubSourceIdentity(release.update.githubOwner, release.update.githubRepository);
        }
        if (!release.acquisition.githubOwner.isEmpty() &&
            !release.acquisition.githubRepository.isEmpty()) {
            return githubSourceIdentity(release.acquisition.githubOwner,
                                        release.acquisition.githubRepository);
        }
    }
    return {};
}

QString requestedSourceIdentity(const ImportOptions &options, const DebianMetadata *metadata) {
    if (!options.acquisition.githubOwner.isEmpty() &&
        !options.acquisition.githubRepository.isEmpty()) {
        return githubSourceIdentity(options.acquisition.githubOwner,
                                    options.acquisition.githubRepository);
    }
    if (!options.acquisition.canonicalIdentity.isEmpty()) {
        return normalizedSourceIdentity(options.acquisition.canonicalIdentity);
    }
    if (options.acquisition.kind == AcquisitionKind::GitHubRelease) {
        return githubSourceIdentity(options.acquisition.githubOwner,
                                    options.acquisition.githubRepository);
    }
    if (metadata != nullptr) return sourceIdentity(*metadata);
    return {};
}

std::optional<Project> findMatchingProject(const QList<Project> &projects,
                                           const ImportOptions &options,
                                           const DebianMetadata *metadata) {
    const auto wanted = requestedSourceIdentity(options, metadata);
    if (!wanted.isEmpty()) {
        for (const auto &existing : projects) {
            if (normalizedSourceIdentity(existing.sourceIdentity) == wanted) return existing;
        }
        if (wanted.startsWith(QStringLiteral("github:"))) {
            for (const auto &existing : projects) {
                if (githubIdentityFromProject(existing) == wanted) return existing;
            }
        }
    }
    if (metadata != nullptr) {
        const auto debIdentity = sourceIdentity(*metadata);
        for (const auto &existing : projects) {
            if (normalizedSourceIdentity(existing.sourceIdentity) == debIdentity) return existing;
            for (const auto &release : existing.releases) {
                if (!release.debian.package.isEmpty() &&
                    sourceIdentity(release.debian) == debIdentity) {
                    return existing;
                }
            }
        }
    }
    return std::nullopt;
}

void adoptCanonicalIdentity(Project &project, const QString &identity) {
    if (identity.isEmpty()) return;
    if (normalizedSourceIdentity(project.sourceIdentity) == identity) {
        project.sourceIdentity = identity;
        return;
    }
    if (identity.startsWith(QStringLiteral("github:"))) project.sourceIdentity = identity;
}

QString releaseId(const QString &version, const QString &sha256) {
    QString safe = version.toLower();
    safe.replace(QRegularExpression(QStringLiteral("[^a-z0-9@._+-]+")), QStringLiteral("-"));
    safe.remove(QRegularExpression(QStringLiteral("^-+|-+$")));
    if (safe.isEmpty() || !safe.at(0).isLetterOrNumber()) safe.prepend(QStringLiteral("v-"));
    if (safe.size() > 80) safe.truncate(80);
    return QStringLiteral("%1-%2").arg(safe, sha256.left(12));
}

QString expectedArchVersion(const PackageRelease &release) {
    const auto [epoch, version] = PkgbuildGenerator::splitEpochAndVersion(release.debian.version);
    return QStringLiteral("%1%2-%3")
        .arg(epoch.isEmpty() ? QString{} : epoch + QLatin1Char(':'), version)
        .arg(release.archPkgrelOverride.isEmpty()
                 ? QString::number(std::max(1, release.archPkgrel))
                 : release.archPkgrelOverride);
}

QString repackagedPkgrel(const QString &upstream) {
    static const QRegularExpression format(QStringLiteral(R"(^(\d+)(?:\.(\d+))?$)"));
    const auto match = format.match(upstream);
    if (!match.hasMatch()) return QStringLiteral("1.1");
    const auto major = match.captured(1).toInt();
    const auto minor = match.captured(2).isEmpty() ? 1 : match.captured(2).toInt() + 1;
    return QStringLiteral("%1.%2").arg(std::max(1, major)).arg(std::max(1, minor));
}

bool releaseMatchesInstalledVersion(const Project &project, const QString &installedVersion) {
    for (const auto &release : project.releases) {
        if (expectedArchVersion(release) == installedVersion) return true;
        for (const auto &build : release.builds) {
            if (std::any_of(build.artifacts.cbegin(), build.artifacts.cend(),
                            [&](const auto &artifact) {
                                return artifact.packageName == project.archPackageName &&
                                       artifact.packageVersion == installedVersion;
                            })) {
                return true;
            }
        }
    }
    return false;
}

bool projectOwnsInstalledPackage(const Project &project, const QString &installedVersion) {
    if (installedVersion.isEmpty()) return false;
    if (const auto managed = ManagedPackageRegistry::find(project.archPackageName, nullptr);
        managed && !managed->projectId().isEmpty()) {
        return managed->projectId() == project.id;
    }
    return releaseMatchesInstalledVersion(project, installedVersion);
}

FieldProvenance detected(const QString &fingerprint, const QString &rationale) {
    return {ValueOrigin::Deterministic, {}, {}, fingerprint, rationale,
            QDateTime::currentDateTimeUtc(), false};
}

bool populateAptCandidates(PackageRelease &release) {
    if (!release.update.aptCandidates.isEmpty()) return false;
    QList<AptRepositoryCandidate> candidates;
    for (const auto &script : release.maintainerScripts) {
        candidates.append(AptSourcesParser::parse(
            QByteArrayView(script.contents.toUtf8()),
            QStringLiteral("control/%1").arg(script.name)));
    }
    if (candidates.isEmpty()) return false;
    release.update.aptCandidates = candidates;
    const auto &candidate = candidates.first();
    if (release.update.url.isEmpty()) release.update.url = candidate.uri;
    if (release.update.aptSuite.isEmpty()) release.update.aptSuite = candidate.suite;
    if (release.update.aptComponent.isEmpty() && !candidate.components.isEmpty()) {
        release.update.aptComponent = candidate.components.first();
    }
    if (release.update.aptArchitecture.isEmpty()) {
        release.update.aptArchitecture = !candidate.architectures.isEmpty()
                                             ? candidate.architectures.first()
                                             : release.debian.architecture;
    }
    if (release.update.aptPackageName.isEmpty()) {
        release.update.aptPackageName = release.debian.package;
    }
    release.update.strategy = UpdateStrategy::AptRepository;
    const auto provenance = detected(sha256Hex(candidate.displayText().toUtf8()),
                                     QStringLiteral("Extracted from vendor package evidence."));
    for (const auto &field : {QStringLiteral("update.url"), QStringLiteral("update.aptSuite"),
                              QStringLiteral("update.aptComponent"),
                              QStringLiteral("update.aptArchitecture"),
                              QStringLiteral("update.aptPackageName")}) {
        release.fieldProvenance.insert(field, provenance);
    }
    return true;
}

bool populateRpmCandidates(PackageRelease &release) {
    if (release.update.rpmCandidates.isEmpty()) return false;
    const auto &candidate = release.update.rpmCandidates.first();
    bool changed = false;
    if (release.update.url.isEmpty()) {
        release.update.url = candidate.baseUrl;
        changed = true;
    }
    if (release.update.rpmArchitecture.isEmpty()) {
        release.update.rpmArchitecture = candidate.architecture.isEmpty()
            ? release.debian.architecture : candidate.architecture;
        changed = true;
    }
    if (release.update.rpmPackageName.isEmpty()) {
        release.update.rpmPackageName = release.debian.package;
        changed = true;
    }
    if (release.update.strategy == UpdateStrategy::Manual) {
        release.update.strategy = UpdateStrategy::RpmRepository;
        changed = true;
    }
    if (changed) {
        const auto provenance = detected(sha256Hex(candidate.displayText().toUtf8()),
                                         QStringLiteral("Extracted from vendor package evidence."));
        for (const auto &field : {QStringLiteral("update.url"),
                                  QStringLiteral("update.rpmArchitecture"),
                                  QStringLiteral("update.rpmPackageName")}) {
            release.fieldProvenance.insert(field, provenance);
        }
    }
    return changed;
}

ScriptEvidence payloadRepositoryEvidence(const PackageRelease &release) {
    QList<MaintainerScript> evidenceFiles;
    for (const auto &entry : release.payload) {
        if (entry.textPreview.isEmpty() || entry.previewTruncated) continue;
        evidenceFiles.append({QStringLiteral("payload/%1").arg(entry.path),
                              entry.textPreview, {}});
    }
    return ScriptEvidenceAnalyzer::analyze(evidenceFiles);
}

bool migrateScriptEvidence(PackageRelease &release) {
    const auto evidence = ScriptEvidenceAnalyzer::analyze(release.maintainerScripts);
    const auto payloadEvidence = payloadRepositoryEvidence(release);
    bool changed = false;
    const bool findingsChanged = release.scriptFindings.size() != evidence.findings.size() ||
        std::any_of(evidence.findings.cbegin(), evidence.findings.cend(),
                    [&](const auto &detected) {
                        return std::none_of(
                            release.scriptFindings.cbegin(), release.scriptFindings.cend(),
                            [&](const auto &stored) {
                                return stored.evidenceFingerprint == detected.evidenceFingerprint;
                            });
                    });
    if (findingsChanged) {
        QList<ScriptFinding> migrated;
        QStringList lifecycleSources;
        for (auto detected : evidence.findings) {
            const auto old = std::find_if(
                release.scriptFindings.cbegin(), release.scriptFindings.cend(),
                [&](const auto &stored) {
                    return stored.scriptName == detected.scriptName &&
                           stored.kind == detected.kind;
                });
            if (old != release.scriptFindings.cend()) {
                const auto newFingerprint = detected.evidenceFingerprint;
                detected.summary = old->summary;
                detected.disposition = old->disposition;
                detected.provenance = old->provenance;
                detected.evidenceFingerprint = newFingerprint;
                detected.provenance.sourceFingerprint = newFingerprint;
                if (detected.disposition == ScriptDisposition::LifecycleRequired &&
                    release.lifecycleScript.sourceFingerprints.contains(
                        old->evidenceFingerprint)) {
                    lifecycleSources.append(newFingerprint);
                }
            }
            migrated.append(std::move(detected));
        }
        release.scriptFindings = std::move(migrated);
        if (!release.lifecycleScript.contents.isEmpty()) {
            release.lifecycleScript.sourceFingerprints = lifecycleSources;
        }
        changed = true;
    }
    if (release.update.aptCandidates.isEmpty() && !evidence.aptCandidates.isEmpty()) {
        release.update.aptCandidates = evidence.aptCandidates;
        changed = true;
    }
    if (release.update.rpmCandidates.isEmpty()) {
        auto candidates = evidence.rpmCandidates;
        candidates.append(payloadEvidence.rpmCandidates);
        if (!candidates.isEmpty()) {
            release.update.rpmCandidates = candidates;
            changed = true;
        }
    }
    if (!release.update.aptCandidates.isEmpty() && release.update.url.isEmpty()) {
        changed = populateAptCandidates(release) || changed;
        if (release.update.url.isEmpty()) {
            const auto &candidate = release.update.aptCandidates.first();
            release.update.url = candidate.uri;
            release.update.aptSuite = candidate.suite;
            if (!candidate.components.isEmpty()) release.update.aptComponent = candidate.components.first();
            if (!candidate.architectures.isEmpty()) {
                release.update.aptArchitecture = candidate.architectures.first();
            }
            release.update.aptPackageName = release.debian.package;
            release.update.strategy = UpdateStrategy::AptRepository;
            changed = true;
        }
    }
    if (release.update.aptCandidates.isEmpty() && !release.update.rpmCandidates.isEmpty()) {
        changed = populateRpmCandidates(release) || changed;
    }
    return changed;
}

bool migratePayloadMetadata(PackageRelease &release) {
    bool changed = false;
    if (release.sourceType == SourcePackageType::AppImage) {
        // AppImage paths are installed below /opt/<bundle>; internal etc/ and
        // usr/ paths are not host filesystem destinations. AppDirs are kept
        // intact, so legacy keep/exclude decisions no longer apply.
        if (!release.payloadRules.isEmpty()) {
            release.payloadRules.clear();
            changed = true;
        }
        for (auto &entry : release.payload) {
            if (!entry.reviewReason.isEmpty() || entry.requiresReview) {
                entry.reviewReason.clear();
                entry.requiresReview = false;
                changed = true;
            }
        }
        return changed;
    }
    for (auto &entry : release.payload) {
        auto reason = PathSafety::reviewReason(entry.path);
        if (!entry.symlinkTarget.isEmpty()) {
            const auto symlinkReason =
                PathSafety::symlinkReviewReason(entry.path, entry.symlinkTarget);
            if (!symlinkReason.isEmpty()) {
                if (!reason.isEmpty()) reason += QStringLiteral("; ");
                reason += symlinkReason;
            }
        }
        const bool review = !reason.isEmpty() && entry.type != QStringLiteral("directory");
        if (entry.reviewReason != reason || entry.requiresReview != review) {
            entry.reviewReason = reason;
            entry.requiresReview = review;
            changed = true;
        }
    }
    return changed;
}

QString desktopField(const QString &contents, const QString &name) {
    return desktopEntryField(contents, name);
}

bool migrateAppImageIntegration(PackageRelease &release) {
    if (release.sourceType != SourcePackageType::AppImage) return false;
    bool changed = false;

    QList<LauncherMapping> launchers;
    const auto appRun = std::find_if(
        release.installMapping.launchers.cbegin(),
        release.installMapping.launchers.cend(),
        [](const auto &launcher) { return launcher.sourcePath == QStringLiteral("AppRun"); });
    if (appRun != release.installMapping.launchers.cend()) {
        auto launcher = *appRun;
        launcher.enabled = true;
        launcher.kind = LauncherKind::Wrapper;
        launcher.missing = false;
        launchers.append(launcher);
    }
    const bool launcherChanged =
        release.installMapping.launchers.size() != launchers.size() ||
        (!launchers.isEmpty() &&
         (release.installMapping.launchers.first().sourcePath != launchers.first().sourcePath ||
          release.installMapping.launchers.first().enabled != launchers.first().enabled ||
          release.installMapping.launchers.first().kind != launchers.first().kind ||
          release.installMapping.launchers.first().missing != launchers.first().missing));
    if (launcherChanged) {
        release.installMapping.launchers = launchers;
        changed = true;
    }

    const auto isIntegrationPath = [](const QString &path) {
        if (path.isEmpty() || !path.contains(QLatin1Char('/'))) return true;
        const auto prefix = QStringLiteral("usr/share/applications/");
        return path.startsWith(prefix) &&
               !path.sliced(prefix.size()).contains(QLatin1Char('/'));
    };
    QList<DesktopEntryConfiguration> desktops;
    QSet<QString> destinations;
    const bool hasTopLevel = std::any_of(
        release.installMapping.desktopEntries.cbegin(),
        release.installMapping.desktopEntries.cend(),
        [&](const auto &desktop) {
            return !desktop.userModified && !desktop.sourcePath.isEmpty() &&
                   !desktop.sourcePath.contains(QLatin1Char('/'));
        });
    for (auto desktop : std::as_const(release.installMapping.desktopEntries)) {
        const bool vendor = !desktop.sourcePath.isEmpty() && !desktop.userModified;
        const bool application = desktopField(desktop.contents, QStringLiteral("Type")) ==
                                     QStringLiteral("Application") &&
                                 !desktopField(desktop.contents, QStringLiteral("Exec")).isEmpty();
        const bool noDisplay = desktopField(desktop.contents, QStringLiteral("NoDisplay"))
                                   .compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0;
        if (vendor && (!isIntegrationPath(desktop.sourcePath) || !application || noDisplay)) {
            changed = true;
            continue;
        }
        if (!desktop.destination.isEmpty() && destinations.contains(desktop.destination)) {
            changed = true;
            continue;
        }
        if (vendor && hasTopLevel && desktop.sourcePath.contains(QLatin1Char('/')) &&
            desktop.enabled) {
            desktop.enabled = false;
            changed = true;
        }
        if (!desktop.destination.isEmpty()) destinations.insert(desktop.destination);
        desktops.append(std::move(desktop));
    }
    release.installMapping.desktopEntries = std::move(desktops);
    return changed;
}

bool migrateSigningTrust(PackageRelease &release, const std::filesystem::path &directory) {
    bool changed = false;
    if (release.update.signingKeys.isEmpty()) {
        auto candidates = ScriptEvidenceAnalyzer::analyze(release.maintainerScripts).signingKeys;
        candidates.append(payloadRepositoryEvidence(release).signingKeys);
        for (const auto &candidate : candidates) {
            QString keyError;
            const auto key = RepositoryTrust::storeVendorKey(directory, candidate, &keyError);
            if (!key) continue;
            const auto duplicate = std::any_of(release.update.signingKeys.cbegin(),
                                               release.update.signingKeys.cend(),
                                               [&](const auto &stored) {
                                                   return stored.sha256 == key->sha256;
                                               });
            if (!duplicate) {
                release.update.signingKeys.append(*key);
                changed = true;
            }
        }
    }
    if (!release.update.signingKeys.isEmpty() &&
        (release.update.aptSigningKeyring.isEmpty() ||
         release.update.trustedSigningFingerprint.isEmpty())) {
        const auto &key = release.update.signingKeys.first();
        if (!key.fingerprints.isEmpty()) {
            release.update.aptSigningKeyring = key.relativePath;
            release.update.trustedSigningFingerprint = key.fingerprints.first();
            changed = true;
        }
    }
    return changed;
}

bool applyCurrentMappings(PackageRelease &release) {
    return DependencyParser::applyVerifiedMappings(
        release.dependencies, DependencyParser::loadVerifiedMappings());
}

const PackageRelease *newestPreparedRelease(const Project &project) {
    const PackageRelease *result = nullptr;
    for (const auto &candidate : project.releases) {
        if (candidate.state == ReleaseState::Discovered ||
            candidate.state == ReleaseState::Preparing) continue;
        if (result == nullptr ||
            compareReleaseVersions(candidate, *result) > 0) {
            result = &candidate;
        }
    }
    return result;
}

bool repairSloganDisplayName(Project &project) {
    const auto *release = newestPreparedRelease(project);
    if (release == nullptr && !project.releases.isEmpty()) release = &project.releases.first();
    if (release == nullptr) return false;
    const auto synopsis = release->debian.description.section(QLatin1Char('\n'), 0, 0).trimmed();
    if (synopsis.isEmpty() || project.displayName != synopsis) return false;
    const auto preferred = preferredDisplayName(release->debian,
                                                release->installMapping.desktopEntries);
    if (preferred.isEmpty() || preferred == project.displayName) return false;
    project.displayName = preferred;
    for (auto &item : project.releases) {
        if (item.displayName == synopsis) item.displayName = preferred;
    }
    return true;
}

void carryForward(const PackageRelease &previous, PackageRelease &next,
                  const QString &previousPkgbuild) {
    for (auto &dependency : next.dependencies) {
        const auto old = std::find_if(previous.dependencies.cbegin(), previous.dependencies.cend(),
                                      [&](const auto &candidate) {
                                          return candidate.rawExpression == dependency.rawExpression;
                                      });
        if (old != previous.dependencies.cend()) dependency = *old;
    }
    for (auto &script : next.maintainerScripts) {
        const auto old = std::find_if(previous.maintainerScripts.cbegin(),
                                      previous.maintainerScripts.cend(), [&](const auto &candidate) {
                                          return candidate.name == script.name &&
                                                 candidate.contentFingerprint() == script.contentFingerprint();
                                      });
        if (old != previous.maintainerScripts.cend()) {
            script.acknowledgedFingerprint = old->acknowledgedFingerprint;
        }
    }
    for (auto &finding : next.scriptFindings) {
        const auto old = std::find_if(previous.scriptFindings.cbegin(),
                                      previous.scriptFindings.cend(), [&](const auto &candidate) {
                                          return candidate.evidenceFingerprint == finding.evidenceFingerprint;
                                      });
        if (old != previous.scriptFindings.cend()) finding = *old;
    }
    for (const auto &rule : previous.payloadRules) {
        const auto oldFingerprint = PayloadReview::fingerprint(previous, rule.path);
        const auto newFingerprint = PayloadReview::fingerprint(next, rule.path);
        if (oldFingerprint.isEmpty() || newFingerprint.isEmpty() ||
            rule.acknowledgedFingerprint != oldFingerprint) {
            continue;
        }
        auto nextRule = std::find_if(next.payloadRules.begin(), next.payloadRules.end(),
                                     [&rule](const auto &candidate) {
                                         return candidate.path == rule.path;
                                     });
        if (nextRule == next.payloadRules.end()) next.payloadRules.append(rule);
        else *nextRule = rule;
        if (oldFingerprint != newFingerprint) {
            for (auto &entry : next.payload) {
                if (entry.type == QStringLiteral("directory") ||
                    !payloadPathCovers(rule.path, entry.path)) {
                    continue;
                }
                entry.requiresReview = true;
                const auto changedReason = QStringLiteral(
                    "Content changed since the exact payload decision made for the previous release");
                if (entry.reviewReason.isEmpty()) entry.reviewReason = changedReason;
                else if (!entry.reviewReason.contains(changedReason)) {
                    entry.reviewReason += QStringLiteral("; ") + changedReason;
                }
            }
        }
    }
    auto oldSources = previous.lifecycleScript.sourceFingerprints;
    auto newSources = QStringList{};
    for (const auto &finding : next.scriptFindings) {
        if (finding.disposition == ScriptDisposition::LifecycleRequired) {
            newSources.append(finding.evidenceFingerprint);
        }
    }
    oldSources.sort();
    newSources.sort();
    if (!previous.lifecycleScript.contents.isEmpty() && oldSources == newSources) {
        next.lifecycleScript = previous.lifecycleScript;
    }
    if (previous.sourceType == next.sourceType) {
        if (next.sourceType == SourcePackageType::Archive) {
            next.installMapping.archiveLayout = previous.installMapping.archiveLayout;
            if (!previous.installMapping.optDirectory.isEmpty()) {
                next.installMapping.optDirectory = previous.installMapping.optDirectory;
            }
            if (!previous.installMapping.binaryDestination.isEmpty()) {
                next.installMapping.binaryDestination = previous.installMapping.binaryDestination;
            }
            next.installMapping.executableLinks = previous.installMapping.executableLinks;
            next.installMapping.commonPrefix = previous.installMapping.commonPrefix;
            next.installMapping.stripCommonPrefix = previous.installMapping.stripCommonPrefix;
            const auto priorPathStillExists = std::any_of(
                next.payload.cbegin(), next.payload.cend(), [&](const auto &entry) {
                    return entry.path == previous.installMapping.binarySourcePath;
                });
            if (priorPathStillExists) {
                next.installMapping.binarySourcePath = previous.installMapping.binarySourcePath;
            }
        } else if (next.sourceType == SourcePackageType::ElfBinary &&
                   !previous.installMapping.binaryDestination.isEmpty()) {
            next.installMapping.binaryDestination = previous.installMapping.binaryDestination;
        }
    }
    // Integration choices are release-owned. Untouched detected content at the
    // same path refreshes from the new artifact; user-edited/custom content is
    // carried byte-for-byte and is marked missing instead of silently rebound.
    for (const auto &prior : previous.installMapping.launchers) {
        if (next.sourceType == SourcePackageType::AppImage &&
            prior.sourcePath != QStringLiteral("AppRun")) {
            continue;
        }
        auto current = std::find_if(next.installMapping.launchers.begin(),
                                    next.installMapping.launchers.end(),
                                    [&](const auto &candidate) {
                                        return candidate.sourcePath == prior.sourcePath;
                                    });
        const bool exists = std::any_of(next.payload.cbegin(), next.payload.cend(),
                                        [&](const auto &entry) {
                                            return entry.path == prior.sourcePath;
                                        });
        auto carried = prior;
        carried.missing = !exists;
        if (current == next.installMapping.launchers.end()) {
            next.installMapping.launchers.append(carried);
        } else {
            *current = carried;
        }
    }
    for (const auto &prior : previous.installMapping.desktopEntries) {
        auto current = std::find_if(next.installMapping.desktopEntries.begin(),
                                    next.installMapping.desktopEntries.end(),
                                    [&](const auto &candidate) {
                                        return !prior.sourcePath.isEmpty() &&
                                               candidate.sourcePath == prior.sourcePath;
                                    });
        const bool exists = prior.sourcePath.isEmpty() ||
            std::any_of(next.payload.cbegin(), next.payload.cend(), [&](const auto &entry) {
                return entry.path == prior.sourcePath;
            });
        if (current != next.installMapping.desktopEntries.end() && !prior.userModified) {
            current->enabled = prior.enabled;
            current->destination = prior.destination;
            current->missing = false;
        } else {
            auto carried = prior;
            carried.missing = !exists;
            if (current == next.installMapping.desktopEntries.end()) {
                next.installMapping.desktopEntries.append(carried);
            } else {
                *current = carried;
            }
        }
    }
    const auto &priorIcon = previous.installMapping.icon;
    if (priorIcon.sourceKind != IconSourceKind::None) {
        const bool payloadStillExists = priorIcon.sourceKind != IconSourceKind::Payload ||
            std::any_of(next.payload.cbegin(), next.payload.cend(), [&](const auto &entry) {
                return entry.path == priorIcon.sourcePath;
            });
        if (priorIcon.provenance.origin == ValueOrigin::User ||
            priorIcon.sourceKind != IconSourceKind::Payload || payloadStillExists) {
            next.installMapping.icon = priorIcon;
            next.installMapping.icon.missing = !payloadStillExists;
        }
    }
    if (previous.installMapping.appRun.userModified) {
        auto carried = previous.installMapping.appRun;
        carried.originalContents = next.installMapping.appRun.originalContents;
        carried.originalContentsSha256 = next.installMapping.appRun.originalContentsSha256;
        carried.present = next.installMapping.appRun.present || carried.present;
        next.installMapping.appRun = carried;
    } else if (next.installMapping.appRun.present &&
               previous.installMapping.appRun.acknowledgedFingerprint ==
                   next.installMapping.appRun.contentFingerprint()) {
        next.installMapping.appRun.acknowledgedFingerprint =
            previous.installMapping.appRun.acknowledgedFingerprint;
    }
    if (previous.pkgbuildManuallyModified) next.previousManualPkgbuild = previousPkgbuild;
}

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
        if (command.isEmpty()) continue;
        if (!exposed.contains(command.toLower())) return true;
    }
    return false;
}

bool releaseNeedsReview(const PackageRelease &release) {
    if (release.installMapping.appRun.requiresReview() ||
        release.installMapping.icon.missing || archiveDesktopCommandUnmapped(release) ||
        std::any_of(release.installMapping.launchers.cbegin(),
                    release.installMapping.launchers.cend(),
                    [](const auto &launcher) { return launcher.enabled && launcher.missing; }) ||
        std::any_of(release.installMapping.desktopEntries.cbegin(),
                    release.installMapping.desktopEntries.cend(),
                    [](const auto &desktop) { return desktop.enabled && desktop.missing; })) {
        return true;
    }
    if (std::any_of(release.dependencies.cbegin(), release.dependencies.cend(),
                    [](const auto &dependency) {
                        return dependency.status == MappingStatus::Unresolved;
                    })) return true;
    if (release.sourceType != SourcePackageType::AppImage) {
        for (const auto &entry : release.payload) {
            if (entry.requiresReview && PayloadReview::state(release, entry).needsReview) return true;
        }
    }
    if (!release.lifecycleScript.contents.isEmpty() &&
        (!release.lifecycleScript.validationPassed ||
         release.lifecycleScript.requiresAcknowledgement())) return true;
    for (const auto &finding : release.scriptFindings) {
        const auto script = std::find_if(release.maintainerScripts.cbegin(),
                                         release.maintainerScripts.cend(),
                                         [&](const auto &candidate) {
                                             return candidate.name == finding.scriptName;
                                         });
        if (script != release.maintainerScripts.cend() && !script->requiresReview()) continue;
        if (finding.disposition == ScriptDisposition::Unresolved) return true;
        if (finding.disposition == ScriptDisposition::LifecycleRequired &&
            (!release.lifecycleScript.validationPassed ||
             !release.lifecycleScript.sourceFingerprints.contains(finding.evidenceFingerprint))) {
            return true;
        }
    }
    return false;
}

} // namespace pacsmith::project_store_internal
