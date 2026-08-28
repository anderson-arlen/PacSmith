#pragma once

#include <QDateTime>
#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QSize>
#include <QString>
#include <QStringList>

namespace pacsmith {

enum class SourcePackageType { Unknown, Debian, Rpm, ArchPackage, Archive, AppImage, ElfBinary };
enum class AcquisitionKind { LocalFile, DirectUrl, AptRepository, RpmRepository, GitHubRelease };
enum class MappingStatus { Unresolved, Resolved, Ignored, Bundled, Provided };
enum class BuildStatus { NeverBuilt, Building, Succeeded, Failed, Canceled };
enum class ReleaseState { Discovered, Preparing, NeedsReview, Ready, Built };
enum class UpdateStrategy { Manual, DirectUrl, AptRepository, RpmRepository, GitHubRelease };
enum class ValueOrigin { Unknown, Deterministic, Ai, User };
enum class ScriptDisposition { HandledByPacSmith, HandledByArch, LifecycleRequired, NotApplicable, Unresolved };

struct FieldProvenance {
    ValueOrigin origin{ValueOrigin::Unknown};
    QString provider;
    QString model;
    QString sourceFingerprint;
    QString rationale;
    QDateTime timestamp;
    bool userApproved{false};

    [[nodiscard]] QJsonObject toJson() const;
    [[nodiscard]] static FieldProvenance fromJson(const QJsonObject &object);
};

struct DebianMetadata {
    QString package;
    QString version;
    QString architecture;
    QString maintainer;
    QString description;
    QString homepage;
    QString depends;
    QString preDepends;
    QString recommends;
    QString suggests;
    QString conflicts;
    QString provides;
    QMap<QString, QString> rawFields;

    [[nodiscard]] QJsonObject toJson() const;
    [[nodiscard]] static DebianMetadata fromJson(const QJsonObject &object);
};

// User-maintained Arch package metadata is separate from immutable metadata
// extracted from the vendor artifact.
struct PackageMetadata {
    QString description;
    QString homepage;
    QStringList licenses;
    QStringList provides;
    QStringList conflicts;
    QStringList additionalDependencies;

    [[nodiscard]] QJsonObject toJson() const;
    [[nodiscard]] static PackageMetadata fromJson(const QJsonObject &object);
};

// Describes where a particular immutable source artifact came from.  This is
// deliberately separate from SourcePackageType: a GitHub release may contain
// a DEB, an Arch package, an archive, or a standalone ELF binary.
struct SourceAcquisition {
    AcquisitionKind kind{AcquisitionKind::LocalFile};
    QString canonicalIdentity;
    QString originalUrl;
    QString githubOwner;
    QString githubRepository;
    qint64 githubReleaseId{0};
    QString githubTag;
    bool githubPrerelease{false};
    qint64 githubAssetId{0};
    QString githubAssetName;
    QString publisherDigest;
    bool publisherVerified{false};

    [[nodiscard]] QJsonObject toJson() const;
    [[nodiscard]] static SourceAcquisition fromJson(const QJsonObject &object);
};

enum class ArchiveLayout { PreserveRoot, OptBundle };

enum class LauncherKind { Symlink, Wrapper };
enum class IconSourceKind { None, Payload, LocalFile, RemoteUrl };

// A command exposed by the generated Arch package.  The source path is always
// an exact, inspected payload path; it is never interpreted as shell text.
struct LauncherMapping {
    bool enabled{true};
    QString sourcePath;
    QString commandName;
    QString destination;
    LauncherKind kind{LauncherKind::Symlink};
    QString sourceFingerprint;
    bool missing{false};
    FieldProvenance provenance;

    [[nodiscard]] QJsonObject toJson() const;
    [[nodiscard]] static LauncherMapping fromJson(const QJsonObject &object);
};

// Desktop entries are first-class, versioned recipe inputs. Detected entries
// retain their original fingerprint so untouched vendor content can refresh on
// update without overwriting a user's edits.
struct DesktopEntryConfiguration {
    QString id;
    bool enabled{true};
    QString sourcePath;
    QString destination;
    QString contents;
    QString sourceSha256;
    QString originalContentsSha256;
    bool generated{false};
    bool userModified{false};
    bool missing{false};
    FieldProvenance provenance;

