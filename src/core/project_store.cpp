#include "core/project_store.hpp"

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
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>

#include <algorithm>
#include <array>

namespace pacsmith {
namespace {

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

QString sourceDisplayName(const DebianMetadata &metadata) {
    const auto first = metadata.description.section(QLatin1Char('\n'), 0, 0).trimmed();
    return !first.isEmpty() && first.size() <= 80 ? first : metadata.package;
}

QString sourceIdentity(const DebianMetadata &metadata) {
    return QStringLiteral("deb:%1:%2").arg(metadata.package.toLower(), metadata.architecture.toLower());
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
        const auto reason = PathSafety::reviewReason(entry.path);
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
    const QRegularExpression expression(
        QStringLiteral("(?m)^%1=(.*)$").arg(QRegularExpression::escape(name)));
    return expression.match(contents).captured(1).trimmed();
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
    if (previous.pkgbuildManuallyModified) next.previousManualPkgbuild = previousPkgbuild;
}

bool releaseNeedsReview(const PackageRelease &release) {
    if (release.installMapping.icon.missing ||
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

} // namespace

ProjectStore::ProjectStore(std::filesystem::path projectsRoot) : root_(std::move(projectsRoot)) {}

std::filesystem::path ProjectStore::defaultProjectsRoot() {
    const auto xdg = qEnvironmentVariable("XDG_DATA_HOME");
    if (!xdg.isEmpty() && QDir::isAbsolutePath(xdg)) {
        return pathFromQString(QDir(xdg).filePath(QStringLiteral("pacsmith/projects")));
    }
    return pathFromQString(QDir::home().filePath(QStringLiteral(".local/share/pacsmith/projects")));
}

const std::filesystem::path &ProjectStore::projectsRoot() const noexcept { return root_; }

std::filesystem::path ProjectStore::projectPath(const QString &id) const {
    return root_ / pathFromQString(id);
}

std::filesystem::path ProjectStore::releasePath(const QString &projectId,
                                                const QString &releaseIdValue) const {
    return projectPath(projectId) / "releases" / pathFromQString(releaseIdValue);
}

std::filesystem::path ProjectStore::releasePath(const PackageRelease &release) const {
    return releasePath(release.projectId, release.id);
}

std::filesystem::path ProjectStore::pkgbuildPath(const PackageRelease &release) const {
    return releasePath(release) / "PKGBUILD";
}

std::filesystem::path ProjectStore::sourcePath(const PackageRelease &release) const {
    return releasePath(release) / "sources" / pathFromQString(release.originalSourceFilename);
}

std::filesystem::path ProjectStore::iconPath(const Project &project) const {
    const auto safe = PathSafety::normalizedArchivePath(project.iconPath);
    if (!safe) return {};
    return projectPath(project.id) / pathFromQString(*safe);
}

std::filesystem::path ProjectStore::releaseIconPath(const PackageRelease &release) const {
    const auto safe = PathSafety::normalizedArchivePath(release.iconPath);
    if (!safe || !safe->startsWith(QStringLiteral("files/"))) return {};
    return releasePath(release) / pathFromQString(*safe);
}

std::filesystem::path ProjectStore::lifecyclePath(const PackageRelease &release) const {
    return releasePath(release) / pathFromQString(release.lifecycleScript.fileName);
}

bool ProjectStore::synchronizeIntegrationSources(const PackageRelease &release,
                                                 QString *error) const {
    const auto &icon = release.installMapping.icon;
    if (icon.missing || icon.projectPath.isEmpty() || icon.sha256.isEmpty()) return true;
    const auto safe = PathSafety::normalizedArchivePath(icon.projectPath);
    if (!safe || !safe->startsWith(QStringLiteral("files/"))) {
        if (error != nullptr) *error = QStringLiteral("Integration icon path is unsafe");
        return false;
    }
    const auto extension = icon.format.isEmpty()
        ? QFileInfo(*safe).suffix().toLower() : icon.format.toLower();
    if (extension != QStringLiteral("png") && extension != QStringLiteral("svg") &&
        extension != QStringLiteral("xpm")) {
        if (error != nullptr) *error = QStringLiteral("Integration icon format is unsupported");
        return false;
    }
    const auto source = releasePath(release) / pathFromQString(*safe);
    QString hashError;
    if (!std::filesystem::is_regular_file(source) ||
        sha256File(source, &hashError) != icon.sha256) {
        if (error != nullptr) {
            *error = hashError.isEmpty()
                ? QStringLiteral("Integration icon is missing or no longer matches its recorded SHA256")
                : hashError;
        }
        return false;
    }
    const auto relativeSource = pathFromQString(*safe);
    const auto alias = releasePath(release) /
                       pathFromQString(QStringLiteral("pacsmith-icon.%1").arg(extension));
    std::error_code filesystemError;
    if (std::filesystem::is_symlink(alias, filesystemError) && !filesystemError &&
        std::filesystem::read_symlink(alias, filesystemError) == relativeSource &&
        !filesystemError) {
        return true;
    }
    filesystemError.clear();
    if (std::filesystem::exists(alias, filesystemError) ||
        std::filesystem::is_symlink(alias, filesystemError)) {
        std::filesystem::remove(alias, filesystemError);
        if (filesystemError) {
            if (error != nullptr) *error = QString::fromStdString(filesystemError.message());
            return false;
        }
    }
    std::filesystem::create_symlink(relativeSource, alias, filesystemError);
    if (!filesystemError) return true;
    filesystemError.clear();
    return copyFileAtomically(source, alias, error);
}

QList<Project> ProjectStore::list(QString *error) const {
    QList<Project> result;
    const QDir root(qStringFromPath(root_));
    if (!root.exists()) return result;
    const auto directories = root.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const auto &directory : directories) {
        QString loadError;
        auto project = loadById(directory, &loadError);
        if (project) result.append(std::move(*project));
        else if (error != nullptr && error->isEmpty()) *error = loadError;
    }
    return result;
}

std::optional<Project> ProjectStore::loadById(const QString &id, QString *error) const {
    QFile file(qStringFromPath(projectPath(id) / "project.json"));
    if (!file.open(QIODevice::ReadOnly)) {
        if (error != nullptr) *error = file.errorString();
        return std::nullopt;
    }
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (error != nullptr) *error = parseError.errorString();
        return std::nullopt;
    }
    const auto object = document.object();
    if (object.value(QStringLiteral("formatVersion")).toInt(1) < 4) {
        return migrateLegacyProject(id, object, error);
    }