    [[nodiscard]] QJsonObject toJson() const;
    [[nodiscard]] static DesktopEntryConfiguration fromJson(const QJsonObject &object);
};

[[nodiscard]] QString desktopEntryField(const QString &contents, const QString &key);
[[nodiscard]] QString withDesktopEntryField(QString contents, const QString &key,
                                            const QString &value);
[[nodiscard]] QString desktopEntryCommand(const QString &contents);
[[nodiscard]] QString preferredDisplayName(
    const DebianMetadata &metadata,
    const QList<DesktopEntryConfiguration> &desktopEntries = {});

struct IconConfiguration {
    IconSourceKind sourceKind{IconSourceKind::None};
    QString sourcePath;
    QString sourceUrl;
    QString projectPath;
    QString sha256;
    QString format;
    QSize dimensions;
    QString iconName;
    bool missing{false};
    FieldProvenance provenance;

    [[nodiscard]] QString installedPath() const;
    [[nodiscard]] bool isConfigured() const;
    [[nodiscard]] QJsonObject toJson() const;
    [[nodiscard]] static IconConfiguration fromJson(const QJsonObject &object);
};

int applyDesktopIconName(QList<DesktopEntryConfiguration> &entries, const QString &iconName);

// Vendor AppDir entry point for an extracted AppImage. Text scripts can be
// edited in the recipe and overlaid after unsquashfs; the AppImage
// bytes stay immutable. Binary or symlink AppRuns are not text-editable.
struct AppRunConfiguration {
    bool present{false};
    bool script{false};
    QString contents;
    QString originalContents;
    QString originalContentsSha256;
    QString acknowledgedFingerprint;
    bool userModified{false};
    QString reviewReason;
    FieldProvenance provenance;

    [[nodiscard]] QString contentFingerprint() const;
    [[nodiscard]] bool requiresReview() const;
    void acknowledge();

    [[nodiscard]] QJsonObject toJson() const;
    [[nodiscard]] static AppRunConfiguration fromJson(const QJsonObject &object);
};

struct InstallMapping {
    ArchiveLayout archiveLayout{ArchiveLayout::OptBundle};
    QString optDirectory;
    QString commonPrefix;
    bool stripCommonPrefix{false};
    qint64 appImageOffset{0};
    QString binarySourcePath;
    QString binaryDestination;
    QStringList executableLinks;
    QList<LauncherMapping> launchers;
    QList<DesktopEntryConfiguration> desktopEntries;
    IconConfiguration icon;
    AppRunConfiguration appRun;

    [[nodiscard]] QJsonObject toJson() const;
    [[nodiscard]] static InstallMapping fromJson(const QJsonObject &object);
};

struct DependencyAlternative {
    QString packageName;
    QString versionOperator;
    QString version;
};

struct DependencyMapping {
    QString rawExpression;
    QList<DependencyAlternative> alternatives;
    QString archPackage;
    MappingStatus status{MappingStatus::Unresolved};
    QString mappingSource;
    double confidence{0.0};
    bool userOverride{false};
    bool ignored{false};
    bool bundled{false};
    bool provided{false};

    [[nodiscard]] QJsonObject toJson() const;
    [[nodiscard]] static DependencyMapping fromJson(const QJsonObject &object);
};

struct MaintainerScript {
    QString name;
    QString contents;
    QString acknowledgedFingerprint;

    [[nodiscard]] QString contentFingerprint() const;
    [[nodiscard]] bool requiresReview() const;
    void acknowledge();

    [[nodiscard]] QJsonObject toJson() const;
    [[nodiscard]] static MaintainerScript fromJson(const QJsonObject &object);
};

struct ScriptFinding {
    QString scriptName;
    QString kind;
    QString summary;
    QString evidence;
    QString evidenceFingerprint;
    ScriptDisposition disposition{ScriptDisposition::Unresolved};
    FieldProvenance provenance;

    [[nodiscard]] bool requiresReview() const;
    [[nodiscard]] QJsonObject toJson() const;
    [[nodiscard]] static ScriptFinding fromJson(const QJsonObject &object);
};

struct PayloadEntry {
    QString path;
    QString type;
    QString symlinkTarget;
    qint64 size{0};
    bool requiresReview{false};
    QString reviewReason;
    QString contentSha256;
    QString textPreview;
    bool previewTruncated{false};
    bool executable{false};

    [[nodiscard]] QJsonObject toJson() const;
    [[nodiscard]] static PayloadEntry fromJson(const QJsonObject &object);
};

struct PayloadRule {
    QString path;
    bool excluded{false};
    QString reason;
    bool userDecision{false};
    QString acknowledgedFingerprint;

    [[nodiscard]] QJsonObject toJson() const;
    [[nodiscard]] static PayloadRule fromJson(const QJsonObject &object);
};

struct AptRepositoryCandidate {
    QString uri;
    QString suite;
    QStringList components;
    QStringList architectures;
    QString signedBy;
    QString sourcePath;

    [[nodiscard]] QString displayText() const;
    [[nodiscard]] QJsonObject toJson() const;
    [[nodiscard]] static AptRepositoryCandidate fromJson(const QJsonObject &object);
};

struct RpmRepositoryCandidate {
    QString baseUrl;
    QString architecture;
    QStringList keyUrls;
    QString sourcePath;

    [[nodiscard]] QString displayText() const;
    [[nodiscard]] QJsonObject toJson() const;
    [[nodiscard]] static RpmRepositoryCandidate fromJson(const QJsonObject &object);
};

struct RepositorySigningKey {
    QString relativePath;
    QString sha256;
    QStringList fingerprints;
    QString sourcePath;
    QString sourceFingerprint;
    bool trusted{false};
    FieldProvenance provenance;
    QString artifactId;
    QByteArray contents;

    [[nodiscard]] QJsonObject toJson() const;
    [[nodiscard]] static RepositorySigningKey fromJson(const QJsonObject &object);
};

struct ArchLifecycleScript {
    QString fileName;
    QString contents;
    QString acknowledgedFingerprint;
    QStringList sourceFingerprints;
    QString validationMessage;
    bool validationPassed{false};
    bool manuallyModified{false};
    FieldProvenance provenance;

    [[nodiscard]] QString contentFingerprint() const;
    [[nodiscard]] bool requiresAcknowledgement() const;
    void acknowledge();
    void bindRequiredFindings(const QList<ScriptFinding> &findings);

    [[nodiscard]] QJsonObject toJson() const;
    [[nodiscard]] static ArchLifecycleScript fromJson(const QJsonObject &object);
};

struct AiChangeRecord {
    QDateTime timestamp;
    QString field;
    QString previousValue;
    QString newValue;
    QString provider;
    QString model;
    QString rationale;

    [[nodiscard]] QJsonObject toJson() const;
    [[nodiscard]] static AiChangeRecord fromJson(const QJsonObject &object);
};

struct UpdateConfiguration {
    UpdateStrategy strategy{UpdateStrategy::Manual};
    QString url;
    QString aptSuite;
    QString aptComponent;
    QString aptArchitecture;
    QString aptPackageName;
    QString aptSigningKeyring;
    QString rpmArchitecture;
    QString rpmPackageName;
    QString trustedSigningFingerprint;
    QString detectedVersion;
    QString detectedFilename;
    QString detectedSha256;
    QString detectedUrl;
    QDateTime lastChecked;
    QString lastCheckMessage;
    bool signatureVerified{false};
    QString directUrlEtag;
    QString directUrlLastModified;
    qint64 directUrlContentLength{-1};
    QString directUrlVendorValidatorName;
    QString directUrlVendorValidator;
    QString directUrlLastSha256;
    QDateTime directUrlLastFullCheck;
    int directUrlFullCheckIntervalHours{24};
    QStringList detectedCandidates;
    QList<AptRepositoryCandidate> aptCandidates;
    QList<RpmRepositoryCandidate> rpmCandidates;
    QList<RepositorySigningKey> signingKeys;
    QString githubOwner;
    QString githubRepository;
    QString githubAssetRegex;
    bool githubIncludePrereleases{false};
    QString githubEtag;
    qint64 githubReleaseId{0};
    qint64 githubAssetId{0};
    QString githubTag;
    QString githubPublisherDigest;