    auto project = Project::fromJson(object);
    bool changed = project.formatVersion < 5;
    project.formatVersion = 5;
    for (const auto &entry : object.value(QStringLiteral("releaseIds")).toArray()) {
        const auto releaseIdValue = entry.toString();
        if (!validId(releaseIdValue)) continue;
        QFile releaseFile(qStringFromPath(releasePath(id, releaseIdValue) / "release.json"));
        if (!releaseFile.open(QIODevice::ReadOnly)) {
            if (error != nullptr) {
                *error = QStringLiteral("Could not load release %1: %2")
                             .arg(releaseIdValue, releaseFile.errorString());
            }
            return std::nullopt;
        }
        QJsonParseError releaseError;
        const auto releaseDocument = QJsonDocument::fromJson(releaseFile.readAll(), &releaseError);
        if (releaseError.error != QJsonParseError::NoError || !releaseDocument.isObject()) {
            if (error != nullptr) *error = releaseError.errorString();
            return std::nullopt;
        }
        auto release = PackageRelease::fromJson(releaseDocument.object());
        release.projectId = project.id;
        if (release.acquisition.canonicalIdentity.isEmpty()) {
            release.sourceType = SourcePackageType::Debian;
            release.acquisition.kind = release.update.strategy == UpdateStrategy::AptRepository
                ? AcquisitionKind::AptRepository
                : release.update.strategy == UpdateStrategy::RpmRepository
                    ? AcquisitionKind::RpmRepository : AcquisitionKind::LocalFile;
            release.acquisition.canonicalIdentity = sourceIdentity(release.debian);
            release.acquisition.originalUrl = release.sourceUrl;
            changed = true;
        }
        changed = populateAptCandidates(release) || changed;
        changed = migrateScriptEvidence(release) || changed;
        changed = migratePayloadMetadata(release) || changed;
        changed = migrateSigningTrust(release, releasePath(release)) || changed;
        bool mappingsChanged = migrateAppImageIntegration(release);
        mappingsChanged = applyCurrentMappings(release) || mappingsChanged;
        if (!release.iconPath.isEmpty() &&
            release.installMapping.icon.projectPath.isEmpty()) {
            release.installMapping.icon.sourceKind = release.iconSourcePath.isEmpty()
                ? IconSourceKind::LocalFile : IconSourceKind::Payload;
            release.installMapping.icon.sourcePath = release.iconSourcePath;
            release.installMapping.icon.projectPath = release.iconPath;
            release.installMapping.icon.sha256 = release.iconSha256;
            release.installMapping.icon.format = QFileInfo(release.iconPath).suffix().toLower();
            release.installMapping.icon.iconName = release.debian.package.isEmpty()
                ? release.archPackageName : release.debian.package;
            release.installMapping.icon.provenance.origin = ValueOrigin::Deterministic;
            release.installMapping.icon.provenance.rationale = QStringLiteral(
                "Migrated the previously selected project icon into the integration recipe");
            mappingsChanged = true;
        }
        changed = mappingsChanged || changed;
        const auto pkgbuild = readPkgbuild(release, nullptr);
        if (pkgbuild) {
            release.pkgbuildManuallyModified =
                sha256Hex(pkgbuild->toUtf8()) != release.generatedPkgbuildSha256;
        }
        const bool generatedRecipeNeedsMigration =
            !release.generatedPkgbuild.contains(QStringLiteral("xdata=(")) ||
            (release.sourceType == SourcePackageType::AppImage &&
             !release.generatedPkgbuild.contains(
                 QStringLiteral("Preserve the complete AppDir below /opt")));
        if (mappingsChanged || project.formatVersion < 5 || generatedRecipeNeedsMigration) {
            release.generatedPkgbuild = PkgbuildGenerator::generate(release);
            release.generatedPkgbuildSha256 = sha256Hex(release.generatedPkgbuild.toUtf8());
            if (!release.pkgbuildManuallyModified &&
                !writeBytes(pkgbuildPath(release), release.generatedPkgbuild.toUtf8(), error)) {
                return std::nullopt;
            }
        }
        project.releases.append(std::move(release));
    }
    changed = reconcileInstalled(project, nullptr) || changed;
    if (changed && !save(project, error)) return std::nullopt;
    return project;
}

std::optional<Project> ProjectStore::load(const QString &idOrName, QString *error) const {
    if (validId(idOrName) && std::filesystem::exists(projectPath(idOrName) / "project.json")) {
        return loadById(idOrName, error);
    }
    const auto projects = list(error);
    for (const auto &project : projects) {
        if (project.displayName.compare(idOrName, Qt::CaseInsensitive) == 0 ||
            project.archPackageName.compare(idOrName, Qt::CaseInsensitive) == 0) {
            return project;
        }
    }
    if (error != nullptr) *error = QStringLiteral("Project not found: %1").arg(idOrName);
    return std::nullopt;
}

bool ProjectStore::save(Project &project, QString *error) const {
    if (!validId(project.id)) {
        if (error != nullptr) *error = QStringLiteral("Invalid project ID: %1").arg(project.id);
        return false;
    }
    std::error_code filesystemError;
    const auto directory = projectPath(project.id);
    std::filesystem::create_directories(directory / "releases", filesystemError);
    if (filesystemError) {
        if (error != nullptr) *error = QString::fromStdString(filesystemError.message());
        return false;
    }
    project.formatVersion = 5;
    project.modifiedAt = QDateTime::currentDateTimeUtc();
    for (auto &release : project.releases) {
        if (!validId(release.id)) {
            if (error != nullptr) *error = QStringLiteral("Invalid release ID: %1").arg(release.id);
            return false;
        }
        release.projectId = project.id;
        release.formatVersion = std::max(2, release.formatVersion);
        if (release.state != ReleaseState::Discovered && release.state != ReleaseState::Preparing) {
            release.state = release.buildStatus == BuildStatus::Succeeded
                ? ReleaseState::Built
                : releaseNeedsReview(release) ? ReleaseState::NeedsReview : ReleaseState::Ready;
        }
        release.modifiedAt = project.modifiedAt;
        std::filesystem::create_directories(releasePath(release), filesystemError);
        if (filesystemError) {
            if (error != nullptr) *error = QString::fromStdString(filesystemError.message());
            return false;
        }
        if (!synchronizeIntegrationSources(release, error)) return false;
        if (!writeBytes(releasePath(release) / "release.json",
                        QJsonDocument(release.toJson()).toJson(QJsonDocument::Indented), error)) {
            return false;
        }
    }
    return writeBytes(directory / "project.json",
                      QJsonDocument(project.toJson()).toJson(QJsonDocument::Indented), error);
}

std::optional<Project> ProjectStore::migrateLegacyProject(const QString &id,
                                                          const QJsonObject &legacyObject,
                                                          QString *error) const {
    auto release = PackageRelease::fromJson(legacyObject);
    const auto oldInstalled = legacyObject.value(QStringLiteral("installedVersion")).toString();
    release.projectId = id;
    release.id = releaseId(release.debian.version, release.sourceSha256);
    release.formatVersion = 1;

    Project project;
    project.id = id;
    project.displayName = release.displayName;
    project.archPackageName = release.archPackageName;
    project.vendorName = release.vendorName;
    project.sourceIdentity = sourceIdentity(release.debian);
    project.createdAt = release.createdAt;
    project.modifiedAt = release.modifiedAt;
    project.installedVersion = oldInstalled;

    const auto directory = projectPath(id);
    const auto finalDirectory = releasePath(release);
    if (!std::filesystem::exists(finalDirectory / "release.json")) {
        const auto staging = directory / "releases" /
                             pathFromQString(QStringLiteral(".migrate-%1").arg(release.id));
        std::error_code filesystemError;
        std::filesystem::create_directories(staging, filesystemError);
        if (filesystemError) {
            if (error != nullptr) *error = QString::fromStdString(filesystemError.message());
            return std::nullopt;
        }
        const auto backup = directory / "project.v3.json.bak";
        if (!std::filesystem::exists(backup) &&
            !writeBytes(backup, QJsonDocument(legacyObject).toJson(QJsonDocument::Indented), error)) {
            return std::nullopt;
        }

        QList<std::pair<std::filesystem::path, std::filesystem::path>> moved;
        auto moveEntry = [&](const std::filesystem::path &source,
                             const std::filesystem::path &target) {
            if (!std::filesystem::exists(source) && !std::filesystem::is_symlink(source)) return true;
            std::error_code moveError;
            std::filesystem::rename(source, target, moveError);
            if (moveError) {
                if (error != nullptr) *error = QString::fromStdString(moveError.message());
                return false;
            }
            moved.append({source, target});
            return true;
        };
        bool moveOk = true;
        for (const auto *name : {"sources", "files", "patches", "build", "history"}) {
            moveOk = moveOk && moveEntry(directory / name, staging / name);
        }
        moveOk = moveOk && moveEntry(directory / "PKGBUILD", staging / "PKGBUILD");
        if (!release.originalSourceFilename.isEmpty()) {
            moveOk = moveOk && moveEntry(directory / pathFromQString(release.originalSourceFilename),
                                         staging / pathFromQString(release.originalSourceFilename));
        }
        if (!release.lifecycleScript.fileName.isEmpty()) {
            moveOk = moveOk && moveEntry(directory / pathFromQString(release.lifecycleScript.fileName),
                                         staging / pathFromQString(release.lifecycleScript.fileName));
        }
        const QDir oldDirectory(qStringFromPath(directory));
        for (const auto &artifact : oldDirectory.entryInfoList(
                 QStringList{QStringLiteral("*.pkg.tar.*")}, QDir::Files)) {
            moveOk = moveOk && moveEntry(pathFromQString(artifact.absoluteFilePath()),
                                         staging / pathFromQString(artifact.fileName()));
        }
        if (!moveOk) {
            for (auto iterator = moved.crbegin(); iterator != moved.crend(); ++iterator) {
                std::error_code rollbackError;
                std::filesystem::rename(iterator->second, iterator->first, rollbackError);
            }
            return std::nullopt;
        }
        for (auto &artifact : release.producedPackages) {
            artifact = qStringFromPath(finalDirectory / pathFromQString(QFileInfo(artifact).fileName()));
        }
        if (!writeBytes(staging / "release.json",
                        QJsonDocument(release.toJson()).toJson(QJsonDocument::Indented), error)) {
            return std::nullopt;
        }
        std::filesystem::rename(staging, finalDirectory, filesystemError);
        if (filesystemError) {
            if (error != nullptr) *error = QString::fromStdString(filesystemError.message());
            return std::nullopt;
        }
    } else {
        QFile releaseFile(qStringFromPath(finalDirectory / "release.json"));
        if (releaseFile.open(QIODevice::ReadOnly)) {
            release = PackageRelease::fromJson(
                QJsonDocument::fromJson(releaseFile.readAll()).object());
            release.projectId = id;
        }
    }

    static_cast<void>(populateAptCandidates(release));
    static_cast<void>(migrateScriptEvidence(release));
    static_cast<void>(migratePayloadMetadata(release));
    static_cast<void>(migrateAppImageIntegration(release));
    static_cast<void>(migrateSigningTrust(release, finalDirectory));

    if (!release.iconPath.isEmpty()) {
        project.iconPath = QStringLiteral("releases/%1/%2").arg(release.id, release.iconPath);
        project.iconSourcePath = release.iconSourcePath;
        project.iconSha256 = release.iconSha256;
    }
    project.releases.append(release);
    project.history.append({QDateTime::currentDateTimeUtc(), QStringLiteral("migration"),
                            QStringLiteral("Migrated the original package into release %1")
                                .arg(release.debian.version)});
    static_cast<void>(reconcileInstalled(project, nullptr));
    if (!save(project, error)) return std::nullopt;
    return project;
}

bool ProjectStore::reconcileInstalled(Project &project, QString *error) const {
    QString queryError;
    const auto installed = queryInstalledVersion(project.archPackageName, &queryError);
    if (!installed) {
        if (error != nullptr) *error = queryError;
        return false;
    }
    const auto previousVersion = project.installedVersion;
    const auto previousRelease = project.installedReleaseId;
    const auto previousExternal = project.externallyInstalled;
    project.installedVersion = *installed;
    project.installedReleaseId.clear();
    project.externallyInstalled = false;
    if (!installed->isEmpty()) {
        if (const auto managed = ManagedPackageRegistry::find(project.archPackageName, nullptr);
            managed && managed->projectId() == project.id &&
            project.release(managed->releaseId()) != nullptr) {
            project.installedReleaseId = managed->releaseId();
        }
        for (const auto &release : project.releases) {
            if (!project.installedReleaseId.isEmpty()) break;
            bool matches = expectedArchVersion(release) == *installed;
            for (const auto &build : release.builds) {
                matches = matches || std::any_of(build.artifacts.cbegin(), build.artifacts.cend(),
                                                  [&](const auto &artifact) {
                                                      return artifact.packageName == project.archPackageName &&
                                                             artifact.packageVersion == *installed;
                                                  });
            }
            if (matches) {
                project.installedReleaseId = release.id;
                break;
            }
        }
        project.externallyInstalled = project.installedReleaseId.isEmpty();
    }
    return previousVersion != project.installedVersion ||
           previousRelease != project.installedReleaseId ||
           previousExternal != project.externallyInstalled;
}

bool ProjectStore::deleteProject(const Project &project, QString *error) const {
    QString statusError;
    const auto installed = queryInstalledVersion(project.archPackageName, &statusError);
    if (!installed) {
        if (error != nullptr) *error = QStringLiteral("Could not verify package state: %1").arg(statusError);
        return false;
    }
    if (!installed->isEmpty()) {
        if (error != nullptr) {
            *error = QStringLiteral("%1 is installed (%2). Uninstall it before deleting this project.")
                         .arg(project.archPackageName, *installed);
        }
        return false;
    }
    const auto directory = projectPath(project.id);
    std::error_code filesystemError;
    const auto canonicalRoot = std::filesystem::weakly_canonical(root_, filesystemError);
    const auto canonicalParent = std::filesystem::weakly_canonical(directory.parent_path(), filesystemError);
    if (filesystemError || canonicalParent != canonicalRoot ||
        std::filesystem::is_symlink(std::filesystem::symlink_status(directory, filesystemError))) {
        if (error != nullptr) *error = QStringLiteral("Unsafe project deletion target");
        return false;
    }
    const auto removed = std::filesystem::remove_all(directory, filesystemError);
    if (filesystemError || removed == 0) {
        if (error != nullptr) *error = filesystemError
            ? QString::fromStdString(filesystemError.message())
            : QStringLiteral("No project files were removed");
        return false;
    }
    return true;
}

bool ProjectStore::deleteRelease(Project &project, const QString &releaseIdValue,
                                 QString *error) const {
    if (releaseIdValue == project.installedReleaseId) {
        if (error != nullptr) *error = QStringLiteral("The installed release cannot be deleted");
        return false;
    }
    const auto iterator = std::find_if(project.releases.begin(), project.releases.end(),
                                       [&](const auto &release) {
                                           return release.id == releaseIdValue;
                                       });
    if (iterator == project.releases.end()) {
        if (error != nullptr) *error = QStringLiteral("Release not found: %1").arg(releaseIdValue);
        return false;
    }
    const auto directory = releasePath(*iterator);
    const auto tombstone = directory.parent_path() /
                           pathFromQString(QStringLiteral(".delete-%1").arg(releaseIdValue));
    std::error_code filesystemError;
    std::filesystem::rename(directory, tombstone, filesystemError);
    if (filesystemError) {
        if (error != nullptr) *error = QString::fromStdString(filesystemError.message());
        return false;
    }
    const auto removedRelease = *iterator;
    project.releases.erase(iterator);
    if (!save(project, error)) {
        project.releases.append(removedRelease);
        filesystemError.clear();
        std::filesystem::rename(tombstone, directory, filesystemError);
        return false;
    }
    std::filesystem::remove_all(tombstone, filesystemError);
    if (filesystemError && error != nullptr) *error = QString::fromStdString(filesystemError.message());
    return !filesystemError;
}

std::optional<ImportResult> ProjectStore::importDeb(const std::filesystem::path &debPath,
                                                    QString *error,
                                                    const ImportProgressCallback &progress,
                                                    const ImportOptions &options) const {
    if (progress) progress({ImportStage::ValidatingSource, 0});
    AnalysisError analysisError;
    const auto analysis = DebAnalyzer{}.analyze(debPath, analysisError, progress);
    if (!analysis) {
        if (error != nullptr) *error = analysisError.message;
        return std::nullopt;
    }
    const auto sourceHash = sha256File(debPath, error);
    if (sourceHash.isEmpty()) return std::nullopt;

    if (progress) progress({ImportStage::PreparingProject, 0});
    const auto identity = !options.acquisition.canonicalIdentity.isEmpty()
        ? options.acquisition.canonicalIdentity
        : options.acquisition.kind == AcquisitionKind::GitHubRelease
            ? QStringLiteral("github:%1/%2")
                  .arg(options.acquisition.githubOwner.toLower(),
                       options.acquisition.githubRepository.toLower())
            : sourceIdentity(analysis->metadata);
    std::optional<Project> matching;
    for (auto existing : list()) {
        if (existing.sourceIdentity == identity) {
            matching = std::move(existing);
            break;
        }
    }
    bool projectCreated = !matching.has_value();
    Project project;
    if (matching) {
        project = std::move(*matching);
        for (const auto &existing : project.releases) {
            if (existing.sourceSha256 == sourceHash) {
                if (existing.state != ReleaseState::Discovered) {
                    return ImportResult{std::move(project), existing.id, false, true};
                }
                break;
            }
        }
    } else {
        project.archPackageName =
            PkgbuildGenerator::sanitizePackageName(analysis->metadata.package) + QStringLiteral("-bin");
        project.id = PkgbuildGenerator::sanitizePackageName(analysis->metadata.package);
        if (project.id.isEmpty()) project.id = QStringLiteral("vendor-package");
        auto candidate = project.id;
        int suffix = 2;
        while (std::filesystem::exists(projectPath(candidate))) {
            candidate = project.id + QLatin1Char('-') + QString::number(suffix++);
        }
        project.id = candidate;
        project.displayName = sourceDisplayName(analysis->metadata);
        project.vendorName = analysis->metadata.maintainer;
        project.sourceIdentity = identity;
        project.createdAt = QDateTime::currentDateTimeUtc();
        project.modifiedAt = project.createdAt;
    }

    PackageRelease release;
    release.projectId = project.id;
    release.id = releaseId(analysis->metadata.version, sourceHash);
    release.displayName = project.displayName;
    release.archPackageName = project.archPackageName;
    release.sourceType = SourcePackageType::Debian;
    release.installMapping = analysis->installMapping;
    release.acquisition = options.acquisition;
    if (release.acquisition.canonicalIdentity.isEmpty()) {
        release.acquisition.kind = AcquisitionKind::LocalFile;
        release.acquisition.canonicalIdentity = sourceIdentity(analysis->metadata);
    }
    release.originalSourceFilename = QFileInfo(qStringFromPath(debPath)).fileName();
    release.sourceSha256 = sourceHash;
    release.vendorName = analysis->metadata.maintainer;
    release.debian = analysis->metadata;
    release.dependencies = analysis->dependencies;
    release.maintainerScripts = analysis->maintainerScripts;
    release.scriptFindings = analysis->scriptFindings;
    release.payload = analysis->payload;
    release.payloadRules = analysis->payloadRules;
    release.update.detectedCandidates = analysis->updateCandidates;
    release.update.aptCandidates = analysis->aptCandidates;
    release.update.rpmCandidates = analysis->rpmCandidates;
    release.update.aptPackageName = release.debian.package;
    release.update.aptArchitecture = release.debian.architecture;
    applyInitialUpdateConfiguration(release, options);
    release.createdAt = QDateTime::currentDateTimeUtc();
    release.modifiedAt = release.createdAt;
    for (const auto &existing : project.releases) {
        if (compareReleaseVersions(existing, release) == 0) {
            release.archPkgrel = std::max(release.archPkgrel, existing.archPkgrel + 1);
        }
    }

    const auto discovered = std::find_if(project.releases.begin(), project.releases.end(),
                                         [&](const auto &candidate) {
                                             return candidate.state == ReleaseState::Discovered &&
                                                    (candidate.sourceSha256 == sourceHash ||
                                                     (release.acquisition.githubAssetId > 0 &&
                                                      candidate.acquisition.githubAssetId ==
                                                          release.acquisition.githubAssetId));
                                         });
    if (discovered != project.releases.end()) release.id = discovered->id;
    const auto *previous = newestPreparedRelease(project);
    if (previous != nullptr) {
        carryForward(*previous, release,
                     previous->pkgbuildManuallyModified
                         ? readPkgbuild(*previous, nullptr).value_or(QString{})
                         : QString{});
    }

    if (options.initialUpdate) {
        // The repository was already selected and cryptographically verified
        // before this artifact was downloaded. Package-contained hints remain
        // visible as candidates but do not replace that explicit configuration.
    } else if (!release.update.aptCandidates.isEmpty()) {
        const auto &candidate = release.update.aptCandidates.first();
        release.update.url = candidate.uri;
        release.update.aptSuite = candidate.suite;
        if (!candidate.components.isEmpty()) release.update.aptComponent = candidate.components.first();
        if (!candidate.architectures.isEmpty()) {
            release.update.aptArchitecture = candidate.architectures.first();
        }
        release.update.strategy = UpdateStrategy::AptRepository;
    } else if (!release.update.rpmCandidates.isEmpty()) {
        static_cast<void>(populateRpmCandidates(release));
    }
    if (release.acquisition.kind == AcquisitionKind::GitHubRelease) {
        release.update.strategy = UpdateStrategy::GitHubRelease;
        release.update.url = release.acquisition.originalUrl;
        release.update.githubOwner = release.acquisition.githubOwner;
        release.update.githubRepository = release.acquisition.githubRepository;
        release.update.githubReleaseId = release.acquisition.githubReleaseId;
        release.update.githubAssetId = release.acquisition.githubAssetId;
        release.update.githubTag = release.acquisition.githubTag;
        release.update.githubPublisherDigest = release.acquisition.publisherDigest;
    }

    const auto directory = releasePath(release);
    std::error_code filesystemError;
    for (const auto *subdirectory : {"sources", "files", "patches", "build", "history"}) {
        std::filesystem::create_directories(directory / subdirectory, filesystemError);
        if (filesystemError) {
            if (error != nullptr) *error = QString::fromStdString(filesystemError.message());
            return std::nullopt;
        }
    }
    if (!writeImportedLifecycle(directory, release, error)) return std::nullopt;
    if (!release.previousManualPkgbuild.isEmpty() &&
        !writeBytes(directory / "files" / "previous-manual-PKGBUILD",
                    release.previousManualPkgbuild.toUtf8(), error)) {
        return std::nullopt;
    }
    if (!materializeIntegrationIcon(
            *this, project, release, debPath,
            analysis->icon ? analysis->icon->sourcePath : QString{},
            analysis->icon ? analysis->icon->contents : QByteArray{}, error)) {
        return std::nullopt;
    }
    if (!storeImportSigningKeys(directory, analysis->signingKeys, options,
                                release.update, error)) return std::nullopt;

    if (progress) progress({ImportStage::CopyingSource, 0});
    if (!copyFileAtomically(debPath, sourcePath(release), error)) return std::nullopt;
    const auto sourceLink = directory / pathFromQString(release.originalSourceFilename);
    std::filesystem::create_symlink(
        std::filesystem::path("sources") / pathFromQString(release.originalSourceFilename),
        sourceLink, filesystemError);
    if (filesystemError) {
        filesystemError.clear();
        if (!copyFileAtomically(sourcePath(release), sourceLink, error)) return std::nullopt;
    }
    if (progress) progress({ImportStage::GeneratingPkgbuild, 0});
    release.generatedPkgbuild = PkgbuildGenerator::generate(release);
    release.generatedPkgbuildSha256 = sha256Hex(release.generatedPkgbuild.toUtf8());
    if (!writeBytes(pkgbuildPath(release), release.generatedPkgbuild.toUtf8(), error)) {
        return std::nullopt;
    }
    release.history.append({release.createdAt, QStringLiteral("created"),
                            QStringLiteral("Imported %1 (%2)")
                                .arg(release.originalSourceFilename, release.debian.version)});
    project.history.append({release.createdAt, QStringLiteral("release-imported"),
                            QStringLiteral("Imported vendor release %1").arg(release.debian.version)});
    if (discovered != project.releases.end()) {
        const auto index = std::distance(project.releases.begin(), discovered);
        project.releases[static_cast<qsizetype>(index)] = release;
    } else {
        project.releases.append(release);
    }
    if (progress) progress({ImportStage::SavingProject, 0});
    if (!save(project, error)) return std::nullopt;
    return ImportResult{std::move(project), release.id, projectCreated, false};
}

std::optional<ImportResult> ProjectStore::importSource(
    const std::filesystem::path &inputPath, const ImportOptions &options, QString *error,
    const ImportProgressCallback &progress) const {
    if (progress) progress({ImportStage::ValidatingSource, 0});
    const auto detectedType = SourceAnalyzer::detect(inputPath, error);
    if (!detectedType) return std::nullopt;
    if (*detectedType == SourcePackageType::Debian) {
        auto imported = importDeb(inputPath, error, progress, options);
        if (!imported) return std::nullopt;
        auto *release = imported->project.release(imported->releaseId);
        if (release != nullptr) {
            release->sourceType = SourcePackageType::Debian;
            release->acquisition = options.acquisition;
            if (release->acquisition.canonicalIdentity.isEmpty()) {
                release->acquisition.kind = AcquisitionKind::LocalFile;
                release->acquisition.canonicalIdentity =
                    QStringLiteral("deb:%1:%2").arg(release->debian.package,
                                                     release->debian.architecture);
            }
            release->sourceUrl = release->acquisition.originalUrl;
            if (release->acquisition.kind == AcquisitionKind::GitHubRelease) {
                release->update.strategy = UpdateStrategy::GitHubRelease;
                release->update.githubOwner = release->acquisition.githubOwner;
                release->update.githubRepository = release->acquisition.githubRepository;
                release->update.githubReleaseId = release->acquisition.githubReleaseId;
                release->update.githubAssetId = release->acquisition.githubAssetId;
                release->update.githubTag = release->acquisition.githubTag;
                release->update.githubPublisherDigest = release->acquisition.publisherDigest;
                release->update.githubAssetRegex = options.githubAssetRegex;
                release->update.githubIncludePrereleases = options.githubIncludePrereleases;
            }
            imported->project.formatVersion = 5;
            if (!save(imported->project, error)) return std::nullopt;
        }
        return imported;
    }

    auto analysis = SourceAnalyzer::analyze(inputPath, error, progress);
    if (!analysis) return std::nullopt;
    if (!options.packageName.isEmpty()) analysis->metadata.package = options.packageName;
    if (!options.version.isEmpty()) analysis->metadata.version = options.version;
    if (!options.architecture.isEmpty()) analysis->metadata.architecture = options.architecture;
    if (!options.description.isEmpty()) analysis->metadata.description = options.description;
    if (analysis->metadata.package.isEmpty() || analysis->metadata.version.isEmpty()) {
        if (error != nullptr) *error = QStringLiteral("Package name and version are required");
        return std::nullopt;
    }
    const auto sourceHash = sha256File(inputPath, error);
    if (sourceHash.isEmpty()) return std::nullopt;
    auto acquisition = options.acquisition;
    if (acquisition.canonicalIdentity.isEmpty()) {
        acquisition.kind = AcquisitionKind::LocalFile;
        acquisition.canonicalIdentity = QStringLiteral("%1:%2:%3")
            .arg(sourcePackageTypeName(analysis->type), analysis->metadata.package.toLower(),
                 analysis->metadata.architecture.toLower());
    }
    const auto identity = acquisition.kind == AcquisitionKind::GitHubRelease
        ? QStringLiteral("github:%1/%2")
              .arg(acquisition.githubOwner.toLower(), acquisition.githubRepository.toLower())
        : acquisition.canonicalIdentity;
    std::optional<Project> matching;
    for (auto existing : list()) {
        if (existing.sourceIdentity == identity) {
            matching = std::move(existing);
            break;
        }
    }
    Project project;
    const bool projectCreated = !matching.has_value();
    if (matching) {
        project = std::move(*matching);
        for (auto &existing : project.releases) {
            if (existing.sourceSha256 == sourceHash) {
                // Re-importing an already analyzed artifact is also a safe way to
                // backfill metadata learned by newer PacSmith analyzers. Keep the
                // immutable release and source bytes, but persist a newly detected
                // icon when older imports did not have one.
                if (analysis->icon && existing.iconPath.isEmpty()) {
                    const auto suffix = QFileInfo(analysis->icon->sourcePath).suffix().toLower();
                    const auto directory = releasePath(existing);
                    std::error_code filesystemError;
                    std::filesystem::create_directories(directory / "files", filesystemError);
                    if (filesystemError) {
                        if (error != nullptr) {
                            *error = QString::fromStdString(filesystemError.message());
                        }
                        return std::nullopt;
                    }
                    existing.iconPath = QStringLiteral("files/icon.%1").arg(suffix);
                    existing.iconSourcePath = analysis->icon->sourcePath;
                    existing.iconSha256 = sha256Hex(analysis->icon->contents);
                    existing.installMapping.icon.sourceKind = IconSourceKind::Payload;
                    existing.installMapping.icon.sourcePath = existing.iconSourcePath;
                    existing.installMapping.icon.projectPath = existing.iconPath;
                    existing.installMapping.icon.sha256 = existing.iconSha256;
                    existing.installMapping.icon.format = suffix;
                    existing.installMapping.icon.iconName = existing.debian.package.isEmpty()
                        ? existing.archPackageName : existing.debian.package;
                    if (!writeBytes(directory / pathFromQString(existing.iconPath),
                                    analysis->icon->contents, error)) {
                        return std::nullopt;
                    }
                    project.iconPath = QStringLiteral("releases/%1/%2")
                                           .arg(existing.id, existing.iconPath);
                    project.iconSourcePath = existing.iconSourcePath;
                    project.iconSha256 = existing.iconSha256;
                    if (!existing.pkgbuildManuallyModified) {
                        existing.generatedPkgbuild = PkgbuildGenerator::generate(existing);
                        existing.generatedPkgbuildSha256 =
                            sha256Hex(existing.generatedPkgbuild.toUtf8());
                        if (!writeBytes(pkgbuildPath(existing),
                                        existing.generatedPkgbuild.toUtf8(), error)) {
                            return std::nullopt;
                        }
                    }
                    if (!save(project, error)) return std::nullopt;
                }
                return ImportResult{std::move(project), existing.id, false, true};
            }
        }
    } else {
        project.formatVersion = 5;
        const auto baseName = PkgbuildGenerator::sanitizePackageName(analysis->metadata.package);
        project.archPackageName = analysis->type == SourcePackageType::ArchPackage
            ? baseName : baseName + QStringLiteral("-bin");
        project.id = baseName.isEmpty() ? QStringLiteral("vendor-package") : baseName;
        auto candidate = project.id;
        int suffix = 2;
        while (std::filesystem::exists(projectPath(candidate))) {
            candidate = project.id + QLatin1Char('-') + QString::number(suffix++);
        }
        project.id = candidate;
        project.displayName = !options.displayName.isEmpty()
            ? options.displayName
            : analysis->metadata.description.section(QLatin1Char('\n'), 0, 0).trimmed();
        if (project.displayName.isEmpty()) project.displayName = analysis->metadata.package;
        project.vendorName = analysis->metadata.maintainer;
        project.sourceIdentity = identity;
        project.createdAt = QDateTime::currentDateTimeUtc();
        project.modifiedAt = project.createdAt;
    }

    PackageRelease release;
    release.formatVersion = 2;
    release.projectId = project.id;
    release.id = releaseId(analysis->metadata.version, sourceHash);
    release.displayName = project.displayName;
    release.archPackageName = project.archPackageName;
    release.sourceType = analysis->type;
    release.acquisition = acquisition;
    release.installMapping = analysis->installMapping;
    release.originalSourceFilename = QFileInfo(qStringFromPath(inputPath)).fileName();
    release.sourceUrl = acquisition.originalUrl;
    release.sourceSha256 = sourceHash;
    release.vendorName = project.vendorName;
    release.debian = analysis->metadata;
    release.dependencies = analysis->dependencies;
    release.maintainerScripts = analysis->maintainerScripts;
    release.scriptFindings = analysis->scriptFindings;
    release.payload = analysis->payload;
    release.payloadRules = analysis->payloadRules;
    release.update.detectedCandidates = analysis->updateCandidates;
    release.update.aptCandidates = analysis->aptCandidates;
    release.update.rpmCandidates = analysis->rpmCandidates;
    release.update.aptPackageName = release.debian.package;
    release.update.aptArchitecture = release.debian.architecture;
    release.update.rpmPackageName = release.debian.package;
    release.update.rpmArchitecture = release.debian.architecture;
    applyInitialUpdateConfiguration(release, options);
    if (analysis->type == SourcePackageType::ArchPackage) {
        release.archPkgrelOverride = repackagedPkgrel(analysis->upstreamArchPkgrel);
    }
    release.createdAt = QDateTime::currentDateTimeUtc();
    release.modifiedAt = release.createdAt;
    release.state = releaseNeedsReview(release) ? ReleaseState::NeedsReview : ReleaseState::Ready;
    if (options.initialUpdate) {
        // Keep the explicitly verified repository acquisition configuration.
    } else if (acquisition.kind == AcquisitionKind::GitHubRelease) {
        release.update.strategy = UpdateStrategy::GitHubRelease;
        release.update.url = acquisition.originalUrl;
        release.update.githubOwner = acquisition.githubOwner;
        release.update.githubRepository = acquisition.githubRepository;
        release.update.githubReleaseId = acquisition.githubReleaseId;
        release.update.githubAssetId = acquisition.githubAssetId;
        release.update.githubTag = acquisition.githubTag;
        release.update.githubPublisherDigest = acquisition.publisherDigest;
        release.update.githubAssetRegex = options.githubAssetRegex;
        release.update.githubIncludePrereleases = options.githubIncludePrereleases;
    } else if (!release.update.aptCandidates.isEmpty()) {
        static_cast<void>(populateAptCandidates(release));
    } else if (!release.update.rpmCandidates.isEmpty()) {
        static_cast<void>(populateRpmCandidates(release));
    }
    if (const auto *previous = newestPreparedRelease(project); previous != nullptr) {
        carryForward(*previous, release,
                     previous->pkgbuildManuallyModified
                         ? readPkgbuild(*previous, nullptr).value_or(QString{})
                         : QString{});
        if (release.update.strategy == UpdateStrategy::Manual) {
            release.update = previous->update;
        } else if (release.update.strategy == UpdateStrategy::GitHubRelease) {
            const auto releaseIdValue = release.update.githubReleaseId;
            const auto assetIdValue = release.update.githubAssetId;
            const auto tagValue = release.update.githubTag;
            const auto digestValue = release.update.githubPublisherDigest;
            release.update = previous->update;
            release.update.strategy = UpdateStrategy::GitHubRelease;
            release.update.githubReleaseId = releaseIdValue;
            release.update.githubAssetId = assetIdValue;
            release.update.githubTag = tagValue;
            release.update.githubPublisherDigest = digestValue;
        }
    }
    const auto discovered = std::find_if(project.releases.begin(), project.releases.end(),
                                         [&](const auto &candidate) {
                                             return candidate.state == ReleaseState::Discovered &&
                                                    ((acquisition.githubAssetId > 0 &&
                                                      candidate.acquisition.githubAssetId ==
                                                          acquisition.githubAssetId) ||
                                                     (!candidate.sourceSha256.isEmpty() &&
                                                      candidate.sourceSha256 == sourceHash));
                                         });
    if (discovered != project.releases.end()) release.id = discovered->id;

    const auto directory = releasePath(release);
    std::error_code filesystemError;
    for (const auto *subdirectory : {"sources", "files", "patches", "build", "history"}) {
        std::filesystem::create_directories(directory / subdirectory, filesystemError);
        if (filesystemError) {
            if (error != nullptr) *error = QString::fromStdString(filesystemError.message());
            return std::nullopt;
        }
    }
    if (!writeImportedLifecycle(directory, release, error)) return std::nullopt;
    if (!materializeIntegrationIcon(
            *this, project, release, inputPath,
            analysis->icon ? analysis->icon->sourcePath : QString{},
            analysis->icon ? analysis->icon->contents : QByteArray{}, error)) {
        return std::nullopt;
    }
    if (!storeImportSigningKeys(directory, analysis->signingKeys, options,
                                release.update, error)) return std::nullopt;
    if (progress) progress({ImportStage::CopyingSource, 0});
    if (!copyFileAtomically(inputPath, sourcePath(release), error)) return std::nullopt;
    const auto sourceLink = directory / pathFromQString(release.originalSourceFilename);
    std::filesystem::create_symlink(
        std::filesystem::path("sources") / pathFromQString(release.originalSourceFilename),
        sourceLink, filesystemError);
    if (filesystemError) {
        filesystemError.clear();
        if (!copyFileAtomically(sourcePath(release), sourceLink, error)) return std::nullopt;
    }
    if (progress) progress({ImportStage::GeneratingPkgbuild, 0});
    release.generatedPkgbuild = PkgbuildGenerator::generate(release);
    release.generatedPkgbuildSha256 = sha256Hex(release.generatedPkgbuild.toUtf8());
    if (!writeBytes(pkgbuildPath(release), release.generatedPkgbuild.toUtf8(), error)) {
        return std::nullopt;
    }
    release.history.append({release.createdAt, QStringLiteral("import"),
                            QStringLiteral("Imported %1 source %2")
                                .arg(sourcePackageTypeName(release.sourceType),
                                     release.originalSourceFilename)});
    project.history.append({release.createdAt, QStringLiteral("release-imported"),
                            QStringLiteral("Imported release %1").arg(release.debian.version)});
    if (discovered != project.releases.end()) {
        const auto index = std::distance(project.releases.begin(), discovered);
        project.releases[static_cast<qsizetype>(index)] = release;
    } else {
        project.releases.append(release);
    }
    project.formatVersion = 5;
    if (progress) progress({ImportStage::SavingProject, 0});
    if (!save(project, error)) return std::nullopt;
    return ImportResult{std::move(project), release.id, projectCreated, false};
}

std::optional<ImportResult> ProjectStore::reanalyzeRelease(
    const QString &projectId, const QString &releaseIdValue, QString *error,
    const ImportProgressCallback &progress) const {
    auto loaded = load(projectId, error);
    if (!loaded) return std::nullopt;
    auto &project = *loaded;
    const auto iterator = std::find_if(
        project.releases.begin(), project.releases.end(),
        [&](const auto &candidate) { return candidate.id == releaseIdValue; });
    if (iterator == project.releases.end()) {
        if (error != nullptr) *error = QStringLiteral("Release not found: %1").arg(releaseIdValue);
        return std::nullopt;
    }
    if (iterator->state == ReleaseState::Discovered ||
        iterator->state == ReleaseState::Preparing) {
        if (error != nullptr) *error = QStringLiteral("The release artifact has not been prepared yet");
        return std::nullopt;
    }

    const auto artifact = sourcePath(*iterator);
    if (!std::filesystem::is_regular_file(artifact)) {
        if (error != nullptr) {
            *error = QStringLiteral("The immutable source artifact is missing: %1")
                         .arg(qStringFromPath(artifact));
        }
        return std::nullopt;
    }
    QString hashError;
    const auto currentHash = sha256File(artifact, &hashError);
    if (currentHash.isEmpty() || currentHash != iterator->sourceSha256) {
        if (error != nullptr) {
            *error = hashError.isEmpty()
                ? QStringLiteral("The stored source artifact no longer matches its recorded SHA256")
                : hashError;
        }
        return std::nullopt;
    }

    auto analysis = analyzeArtifact(artifact, error, progress);
    if (!analysis) return std::nullopt;
    if (analysis->metadata.package.isEmpty()) analysis->metadata.package = iterator->debian.package;
    if (analysis->metadata.version.isEmpty()) analysis->metadata.version = iterator->debian.version;
    if (analysis->metadata.architecture.isEmpty()) {
        analysis->metadata.architecture = iterator->debian.architecture;
    }

    if (progress) progress({ImportStage::PreparingProject, 0});
    const auto previous = *iterator;
    PackageRelease reset;
    reset.formatVersion = std::max(2, previous.formatVersion);
    reset.id = previous.id;
    reset.projectId = project.id;
    reset.displayName = previous.displayName;
    reset.archPackageName = previous.archPackageName;
    reset.sourceType = analysis->type;
    reset.acquisition = previous.acquisition;
    reset.originalSourceFilename = previous.originalSourceFilename;
    reset.sourceUrl = previous.sourceUrl;
    reset.sourceSha256 = previous.sourceSha256;
    reset.vendorName = analysis->metadata.maintainer.isEmpty()
        ? previous.vendorName : analysis->metadata.maintainer;
    // pkgrel belongs to the release identity rather than an editor decision. In
    // particular, retaining it avoids turning a reset of an installed release
    // into an accidental package downgrade.
    reset.archPkgrel = previous.archPkgrel;
    reset.debian = analysis->metadata;
    reset.dependencies = analysis->dependencies;
    reset.maintainerScripts = analysis->maintainerScripts;
    reset.scriptFindings = analysis->scriptFindings;
    reset.payload = analysis->payload;
    reset.payloadRules = analysis->payloadRules;
    reset.installMapping = analysis->installMapping;
    // Update discovery is configured independently from the package recipe and
    // may be the only way to acquire the next artifact, so a package reset must
    // not destroy it.
    reset.update = previous.update;
    reset.buildStatus = BuildStatus::NeverBuilt;
    reset.builds = previous.builds;
    reset.history = previous.history;
    reset.createdAt = previous.createdAt;
    reset.modifiedAt = QDateTime::currentDateTimeUtc();
    reset.history.append(
        {reset.modifiedAt, QStringLiteral("reanalyzed"),
         QStringLiteral("Reset package setup and reran static artifact analysis")});
    project.history.append(
        {reset.modifiedAt, QStringLiteral("release-reanalyzed"),
         QStringLiteral("Reset and reanalyzed release %1").arg(reset.debian.version)});

    const auto directory = releasePath(reset);
    std::error_code filesystemError;
    for (const auto *subdirectory : {"files", "build", "history", "patches", "sources"}) {
        std::filesystem::create_directories(directory / subdirectory, filesystemError);
        if (filesystemError) {
            if (error != nullptr) *error = QString::fromStdString(filesystemError.message());
            return std::nullopt;
        }
    }
    if (project.iconPath.startsWith(
            QStringLiteral("releases/%1/").arg(previous.id))) {
        project.iconPath.clear();
        project.iconSourcePath.clear();
        project.iconSha256.clear();
    }
    if (!materializeIntegrationIcon(
            *this, project, reset, artifact,
            analysis->icon ? analysis->icon->sourcePath : QString{},
            analysis->icon ? analysis->icon->contents : QByteArray{}, error)) {
        return std::nullopt;
    }
    if (!storeImportSigningKeys(directory, analysis->signingKeys, {}, reset.update, error)) {
        return std::nullopt;
    }
    if (progress) progress({ImportStage::GeneratingPkgbuild, 0});
    reset.generatedPkgbuild = PkgbuildGenerator::generate(reset);
    reset.generatedPkgbuildSha256 = sha256Hex(reset.generatedPkgbuild.toUtf8());
    if (!writeBytes(pkgbuildPath(reset), reset.generatedPkgbuild.toUtf8(), error)) {
        return std::nullopt;
    }

    *iterator = reset;
    if (progress) progress({ImportStage::SavingProject, 0});
    if (!save(project, error)) return std::nullopt;

    // Remove obsolete generated/editor files after the replacement state is
    // safely persisted. Source bytes, signing keys, prior package artifacts,
    // and history are deliberately untouched.
    const auto removeObsolete = [](const std::filesystem::path &path) {
        std::error_code cleanupError;
        if (std::filesystem::is_regular_file(path, cleanupError) ||
            std::filesystem::is_symlink(path, cleanupError)) {
            std::filesystem::remove(path, cleanupError);
        }
    };
    if (!previous.lifecycleScript.fileName.isEmpty()) {
        removeObsolete(directory / pathFromQString(previous.lifecycleScript.fileName));
    }
    removeObsolete(directory / "files" / "previous-manual-PKGBUILD");
    const auto previousIcon = PathSafety::normalizedArchivePath(
        previous.installMapping.icon.projectPath);
    if (previousIcon && previousIcon->startsWith(QStringLiteral("files/")) &&
        *previousIcon != reset.installMapping.icon.projectPath) {
        removeObsolete(directory / pathFromQString(*previousIcon));
    }
    for (const auto *suffix : {"png", "svg", "xpm"}) {
        if (reset.installMapping.icon.format != QString::fromLatin1(suffix)) {
            removeObsolete(directory / QStringLiteral("pacsmith-icon.%1").arg(
                                        QString::fromLatin1(suffix)).toStdString());
        }
    }
    return ImportResult{std::move(project), releaseIdValue, false, false};
}

PackageRelease *ProjectStore::recordDiscoveredRelease(
    Project &project, const PackageRelease &tracker, const QString &version,
    const QString &filename, const QString &sha256, const QString &downloadUrl,
    QString *error, const qint64 providerReleaseId, const qint64 providerAssetId,
    const QString &providerTag, const QString &publisherDigest,
    const bool providerPrerelease) const {
    if (version.isEmpty() || filename.isEmpty() || downloadUrl.isEmpty()) {
        if (error != nullptr) *error = QStringLiteral("The update result is missing version, filename, or download URL");
        return nullptr;
    }
    const auto existing = std::find_if(project.releases.begin(), project.releases.end(),
                                       [&](const auto &candidate) {
                                           return (!sha256.isEmpty() && candidate.sourceSha256 == sha256) ||
                                                  (providerAssetId > 0 &&
                                                   candidate.acquisition.githubAssetId == providerAssetId) ||
                                                  (candidate.debian.version == version &&
                                                   candidate.sourceUrl == downloadUrl);
                                       });
    if (existing != project.releases.end()) {
        // The caller may have just updated the tracking release's last-check result.
        // Persist that state even when this vendor release was already discovered.
        if (!save(project, error)) return nullptr;
        return &*existing;
    }
    PackageRelease release;
    release.projectId = project.id;
    const auto identityHash = !sha256.isEmpty()
        ? sha256 : sha256Hex(QStringLiteral("%1:%2:%3").arg(providerReleaseId).arg(providerAssetId).arg(downloadUrl).toUtf8());
    release.id = releaseId(version, identityHash);
    release.displayName = project.displayName;
    release.archPackageName = project.archPackageName;
    release.vendorName = project.vendorName;
    release.originalSourceFilename = filename;
    release.sourceUrl = downloadUrl;
    release.sourceSha256 = sha256;
    // GitHub can change artifact formats between releases. Do not claim a type until the
    // downloaded bytes have passed content-based detection and static analysis.
    release.sourceType = SourcePackageType::Unknown;
    release.acquisition = tracker.acquisition;
    release.acquisition.originalUrl = downloadUrl;
    if (tracker.update.strategy == UpdateStrategy::AptRepository) {
        release.acquisition.kind = AcquisitionKind::AptRepository;
    } else if (tracker.update.strategy == UpdateStrategy::RpmRepository) {
        release.acquisition.kind = AcquisitionKind::RpmRepository;
    }
    if (tracker.update.strategy == UpdateStrategy::GitHubRelease) {
        release.acquisition.kind = AcquisitionKind::GitHubRelease;
        release.acquisition.githubOwner = tracker.update.githubOwner;
        release.acquisition.githubRepository = tracker.update.githubRepository;
        release.acquisition.githubReleaseId = providerReleaseId;
        release.acquisition.githubAssetId = providerAssetId;
        release.acquisition.githubTag = providerTag;
        release.acquisition.githubPrerelease = providerPrerelease;
        release.acquisition.githubAssetName = filename;
        release.acquisition.publisherDigest = publisherDigest;
        release.acquisition.canonicalIdentity = QStringLiteral("github:%1/%2")
            .arg(tracker.update.githubOwner, tracker.update.githubRepository);
    }
    release.debian.package = tracker.debian.package;
    release.debian.version = version;
    release.debian.architecture = tracker.debian.architecture;
    release.update = tracker.update;
    release.update.githubReleaseId = providerReleaseId;
    release.update.githubAssetId = providerAssetId;
    release.update.githubTag = providerTag;
    release.update.githubPublisherDigest = publisherDigest;
    release.state = ReleaseState::Discovered;
    release.createdAt = QDateTime::currentDateTimeUtc();
    release.modifiedAt = release.createdAt;
    const auto discoveryMessage = (tracker.update.strategy == UpdateStrategy::AptRepository ||
                                   tracker.update.strategy == UpdateStrategy::RpmRepository)
        ? QStringLiteral("Discovered signature-verified vendor release %1").arg(version)
        : !publisherDigest.isEmpty()
            ? QStringLiteral("Discovered GitHub release %1 with publisher digest").arg(version)
            : QStringLiteral("Discovered GitHub release %1 without a publisher digest; downloaded bytes still require a local SHA256")
                  .arg(version);
    release.history.append({release.createdAt, QStringLiteral("discovered"), discoveryMessage});
    project.history.append({release.createdAt, QStringLiteral("release-discovered"),
                            discoveryMessage});
    project.releases.append(release);
    if (!save(project, error)) {
        project.releases.removeLast();
        return nullptr;
    }
    return project.release(release.id);
}

bool ProjectStore::savePkgbuild(Project &project, PackageRelease &release,
                                const QString &contents, QString *error) const {
    if (!writeBytes(pkgbuildPath(release), contents.toUtf8(), error)) return false;
    release.pkgbuildManuallyModified =
        sha256Hex(contents.toUtf8()) != release.generatedPkgbuildSha256;
    return save(project, error);
}

bool ProjectStore::saveLifecycle(Project &project, PackageRelease &release,
                                 QString *error) const {
    static const QRegularExpression safeName(QStringLiteral("^[a-z0-9@._+\\-]+\\.install$"));
    if (release.lifecycleScript.contents.isEmpty()) return true;
    if (!safeName.match(release.lifecycleScript.fileName).hasMatch()) {
        if (error != nullptr) *error = QStringLiteral("Unsafe Arch lifecycle filename");
        return false;
    }
    const auto validation = LifecycleValidator::validate(release.lifecycleScript.contents);
    release.lifecycleScript.validationPassed = validation.passed;
    release.lifecycleScript.validationMessage = validation.message();
    release.lifecycleScript.manuallyModified = false;
    release.buildStatus = BuildStatus::NeverBuilt;
    release.producedPackages.clear();
    if (!writeBytes(lifecyclePath(release), release.lifecycleScript.contents.toUtf8(), error)) {
        return false;
    }
    return save(project, error);
}

bool ProjectStore::removeLifecycle(Project &project, PackageRelease &release,
                                   QString *error) const {
    if (!release.lifecycleScript.fileName.isEmpty()) {
        const auto path = lifecyclePath(release);
        if (QFileInfo::exists(qStringFromPath(path)) && !QFile::remove(qStringFromPath(path))) {
            if (error != nullptr) *error = QStringLiteral("Could not remove lifecycle file");
            return false;
        }
    }
    release.lifecycleScript = {};
    release.buildStatus = BuildStatus::NeverBuilt;
    release.producedPackages.clear();
    return save(project, error);
}

bool ProjectStore::synchronizeLifecycle(Project &project, PackageRelease &release,
                                        bool *changed, QString *error) const {
    if (changed != nullptr) *changed = false;
    if (release.lifecycleScript.contents.isEmpty()) return true;
    QFile file(qStringFromPath(lifecyclePath(release)));
    bool missing = !file.open(QIODevice::ReadOnly);
    auto contents = missing ? QString{} : QString::fromUtf8(file.read(128 * 1024 + 1));
    bool wasChanged = false;
    if (missing) {
        // Release imports before format v5 could carry the lifecycle metadata and
        // generate install=... without copying the exact file. Restore only those
        // already-recorded bytes; package content is never executed here.
        if (!writeImportedLifecycle(releasePath(release), release, error)) return false;
        missing = false;
        contents = release.lifecycleScript.contents;
        release.history.append(
            {QDateTime::currentDateTimeUtc(), QStringLiteral("lifecycle-restored"),
             QStringLiteral("Restored the project-local lifecycle file from its exact recorded content")});
        wasChanged = true;
    }
    if (!missing && contents != release.lifecycleScript.contents) {
        release.lifecycleScript.contents = contents;
        release.lifecycleScript.manuallyModified = true;
        release.lifecycleScript.acknowledgedFingerprint.clear();
        release.lifecycleScript.provenance = {
            ValueOrigin::User, {}, {}, release.sourceSha256,
            QStringLiteral("Edited directly in the project release directory"),
            QDateTime::currentDateTimeUtc(), true};
        wasChanged = true;
    }
    const auto validation = missing
        ? LifecycleValidation{false, {QStringLiteral("The project-local lifecycle file is missing")}}
        : LifecycleValidator::validate(release.lifecycleScript.contents);
    if (release.lifecycleScript.validationPassed != validation.passed ||
        release.lifecycleScript.validationMessage != validation.message()) {
        release.lifecycleScript.validationPassed = validation.passed;
        release.lifecycleScript.validationMessage = validation.message();
        wasChanged = true;
    }
    if (wasChanged) {
        release.buildStatus = BuildStatus::NeverBuilt;
        release.producedPackages.clear();
        if (!save(project, error)) return false;
    }
    if (changed != nullptr) *changed = wasChanged;
    return true;
}

std::optional<QString> ProjectStore::readPkgbuild(const PackageRelease &release,
                                                  QString *error) const {
    QFile file(qStringFromPath(pkgbuildPath(release)));
    if (!file.open(QIODevice::ReadOnly)) {
        if (error != nullptr) *error = file.errorString();
        return std::nullopt;
    }
    return QString::fromUtf8(file.readAll());
}

CleanupResult ProjectStore::cleanup(Project &project, const RetentionPolicy &policy,
                                    QString *error) const {
    CleanupResult result;
    static_cast<void>(reconcileInstalled(project, nullptr));
    const auto *installed = project.installedRelease();
    if (installed == nullptr || project.externallyInstalled || project.installedVersion.isEmpty()) {
        result.skipped = true;
        result.message = QStringLiteral("Cleanup skipped because no known PacSmith release is installed");
        return result;
    }
    QList<QString> olderIds;
    for (const auto &release : project.releases) {
        if (release.id != installed->id &&
            compareReleaseVersions(release, *installed) < 0) {
            olderIds.append(release.id);
        }
    }
    std::sort(olderIds.begin(), olderIds.end(), [&](const auto &leftId, const auto &rightId) {
        const auto *left = project.release(leftId);
        const auto *right = project.release(rightId);
        return compareReleaseVersions(*left, *right) > 0;
    });

    if (policy.packageVersions >= 0) {
        int artifactReleaseIndex = 0;
        for (const auto &id : olderIds) {
            auto *release = project.release(id);
            if (release == nullptr || release->producedPackages.isEmpty()) continue;
            if (artifactReleaseIndex++ < policy.packageVersions) continue;
            const auto base = std::filesystem::weakly_canonical(releasePath(*release));
            for (const auto &package : std::as_const(release->producedPackages)) {
                std::error_code pathError;
                const auto path = std::filesystem::weakly_canonical(pathFromQString(package), pathError);
                if (!pathError && path.parent_path() == base && QFile::remove(package)) {
                    result.removedArtifacts.append(package);
                }
            }
            release->producedPackages.clear();
            for (auto &build : release->builds) build.artifacts.clear();
        }
    }

    const auto completeLimit = policy.packageVersions < 0 || policy.completeReleases < 0
                                   ? -1
                                   : std::max(policy.completeReleases, policy.packageVersions);
    if (completeLimit >= 0 && olderIds.size() > completeLimit) {
        const auto removeIds = olderIds.sliced(completeLimit);
        for (const auto &id : removeIds) {
            if (!deleteRelease(project, id, error)) return result;
            result.removedReleases.append(id);
        }
    } else if (!save(project, error)) {
        return result;
    }
    result.message = QStringLiteral("Removed %1 package artifact(s) and %2 complete release(s)")
                         .arg(result.removedArtifacts.size())
                         .arg(result.removedReleases.size());
    return result;
}

std::optional<QString> ProjectStore::queryInstalledVersion(const QString &archPackageName,
                                                           QString *error) {
    if (archPackageName.isEmpty()) {
        if (error != nullptr) *error = QStringLiteral("The project has no Arch package name");
        return std::nullopt;
    }
    QProcess process;
    process.setProgram(QStringLiteral("/usr/bin/pacman"));
    process.setArguments({QStringLiteral("-Q"), QStringLiteral("--"), archPackageName});
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("LC_ALL"), QStringLiteral("C"));
    process.setProcessEnvironment(environment);
    process.start();
    if (!process.waitForStarted(3000)) {
        if (error != nullptr) *error = QStringLiteral("Could not start pacman: %1").arg(process.errorString());
        return std::nullopt;
    }
    if (!process.waitForFinished(10000)) {
        process.kill();
        process.waitForFinished();
        if (error != nullptr) *error = QStringLiteral("Timed out while asking pacman for installation status");
        return std::nullopt;
    }
    const auto output = QString::fromLocal8Bit(process.readAllStandardOutput()).trimmed();
    const auto errorOutput = QString::fromLocal8Bit(process.readAllStandardError()).trimmed();
    if (process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0) {
        const auto prefix = archPackageName + QLatin1Char(' ');
        if (!output.startsWith(prefix)) {
            if (error != nullptr) *error = QStringLiteral("pacman returned an unexpected package status");
            return std::nullopt;
        }
        return output.mid(prefix.size()).trimmed();
    }
    if (process.exitStatus() == QProcess::NormalExit && process.exitCode() == 1 &&
        errorOutput.contains(QStringLiteral("was not found"))) {
        return QString{};
    }
    if (error != nullptr) {
        *error = errorOutput.isEmpty() ? QStringLiteral("pacman could not determine installation status")
                                      : errorOutput;
    }
    return std::nullopt;
}

QString sha256Hex(const QByteArray &contents) {
    return QString::fromLatin1(QCryptographicHash::hash(contents, QCryptographicHash::Sha256).toHex());
}

QString sha256File(const std::filesystem::path &path, QString *error) {
    QFile file(qStringFromPath(path));
    if (!file.open(QIODevice::ReadOnly)) {
        if (error != nullptr) *error = file.errorString();
        return {};
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file)) {
        if (error != nullptr) *error = file.errorString();
        return {};
    }
    return QString::fromLatin1(hash.result().toHex());
}

} // namespace pacsmith