    [[nodiscard]] QJsonObject toJson() const;
    [[nodiscard]] static UpdateConfiguration fromJson(const QJsonObject &object);
};

struct HistoryEntry {
    QDateTime timestamp;
    QString event;
    QString detail;

    [[nodiscard]] QJsonObject toJson() const;
    [[nodiscard]] static HistoryEntry fromJson(const QJsonObject &object);
};

struct PackageArtifact {
    QString relativePath;
    QString sha256;
    QString packageName;
    QString packageVersion;
    QString architecture;
    qint64 size{0};
    QDateTime createdAt;

    [[nodiscard]] QJsonObject toJson() const;
    [[nodiscard]] static PackageArtifact fromJson(const QJsonObject &object);
};

struct BuildRecord {
    QString id;
    BuildStatus status{BuildStatus::NeverBuilt};
    QString log;
    QList<PackageArtifact> artifacts;
    QDateTime startedAt;
    QDateTime finishedAt;

    [[nodiscard]] QJsonObject toJson() const;
    [[nodiscard]] static BuildRecord fromJson(const QJsonObject &object);
};

// One vendor release and its complete, independently inspectable Arch recipe.
struct PackageRelease {
    int formatVersion{1};
    qint64 revision{1};
    QString id;
    QString projectId;
    QString displayName;
    QString iconPath;
    QString iconSourcePath;
    QString iconSha256;
    QString archPackageName;
    SourcePackageType sourceType{SourcePackageType::Debian};
    SourceAcquisition acquisition;
    InstallMapping installMapping;
    QString originalSourceFilename;
    QString sourceUrl;
    QString sourceSha256;
    QString vendorName;
    int archPkgrel{1};
    QString archPkgrelOverride;
    DebianMetadata debian;
    PackageMetadata packageMetadata;
    QList<DependencyMapping> dependencies;
    QList<MaintainerScript> maintainerScripts;
    QList<ScriptFinding> scriptFindings;
    QList<PayloadEntry> payload;
    QList<PayloadRule> payloadRules;
    QString generatedPkgbuild;
    QString generatedPkgbuildSha256;
    bool pkgbuildManuallyModified{false};
    QString customPkgbuild;
    QMap<QString, QString> customFiles;
    ArchLifecycleScript lifecycleScript;
    QMap<QString, FieldProvenance> fieldProvenance;
    QList<AiChangeRecord> aiChanges;
    UpdateConfiguration update;
    BuildStatus buildStatus{BuildStatus::NeverBuilt};
    bool automaticBuild{false};
    ReleaseState state{ReleaseState::NeedsReview};
    QString lastBuildLog;
    QStringList producedPackages;
    QList<BuildRecord> builds;
    QStringList builtArtifactIds;
    QString sourceArtifactId;
    QString iconArtifactId;
    QList<HistoryEntry> history;
    QDateTime createdAt;
    QDateTime modifiedAt;

    [[nodiscard]] QJsonObject toJson() const;
    [[nodiscard]] static PackageRelease fromJson(const QJsonObject &object);
};

struct RepoPackageRef {
    QString pkgname;
    QString arch;
    qint64 epoch{0};
    QString pkgver;
    QString pkgrel;
    QString version;
    QString filename;
    QString artifactId;
    QString signatureArtifactId;
    QString releaseId;

    [[nodiscard]] static RepoPackageRef fromJson(const QJsonObject &object);
};

struct RepoSoakStatus {
    QString pkgname;
    QString arch;
    QString pkgver;
    QString pkgrel;
    QString version;
    QString status;
    QString startedAt;
    QString eligibleAt;
    QString artifactId;
    QString releaseId;

    [[nodiscard]] static RepoSoakStatus fromJson(const QJsonObject &object);
};

struct ProjectRepository {
    qint64 revision{0};
    bool publish{false};
    bool stableChannelEnabled{false};
    bool automaticSoak{false};
    qint64 soakSecondsOverride{-1};
    qint64 librarySoakSeconds{0};
    qint64 effectiveSoakSeconds{0};
    QString originalPackageName;
    QString archPackageName;
    QString prefixDefault;
    QString packageNameOverride;
    QString effectivePackageName;
    QString publishedPackageName;
    bool pkgnameChangeWarning{false};
    bool reserved{false};
    bool hasUnstable{false};
    RepoPackageRef unstable;
    bool hasStable{false};
    RepoPackageRef stable;
    QList<RepoSoakStatus> soaks;

    [[nodiscard]] static ProjectRepository fromJson(const QJsonObject &object);
};

// An application-level project. Installed state is reconciled from pacman and is
// never inferred from the newest release.
struct Project {
    int formatVersion{5};
    qint64 revision{1};
    QString id;
    QString displayName;
    QString archPackageName;
    QString vendorName;
    QString sourceIdentity;
    QString iconPath;
    QString iconSourcePath;
    QString iconSha256;
    QList<PackageRelease> releases;
    QString installedVersion;
    QString installedReleaseId;
    bool externallyInstalled{false};
    QList<HistoryEntry> history;
    QDateTime createdAt;
    QDateTime modifiedAt;
    ProjectRepository repository;

    [[nodiscard]] PackageRelease *release(const QString &releaseId);
    [[nodiscard]] const PackageRelease *release(const QString &releaseId) const;
    [[nodiscard]] PackageRelease *newestRelease();
    [[nodiscard]] const PackageRelease *newestRelease() const;
    [[nodiscard]] PackageRelease *installedRelease();
    [[nodiscard]] const PackageRelease *installedRelease() const;
    // Update configuration belongs to a release. The project dashboard uses the
    // installed PacSmith release when one can be identified, otherwise the newest
    // analyzed release. Installed-package ownership remains a separate decision.
    [[nodiscard]] PackageRelease *activeTrackingRelease();
    [[nodiscard]] const PackageRelease *activeTrackingRelease() const;
    // True when a retained vendor version is newer than the PacSmith release
    // currently installed through pacman. Last-check detections do not count
    // until that version exists as a project release again.
    [[nodiscard]] bool hasAvailableUpdate() const;
    // True when this project is the PacSmith owner of the pacman package, not
    // merely sharing that package name with another project or an external install.
    [[nodiscard]] bool ownsInstalledPackage() const;
    [[nodiscard]] QJsonObject toJson() const;
    [[nodiscard]] static Project fromJson(const QJsonObject &object);
};

[[nodiscard]] QString mappingStatusName(MappingStatus status);
[[nodiscard]] QString buildStatusName(BuildStatus status);
[[nodiscard]] QString releaseStateName(ReleaseState state);
[[nodiscard]] ReleaseState releaseStateFromName(const QString &name);
[[nodiscard]] QString updateStrategyName(UpdateStrategy strategy);
[[nodiscard]] QString sourcePackageTypeName(SourcePackageType type);
[[nodiscard]] SourcePackageType sourcePackageTypeFromName(const QString &name);
[[nodiscard]] QString acquisitionKindName(AcquisitionKind kind);
[[nodiscard]] AcquisitionKind acquisitionKindFromName(const QString &name);
[[nodiscard]] MappingStatus mappingStatusFromName(const QString &name);
[[nodiscard]] UpdateStrategy updateStrategyFromName(const QString &name);
[[nodiscard]] QString valueOriginName(ValueOrigin origin);
[[nodiscard]] ValueOrigin valueOriginFromName(const QString &name);
[[nodiscard]] QString scriptDispositionName(ScriptDisposition disposition);
[[nodiscard]] QString scriptDispositionLabel(ScriptDisposition disposition);
[[nodiscard]] ScriptDisposition scriptDispositionFromName(const QString &name);
[[nodiscard]] int comparePackageVersions(SourcePackageType sourceType,
                                         const QString &left, const QString &right);
[[nodiscard]] int compareReleaseVersions(const PackageRelease &left,
                                         const PackageRelease &right);

} // namespace pacsmith
