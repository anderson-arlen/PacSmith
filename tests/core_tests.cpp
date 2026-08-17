#include "core/ai_service.hpp"
#include "core/ai_model_catalog_service.hpp"
#include "core/app_settings.hpp"
#include "core/background_updates.hpp"
#include "core/apt_repository.hpp"
#include "core/apt_update_service.hpp"
#include "core/apt_sources.hpp"
#include "core/control_parser.hpp"
#include "core/credential_store.hpp"
#include "core/chatgpt_auth.hpp"
#include "core/dependency_parser.hpp"
#include "core/deb_analyzer.hpp"
#include "core/github_update_service.hpp"
#include "core/lifecycle_validator.hpp"
#include "core/path_safety.hpp"
#include "core/payload_inspector.hpp"
#include "core/payload_review.hpp"
#include "core/pkgbuild_generator.hpp"
#include "core/pkgbuild_install_plan.hpp"
#include "core/project_store.hpp"
#include "core/repository_trust.hpp"
#include "core/repository_key_download_service.hpp"
#include "core/rpm_analyzer.hpp"
#include "core/rpm_repository.hpp"
#include "core/script_evidence.hpp"
#include "core/source_analyzer.hpp"
#include "core/terminal_install_service.hpp"

#include <QJsonDocument>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QDebug>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

#include <algorithm>
#include <filesystem>

class CoreTests final : public QObject {
    Q_OBJECT

private slots:
    void parsesControlFields();
    void parsesMultilineControlFields();
    void prefersApplicationNameOverPackageDescription();
    void parsesDependencies();
    void parsesAlternativesAndVersions();
    void loadsVerifiedDependencyMappings();
    void mapsChatGptDependencies();
    void acknowledgesScriptContentSpecifically();
    void extractsScriptResponsibilitiesAndAptEvidence();
    void validatesLifecycleScriptsAndContentAcknowledgement();
    void constrainsAiFindingResolutionFingerprints();
    void appliesAiResolutionWithinTrustBoundaries();
    void appliesAiAppRunRewriteForExtractedAppImage();
    void appliesRequiredAiDependencyTreatment();
    void requiresApprovalForUnclassifiedAiPayloadChanges();
    void rejectsUnevidencedAiSigningKeysAndUnsafeInformationRequests();
    void parsesAptSourceFormats();
    void comparesDebianVersions();
    void parsesAptRepositoryMetadata();
    void extractsPayloadRpmRepositoryEvidence();
    void parsesRpmRepositoryMetadata();
    void comparesRpmVersions();
    void verifiesPinnedAptRepositorySignatures();
    void validatesRepositorySigningKeyUrls();
    void tracksContentSpecificPayloadDecisions();
    void serializesProjectsAndOverrides();
    void persistsAiSettingsOutsideProjectData();
    void persistsBackgroundUpdateSettings();
    void buildsSystemdCalendarSchedules();
    void persistsMultipleReleases();
    void selectsActiveTrackingRelease();
    void reportsInstalledUpdateStatus();
    void recordsUninspectedGitHubDiscoveries();
    void carriesInstallMappingAcrossGitHubVersions();
    void preservesRepositoryFirstImportConfiguration();
    void parsesAiModelCatalog();
    void parsesChatGptCredentialsAndCatalog();
    void buildsExternalTerminalCommandsSafely();
    void buildsNonInteractivePacmanArgumentsSafely();
    void buildsRebuildableMakepkgArguments();
    void validatesInstallSessionProtocol();
    void encryptsCredentialsWithAge();
    void migratesScriptEvidenceForExistingProjects();
    void handlesProjectPaths();
    void detectsManualPkgbuildEdits();
    void roundTripsGuidedAndCustomPkgbuilds();
    void detectsExternalLifecycleEdits();
    void reanalyzesReleaseFromBlankPackageSetup();
    void detectsDebDeclaredOptCommandWithoutExecutingScript();
    void inspectsDebAndAppImagePayloadContents();
    void deletesUninstalledProject();
    void refusesToDeleteInstalledProject();
    void generatesPkgbuild();
    void generatesMultiSourcePkgbuilds();
    void parsesPkgbuildInstallPlans();
    void synchronizesIntegrationIconSource();
    void parsesRpmHeadersWithoutExecutingScripts();
    void inspectsArchiveIconsRepositoryEvidenceAndPrivilegedModes();
    void reviewsUnsafeArchiveSymlinksWithoutFailingImport();
    void mapsArchiveDesktopExecToUsrBinCommand();
    void flagsMissingArchiveDesktopCommandForReview();
    void detectsStandaloneElfWithoutExecutingIt();
    void rejectsAppImagesBeforeElfDetection();
    void acceptsStandardExternalAppImageRuntimeSymlinks();
    void flagsAppRunFilenameDispatchForReview();
    void selectsGitHubReleaseAssets();
    void sanitizesPackageNames_data();
    void sanitizesPackageNames();
    void translatesVersions_data();
    void translatesVersions();
    void validatesArchivePaths();
    void preservesUserMappingOverrides();
};

void CoreTests::parsesControlFields() {
    const QByteArray control = "Package: Vendor-App\nVersion: 1.2.3-1\nArchitecture: amd64\n"
                               "Maintainer: Vendor <packages@example.com>\nHomepage: https://example.com\n"
                               "X-Vendor-Field: retained\n";
    const auto parsed = pacsmith::ControlParser::parsePackage(QByteArrayView(control));
    QCOMPARE(parsed.package, QStringLiteral("Vendor-App"));
    QCOMPARE(parsed.version, QStringLiteral("1.2.3-1"));
    QCOMPARE(parsed.architecture, QStringLiteral("amd64"));
    QCOMPARE(parsed.rawFields.value(QStringLiteral("X-Vendor-Field")), QStringLiteral("retained"));
}

void CoreTests::parsesMultilineControlFields() {
    const QByteArray control = "Package: example\nDescription: Short summary\n long detail\n .\n final paragraph\n";
    const auto parsed = pacsmith::ControlParser::parsePackage(QByteArrayView(control));
    QCOMPARE(parsed.description, QStringLiteral("Short summary\nlong detail\n\nfinal paragraph"));
}

void CoreTests::prefersApplicationNameOverPackageDescription() {
    pacsmith::DebianMetadata metadata;
    metadata.package = QStringLiteral("code");
    metadata.description = QStringLiteral("Code editing. Redefined.\nVisual Studio Code is a code editor.");
    QCOMPARE(pacsmith::preferredDisplayName(metadata), QStringLiteral("code"));

    pacsmith::DesktopEntryConfiguration desktop;
    desktop.enabled = true;
    desktop.id = QStringLiteral("code");
    desktop.sourcePath = QStringLiteral("usr/share/applications/code.desktop");
    desktop.contents = QStringLiteral("[Desktop Entry]\nType=Application\nName=Visual Studio Code\nExec=/usr/share/code/code %F\n");
    QCOMPARE(pacsmith::preferredDisplayName(metadata, {desktop}),
             QStringLiteral("Visual Studio Code"));
}

void CoreTests::parsesDependencies() {
    const auto dependencies = pacsmith::DependencyParser::parse(
        QStringLiteral("libgtk-3-0, libnss3:any, vendor-runtime [amd64]"));
    QCOMPARE(dependencies.size(), 3);
    QCOMPARE(dependencies.at(0).alternatives.first().packageName, QStringLiteral("libgtk-3-0"));
    QCOMPARE(dependencies.at(1).alternatives.first().packageName, QStringLiteral("libnss3"));
    QCOMPARE(dependencies.at(2).alternatives.first().packageName, QStringLiteral("vendor-runtime"));
}

void CoreTests::parsesAlternativesAndVersions() {
    const auto dependencies = pacsmith::DependencyParser::parse(
        QStringLiteral("libfoo (>= 1.2) | libbar (<< 3:4.0-1), plain"));
    QCOMPARE(dependencies.size(), 2);
    QCOMPARE(dependencies.first().alternatives.size(), 2);
    QCOMPARE(dependencies.first().alternatives.at(0).versionOperator, QStringLiteral(">="));
    QCOMPARE(dependencies.first().alternatives.at(0).version, QStringLiteral("1.2"));
    QCOMPARE(dependencies.first().alternatives.at(1).packageName, QStringLiteral("libbar"));
    QCOMPARE(dependencies.first().alternatives.at(1).versionOperator, QStringLiteral("<<"));
    QCOMPARE(dependencies.first().alternatives.at(1).version, QStringLiteral("3:4.0-1"));
}

void CoreTests::loadsVerifiedDependencyMappings() {
    const auto mappings = pacsmith::DependencyParser::loadVerifiedMappings();
    QCOMPARE(mappings.value(QStringLiteral("libgtk-3-0")), QStringLiteral("gtk3"));
    QCOMPARE(mappings.value(QStringLiteral("libc6")), QStringLiteral("glibc"));
}

void CoreTests::mapsChatGptDependencies() {
    const QMap<QString, QString> expected{
        {QStringLiteral("libnotify4"), QStringLiteral("libnotify")},
        {QStringLiteral("xdg-utils"), QStringLiteral("xdg-utils")},
        {QStringLiteral("libatspi2.0-0"), QStringLiteral("at-spi2-core")},
        {QStringLiteral("libdrm2"), QStringLiteral("libdrm")},
        {QStringLiteral("libgbm1"), QStringLiteral("mesa")},
        {QStringLiteral("libxcb-dri3-0"), QStringLiteral("libxcb")},
        {QStringLiteral("libglib2.0-bin"), QStringLiteral("glib2")},
        {QStringLiteral("libatk-bridge2.0-0"), QStringLiteral("at-spi2-core")},
        {QStringLiteral("libcups2"), QStringLiteral("libcups")},
        {QStringLiteral("libgdk-pixbuf-2.0-0"), QStringLiteral("gdk-pixbuf2")},
        {QStringLiteral("libgl1"), QStringLiteral("libglvnd")},
        {QStringLiteral("libgraphite2-3"), QStringLiteral("graphite")},
        {QStringLiteral("libnspr4"), QStringLiteral("nspr")},
        {QStringLiteral("libssl3"), QStringLiteral("openssl")},
        {QStringLiteral("libudev1"), QStringLiteral("systemd-libs")},
        {QStringLiteral("libusb-1.0-0"), QStringLiteral("libusb")},
        {QStringLiteral("libx11-xcb1"), QStringLiteral("libx11")},
        {QStringLiteral("libxkbcommon0"), QStringLiteral("libxkbcommon")},
        {QStringLiteral("xz-utils"), QStringLiteral("xz")}};
    auto dependencies = pacsmith::DependencyParser::parse(expected.keys().join(QStringLiteral(", ")));
    QVERIFY(pacsmith::DependencyParser::applyVerifiedMappings(
        dependencies, pacsmith::DependencyParser::loadVerifiedMappings()));
    QCOMPARE(dependencies.size(), expected.size());
    for (const auto &dependency : dependencies) {
        QCOMPARE(dependency.status, pacsmith::MappingStatus::Resolved);
        QCOMPARE(dependency.archPackage, expected.value(dependency.alternatives.first().packageName));
    }

    auto alternatives = pacsmith::DependencyParser::parse(QStringLiteral(
        "libglib2.0-bin | kde-cli-tools | kde-runtime | trash-cli | gvfs-bin"));
    QVERIFY(pacsmith::DependencyParser::applyVerifiedMappings(
        alternatives, pacsmith::DependencyParser::loadVerifiedMappings()));
    QCOMPARE(alternatives.first().archPackage, QStringLiteral("glib2"));

    auto blankOverride = pacsmith::DependencyParser::parse(QStringLiteral("libnotify4"));
    blankOverride.first().userOverride = true;
    blankOverride.first().mappingSource = QStringLiteral("user override");
    blankOverride.first().confidence = 1.0;
    QVERIFY(pacsmith::DependencyParser::applyVerifiedMappings(
        blankOverride, pacsmith::DependencyParser::loadVerifiedMappings()));
    QCOMPARE(blankOverride.first().archPackage, QStringLiteral("libnotify"));
    QVERIFY(!blankOverride.first().userOverride);
}

void CoreTests::acknowledgesScriptContentSpecifically() {
    pacsmith::MaintainerScript script{QStringLiteral("postinst"), QStringLiteral("#!/bin/sh\necho reviewed\n"), {}};
    QVERIFY(script.requiresReview());
    script.acknowledge();
    QVERIFY(!script.requiresReview());
    const auto fingerprint = script.acknowledgedFingerprint;

    const auto restored = pacsmith::MaintainerScript::fromJson(script.toJson());
    QCOMPARE(restored.acknowledgedFingerprint, fingerprint);
    QVERIFY(!restored.requiresReview());

    auto changed = restored;
    changed.contents += QStringLiteral("echo changed\n");
    QVERIFY(changed.requiresReview());
    auto renamed = restored;
    renamed.name = QStringLiteral("preinst");
    QVERIFY(renamed.requiresReview());
}

void CoreTests::extractsScriptResponsibilitiesAndAptEvidence() {
    QByteArray keyBytes(96, '\x42');
    keyBytes[0] = static_cast<char>(0x99);
    const auto armoredKey = QStringLiteral(
        "-----BEGIN PGP PUBLIC KEY BLOCK-----\n"
        "\n"
        "bG9jYWxseS12YWxpZGF0ZWQtdGVzdC1maXh0dXJl\n"
        "=abcd\n"
        "-----END PGP PUBLIC KEY BLOCK-----\n");
    const auto scriptContents = QStringLiteral(
        "#!/bin/sh\n"
        "SIGNING_KEY_BASE64='%1'\n"
        "cat > /etc/apt/sources.list.d/vendor.sources <<'EOF'\n"
        "Types: deb\n"
        "URIs: https://packages.vendor.example/linux/deb\n"
        "Suites: stable\n"
        "Components: main\n"
        "Architectures: amd64\n"
        "Signed-By: /usr/share/keyrings/vendor.gpg\n"
        "EOF\n"
        "echo \"%2\" | gpg --dearmor > /usr/share/keyrings/vendor.gpg\n"
        "update-desktop-database -q || true\n"
        "aa-enabled && apparmor_parser -r /etc/apparmor.d/vendor || true\n")
                                    .arg(QString::fromLatin1(keyBytes.toBase64()), armoredKey);
    const auto evidence = pacsmith::ScriptEvidenceAnalyzer::analyze(
        {{QStringLiteral("postinst"), scriptContents, {}}});
    QCOMPARE(evidence.aptCandidates.size(), 1);
    QCOMPARE(evidence.aptCandidates.first().uri,
             QStringLiteral("https://packages.vendor.example/linux/deb"));
    QCOMPARE(evidence.aptCandidates.first().signedBy,
             QStringLiteral("/usr/share/keyrings/vendor.gpg"));
    QCOMPARE(evidence.signingKeys.size(), 2);
    QCOMPARE(evidence.signingKeys.first().contents, keyBytes);
    QCOMPARE(evidence.signingKeys.at(1).contents, armoredKey.toLatin1());
    QCOMPARE(evidence.signingKeys.at(1).sourcePath,
             QStringLiteral("control/postinst:armored-openpgp-1"));

    const auto hasDisposition = [&evidence](const QString &kind,
                                            const pacsmith::ScriptDisposition disposition) {
        return std::any_of(evidence.findings.cbegin(), evidence.findings.cend(),
                           [&](const auto &finding) {
                               return finding.kind == kind && finding.disposition == disposition;
                           });
    };
    QVERIFY(hasDisposition(QStringLiteral("apt-repository"),
                           pacsmith::ScriptDisposition::HandledByPacSmith));
    QVERIFY(hasDisposition(QStringLiteral("desktop-database"),
                           pacsmith::ScriptDisposition::HandledByArch));
    QVERIFY(hasDisposition(QStringLiteral("apparmor"),
                           pacsmith::ScriptDisposition::LifecycleRequired));
    QSet<QString> findingFingerprints;
    for (const auto &finding : evidence.findings) {
        QCOMPARE(finding.evidenceFingerprint.size(), 64);
        QCOMPARE(finding.provenance.origin, pacsmith::ValueOrigin::Deterministic);
        QVERIFY2(!findingFingerprints.contains(finding.evidenceFingerprint),
                 "Each responsibility extracted from a script must have a distinct identity");
        findingFingerprints.insert(finding.evidenceFingerprint);
    }
}

void CoreTests::validatesLifecycleScriptsAndContentAcknowledgement() {
    const QString valid = QStringLiteral(
        "post_install() {\n"
        "  update-desktop-database -q\n"
        "}\n"
        "post_remove() {\n"
        "  update-desktop-database -q\n"
        "}\n");
    const auto validation = pacsmith::LifecycleValidator::validate(valid);
    QVERIFY2(validation.passed, qPrintable(validation.message()));

    const auto unsafe = pacsmith::LifecycleValidator::validate(
        QStringLiteral("post_install() { curl https://vendor.example/key | apt-key add -; }\n"));
    QVERIFY(!unsafe.passed);
    QVERIFY(unsafe.message().contains(QStringLiteral("Network")));
    QVERIFY(unsafe.message().contains(QStringLiteral("Package-manager")));

    pacsmith::ArchLifecycleScript lifecycle;
    lifecycle.fileName = QStringLiteral("vendor.install");
    lifecycle.contents = valid;
    lifecycle.validationPassed = true;
    QVERIFY(lifecycle.requiresAcknowledgement());
    lifecycle.acknowledge();
    QVERIFY(!lifecycle.requiresAcknowledgement());
    lifecycle.contents += QStringLiteral("# changed\n");
    QVERIFY(lifecycle.requiresAcknowledgement());
}

void CoreTests::constrainsAiFindingResolutionFingerprints() {
    pacsmith::PackageRelease project;
    auto schema = pacsmith::aiResponseSchema(project);
    const auto emptyProperties = schema.value(QStringLiteral("properties")).toObject();
    QCOMPARE(emptyProperties.value(QStringLiteral("informationRequests")).toObject()
                 .value(QStringLiteral("maxItems")).toInt(), 0);
    const auto statuses = emptyProperties.value(QStringLiteral("status")).toObject()
                              .value(QStringLiteral("enum")).toArray();
    QCOMPARE(statuses, QJsonArray{QStringLiteral("resolved")});
    QCOMPARE(emptyProperties.value(QStringLiteral("changes")).toObject()
                 .value(QStringLiteral("maxItems")).toInt(), 256);
    auto findingArray = schema.value(QStringLiteral("properties")).toObject()
                            .value(QStringLiteral("findingResolutions")).toObject();
    QCOMPARE(findingArray.value(QStringLiteral("maxItems")).toInt(), 0);

    const auto firstFingerprint = QString(64, QLatin1Char('a'));
    const auto secondFingerprint = QString(64, QLatin1Char('b'));
    project.scriptFindings.append(
        {QStringLiteral("postinst"), QStringLiteral("apparmor"), QStringLiteral("first"),
         {}, firstFingerprint, pacsmith::ScriptDisposition::Unresolved, {}});
    project.scriptFindings.append(
        {QStringLiteral("postrm"), QStringLiteral("desktop-database"), QStringLiteral("second"),
         {}, secondFingerprint, pacsmith::ScriptDisposition::Unresolved, {}});

    schema = pacsmith::aiResponseSchema(project);
    findingArray = schema.value(QStringLiteral("properties")).toObject()
                       .value(QStringLiteral("findingResolutions")).toObject();
    QCOMPARE(findingArray.value(QStringLiteral("maxItems")).toInt(), 2);
    const auto fingerprintSchema = findingArray.value(QStringLiteral("items")).toObject()
                                       .value(QStringLiteral("properties")).toObject()
                                       .value(QStringLiteral("evidenceFingerprint")).toObject();
    QCOMPARE(fingerprintSchema.value(QStringLiteral("enum")).toArray(),
             QJsonArray({firstFingerprint, secondFingerprint}));

    const auto githubSchema = pacsmith::aiResponseSchema(project, false);
    QCOMPARE(githubSchema.value(QStringLiteral("properties")).toObject()
                 .value(QStringLiteral("findingResolutions")).toObject()
                 .value(QStringLiteral("maxItems")).toInt(),
             0);

    pacsmith::PayloadEntry lintian;
    lintian.path = QStringLiteral("usr/share/lintian/overrides/vendor");
    lintian.type = QStringLiteral("file");
    lintian.contentSha256 = QString(64, QLatin1Char('c'));
    project.scriptFindings.clear();
    project.payload.append(lintian);
    pacsmith::AiResolution invalid;
    invalid.success = true;
    invalid.findingResolutions.append(
        {lintian.contentSha256, pacsmith::ScriptDisposition::HandledByPacSmith,
         QStringLiteral("Exclude Lintian metadata"), QStringLiteral("Debian-only metadata")});
    const auto applied = pacsmith::AiResolutionApplier::apply(project, invalid);
    QCOMPARE(applied.errors.size(), 1);
    QVERIFY(applied.errors.first().contains(QStringLiteral("content SHA256")));
    QVERIFY(applied.errors.first().contains(QStringLiteral("payload.usr/share/lintian/overrides/vendor.treatment")));
}

void CoreTests::appliesAiResolutionWithinTrustBoundaries() {
    pacsmith::PackageRelease project;
    project.archPackageName = QStringLiteral("vendor-bin");
    project.debian.version = QStringLiteral("1.0");
    project.dependencies = pacsmith::DependencyParser::parse(QStringLiteral("vendor-runtime"));
    project.update.url = QStringLiteral("https://user.example/repository");
    project.fieldProvenance.insert(
        QStringLiteral("update.url"),
        {pacsmith::ValueOrigin::User, {}, {}, QStringLiteral("user"),
         QStringLiteral("entered by user"), QDateTime::currentDateTimeUtc(), true});
    project.update.signingKeys.append(
        {QStringLiteral("files/keys/vendor.gpg"), QString(64, QLatin1Char('a')),
         {QString(40, QLatin1Char('B'))}, QStringLiteral("control/postinst:SIGNING_KEY_BASE64"),
         QStringLiteral("key-evidence"), true,
         {pacsmith::ValueOrigin::Deterministic, {}, {}, QStringLiteral("key-evidence"),
          QStringLiteral("embedded vendor key"), QDateTime::currentDateTimeUtc(), false}});
    project.scriptFindings.append(
        {QStringLiteral("postinst"), QStringLiteral("apparmor"), QStringLiteral("needs decision"),
         QStringLiteral("apparmor_parser"), QStringLiteral("finding-fingerprint"),
         pacsmith::ScriptDisposition::Unresolved, {}});
    pacsmith::PayloadEntry payload;
    payload.path = QStringLiteral("etc/apparmor.d/vendor");
    payload.type = QStringLiteral("file");
    payload.size = 20;
    payload.requiresReview = true;
    payload.reviewReason = QStringLiteral("AppArmor policy");
    payload.contentSha256 = QString(64, QLatin1Char('c'));
    pacsmith::PayloadEntry payloadDirectory;
    payloadDirectory.path = QStringLiteral("etc/apparmor.d");
    payloadDirectory.type = QStringLiteral("directory");
    // Directories may be unflagged themselves while containing review-sensitive files.
    // A subtree-fingerprinted decision must still be eligible in that case.
    payloadDirectory.requiresReview = false;
    payloadDirectory.reviewReason = QStringLiteral("AppArmor policy");
    pacsmith::PayloadEntry sandboxPayload;
    sandboxPayload.path = QStringLiteral("usr/share/vendor/chrome-sandbox");
    sandboxPayload.type = QStringLiteral("file");
    sandboxPayload.requiresReview = true;
    sandboxPayload.reviewReason = QStringLiteral("Sandbox helper");
    sandboxPayload.contentSha256 = QString(64, QLatin1Char('d'));
    project.payload.append(payloadDirectory);
    project.payload.append(payload);
    project.payload.append(sandboxPayload);

    pacsmith::AiResolution resolution;
    resolution.success = true;
    resolution.provider = QStringLiteral("openai");
    resolution.model = QStringLiteral("test-model");
    resolution.rationale = QStringLiteral("Converted Debian responsibilities to Arch semantics");
    resolution.changes = {
        {QStringLiteral("update.url"), QStringLiteral("https://ai.example/repository"), QStringLiteral("candidate")},
        {QStringLiteral("update.aptSuite"), QStringLiteral("stable"), QStringLiteral("source stanza")},
        {QStringLiteral("update.signingKeySha256"), QString(64, QLatin1Char('a')), QStringLiteral("embedded key")},
        {QStringLiteral("dependency.0.archPackage"), QStringLiteral("vendor-runtime-arch"), QStringLiteral("package match")},
        {QStringLiteral("payload.etc/apparmor.d.treatment"), QStringLiteral("exclude"), QStringLiteral("optional policy")},
        {QStringLiteral("payload.usr/share/vendor/chrome-sandbox.treatment"), QStringLiteral("include"), QStringLiteral("retain sandbox helper")}};
    resolution.findingResolutions = {
        {QStringLiteral("finding-fingerprint"), pacsmith::ScriptDisposition::LifecycleRequired,
         QStringLiteral("Install the optional profile only when AppArmor tooling exists"), QStringLiteral("Arch-specific")}};
    resolution.lifecycleScript = QStringLiteral(
        "post_install() {\n"
        "  if [[ -x /usr/bin/apparmor_parser ]]; then\n"
        "    /usr/bin/apparmor_parser -r /etc/apparmor.d/vendor || true\n"
        "  fi\n"
        "}\n");

    const auto conflicts = pacsmith::AiResolutionApplier::manualConflicts(project, resolution);
    QCOMPARE(conflicts, QStringList{QStringLiteral("update.url")});
    const auto applied = pacsmith::AiResolutionApplier::apply(project, resolution);
    QVERIFY(applied.changed);
    QVERIFY(applied.errors.isEmpty());
    QCOMPARE(project.update.url, QStringLiteral("https://user.example/repository"));
    QCOMPARE(project.update.aptSuite, QStringLiteral("stable"));
    QCOMPARE(project.update.aptSigningKeyring, QStringLiteral("files/keys/vendor.gpg"));
    QCOMPARE(project.update.trustedSigningFingerprint, QString(40, QLatin1Char('B')));
    QCOMPARE(project.dependencies.first().archPackage, QStringLiteral("vendor-runtime-arch"));
    QCOMPARE(project.dependencies.first().status, pacsmith::MappingStatus::Resolved);
    QCOMPARE(project.payloadRules.first().path, QStringLiteral("etc/apparmor.d"));
    QCOMPARE(pacsmith::PayloadReview::state(project, project.payload.at(1)).disposition,
             pacsmith::PayloadDisposition::Excluded);
    const auto sandboxRule = std::find_if(
        project.payloadRules.cbegin(), project.payloadRules.cend(), [](const auto &rule) {
            return rule.path == QStringLiteral("usr/share/vendor/chrome-sandbox");
        });
    QVERIFY(sandboxRule != project.payloadRules.cend());
    QVERIFY(!sandboxRule->excluded);
    const auto sandboxChange = std::find_if(
        project.aiChanges.cbegin(), project.aiChanges.cend(), [](const auto &change) {
            return change.field == QStringLiteral("payload.usr/share/vendor/chrome-sandbox.treatment");
        });
    QVERIFY(sandboxChange != project.aiChanges.cend());
    QCOMPARE(sandboxChange->newValue, QStringLiteral("keep"));
    QCOMPARE(project.scriptFindings.first().provenance.origin, pacsmith::ValueOrigin::Ai);
    QVERIFY(project.lifecycleScript.validationPassed);
    QVERIFY(project.lifecycleScript.requiresAcknowledgement());
    QVERIFY(project.lifecycleScript.sourceFingerprints.contains(QStringLiteral("finding-fingerprint")));
    QVERIFY(project.aiChanges.size() >= 4);
    QCOMPARE(project.fieldProvenance.value(QStringLiteral("update.aptSuite")).origin,
             pacsmith::ValueOrigin::Ai);

    const auto approved = pacsmith::AiResolutionApplier::apply(
        project, resolution, {QStringLiteral("update.url")});
    QVERIFY(approved.changed);
    QCOMPARE(project.update.url, QStringLiteral("https://ai.example/repository"));
    QVERIFY(project.fieldProvenance.value(QStringLiteral("update.url")).userApproved);
}

void CoreTests::appliesAiAppRunRewriteForExtractedAppImage() {
    pacsmith::PackageRelease project;
    project.sourceType = pacsmith::SourcePackageType::AppImage;
    project.archPackageName = QStringLiteral("freac");
    project.installMapping.optDirectory = QStringLiteral("freac");
    pacsmith::LauncherMapping launcher;
    launcher.enabled = true;
    launcher.sourcePath = QStringLiteral("AppRun");
    launcher.commandName = QStringLiteral("freac");
    launcher.destination = QStringLiteral("/usr/bin/freac");
    launcher.kind = pacsmith::LauncherKind::Wrapper;
    project.installMapping.launchers.append(launcher);
    project.installMapping.appRun.present = true;
    project.installMapping.appRun.script = true;
    project.installMapping.appRun.originalContents = QStringLiteral(
        "#!/bin/bash\n"
        "HERE=\"$(dirname \"$(readlink -f \"${0}\")\")\"\n"
        "if [ ! -z \"$APPIMAGE\" ]; then\n"
        "  BINARY_NAME=$(basename \"$ARGV0\")\n"
        "else\n"
        "  BINARY_NAME=$(basename \"$0\")\n"
        "fi\n"
        "exec \"$HERE/$BINARY_NAME\" \"$@\"\n");
    project.installMapping.appRun.contents = project.installMapping.appRun.originalContents;
    project.installMapping.appRun.reviewReason = QStringLiteral("filename dispatch");
    QVERIFY(project.installMapping.appRun.requiresReview());

    pacsmith::AiResolution resolution;
    resolution.success = true;
    resolution.provider = QStringLiteral("chatgpt");
    resolution.model = QStringLiteral("test-model");
    const auto rewritten = QStringLiteral(
        "#!/bin/sh\nexport LD_LIBRARY_PATH=\"$APPDIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}\"\n"
        "exec \"$APPDIR/freac\" \"$@\"\n");
    resolution.changes.append(
        {QStringLiteral("appRun.contents"), rewritten,
         QStringLiteral("Extracted AppDir does not need APPIMAGE/BINARY_NAME dispatch")});

    const auto applied = pacsmith::AiResolutionApplier::apply(project, resolution);
    QVERIFY2(applied.errors.isEmpty(), qPrintable(applied.errors.join(QLatin1Char('\n'))));
    QVERIFY(applied.changed);
    QCOMPARE(project.installMapping.appRun.contents, rewritten);
    QVERIFY(project.installMapping.appRun.userModified);
    QVERIFY(!project.installMapping.appRun.requiresReview());
    const auto pkgbuild = pacsmith::PkgbuildGenerator::generate(project);
    QVERIFY(pkgbuild.contains(QStringLiteral("exec \"$APPDIR/freac\" \"$@\"")));
    QVERIFY(pkgbuild.contains(QStringLiteral("unset APPIMAGE")));
    QVERIFY(pkgbuild.contains(QStringLiteral("exec \"/opt/freac/AppRun\" \"$@\"")));

    pacsmith::AiResolution second;
    second.success = true;
    second.changes.append(
        {QStringLiteral("appRun.contents"), QStringLiteral("#!/bin/sh\nexec \"$APPDIR/other\" \"$@\"\n"),
         QStringLiteral("another rewrite")});
    QCOMPARE(pacsmith::AiResolutionApplier::manualConflicts(project, second),
             QStringList{QStringLiteral("appRun.contents")});
    const auto blocked = pacsmith::AiResolutionApplier::apply(project, second);
    QCOMPARE(project.installMapping.appRun.contents, rewritten);

    pacsmith::PackageRelease binaryProject;
    binaryProject.sourceType = pacsmith::SourcePackageType::AppImage;
    binaryProject.installMapping.appRun.present = true;
    binaryProject.installMapping.appRun.script = false;
    const auto rejected = pacsmith::AiResolutionApplier::apply(binaryProject, resolution);
    QVERIFY(!rejected.errors.isEmpty());
    QVERIFY(rejected.errors.first().contains(QStringLiteral("appRun.contents")));
}

void CoreTests::appliesRequiredAiDependencyTreatment() {
    pacsmith::PackageRelease project;
    auto dependency = pacsmith::DependencyParser::parse(
        QStringLiteral("libqt5webengine5 (>= 5.7.1)")).first();
    dependency.archPackage = QStringLiteral("qt5-webengine");
    dependency.status = pacsmith::MappingStatus::Provided;
    dependency.provided = true;
    dependency.mappingSource = QStringLiteral("AI: chatgpt/test-model");
    project.dependencies.append(dependency);

    pacsmith::AiResolution resolution;
    resolution.success = true;
    resolution.provider = QStringLiteral("chatgpt");
    resolution.model = QStringLiteral("test-model");
    resolution.changes.append(
        {QStringLiteral("dependency.0.treatment"), QStringLiteral("required"),
         QStringLiteral("The payload contains no Qt WebEngine shared library")});

    const auto applied = pacsmith::AiResolutionApplier::apply(project, resolution);
    QVERIFY2(applied.errors.isEmpty(), qPrintable(applied.errors.join(QLatin1Char('\n'))));
    QVERIFY(applied.changed);
    QCOMPARE(project.dependencies.first().status, pacsmith::MappingStatus::Resolved);
    QVERIFY(!project.dependencies.first().provided);
    QVERIFY(!project.dependencies.first().bundled);
    QVERIFY(!project.dependencies.first().ignored);
    QVERIFY(pacsmith::PkgbuildGenerator::generate(project).contains(
        QStringLiteral("depends=('qt5-webengine')")));

    pacsmith::AiResolution unavailable;
    unavailable.success = true;
    unavailable.provider = QStringLiteral("chatgpt");
    unavailable.model = QStringLiteral("test-model");
    unavailable.changes.append(
        {QStringLiteral("dependency.0.treatment"), QStringLiteral("unresolved"),
         QStringLiteral("The proposed repository package is unavailable")});
    const auto cleared = pacsmith::AiResolutionApplier::apply(project, unavailable);
    QVERIFY2(cleared.errors.isEmpty(), qPrintable(cleared.errors.join(QLatin1Char('\n'))));
    QCOMPARE(project.dependencies.first().status, pacsmith::MappingStatus::Unresolved);
    QVERIFY(project.dependencies.first().archPackage.isEmpty());
    QVERIFY(!pacsmith::PkgbuildGenerator::generate(project).contains(QStringLiteral("qt5-webengine")));
}

void CoreTests::requiresApprovalForUnclassifiedAiPayloadChanges() {
    pacsmith::PackageRelease project;
    pacsmith::PayloadEntry payload;
    payload.path = QStringLiteral("usr/share/vendor/optional-metadata");
    payload.type = QStringLiteral("file");
    payload.size = 42;
    payload.contentSha256 = QString(64, QLatin1Char('a'));
    project.payload.append(payload);

    pacsmith::AiResolution resolution;
    resolution.success = true;
    resolution.provider = QStringLiteral("chatgpt");
    resolution.model = QStringLiteral("test-model");
    const auto field = QStringLiteral("payload.usr/share/vendor/optional-metadata.treatment");
    resolution.changes.append(
        {field, QStringLiteral("exclude"), QStringLiteral("Not useful on Arch")});

    QCOMPARE(pacsmith::AiResolutionApplier::explicitApprovalRequired(project, resolution),
             QStringList{field});
    const auto blocked = pacsmith::AiResolutionApplier::apply(project, resolution);
    QVERIFY(!blocked.errors.isEmpty());
    QVERIFY(project.payloadRules.isEmpty());

    const auto approved = pacsmith::AiResolutionApplier::apply(project, resolution, {field});
    QVERIFY2(approved.errors.isEmpty(), qPrintable(approved.errors.join(QLatin1Char('\n'))));
    QCOMPARE(project.payloadRules.size(), 1);
    QVERIFY(project.payloadRules.first().excluded);
    QVERIFY(project.payloadRules.first().userDecision);
    QCOMPARE(pacsmith::PayloadReview::state(project, project.payload.first()).disposition,
             pacsmith::PayloadDisposition::Excluded);
    QVERIFY(!pacsmith::PayloadReview::state(project, project.payload.first()).needsReview);
    QVERIFY(project.fieldProvenance.value(field).userApproved);

    project.payload.first().contentSha256 = QString(64, QLatin1Char('b'));
    QVERIFY(pacsmith::PayloadReview::state(project, project.payload.first()).needsReview);
}

void CoreTests::rejectsUnevidencedAiSigningKeysAndUnsafeInformationRequests() {
    pacsmith::PackageRelease project;
    pacsmith::AiResolution resolution;
    resolution.provider = QStringLiteral("xai");
    resolution.model = QStringLiteral("test-model");
    resolution.changes.append({QStringLiteral("update.signingKeySha256"),
                               QString(64, QLatin1Char('f')), QStringLiteral("invented")});
    const auto applied = pacsmith::AiResolutionApplier::apply(project, resolution);
    QVERIFY(!applied.errors.isEmpty());
    QVERIFY(applied.errors.first().contains(QStringLiteral("Field: update.signingKeySha256")));
    QVERIFY(applied.errors.first().contains(QStringLiteral("Proposed value:")));
    QVERIFY(applied.errors.first().contains(QStringLiteral("PacSmith rejection reason:")));
    QVERIFY(project.update.aptSigningKeyring.isEmpty());
    QVERIFY(project.update.trustedSigningFingerprint.isEmpty());

    const auto unsupported = pacsmith::SystemInformationBroker::execute(
        {QStringLiteral("1"), QStringLiteral("shell"), QStringLiteral("id"), QStringLiteral("test")});
    QVERIFY(unsupported.contains(QStringLiteral("error")));
    const auto unsafePackage = pacsmith::SystemInformationBroker::execute(
        {QStringLiteral("2"), QStringLiteral("installed-package"),
         QStringLiteral("glibc;touch /tmp/not-allowed"), QStringLiteral("test")});
    QCOMPARE(unsafePackage.value(QStringLiteral("error")).toString(), QStringLiteral("Unsafe package name"));
    const auto unsafeRepositoryPackage = pacsmith::SystemInformationBroker::execute(
        {QStringLiteral("2b"), QStringLiteral("repository-package"),
         QStringLiteral("glibc;touch /tmp/not-allowed"), QStringLiteral("test")});
    QCOMPARE(unsafeRepositoryPackage.value(QStringLiteral("error")).toString(),
             QStringLiteral("Unsafe package name"));
    const auto repositoryPackage = pacsmith::SystemInformationBroker::execute(
        {QStringLiteral("2c"), QStringLiteral("repository-package"),
         QStringLiteral("glibc"), QStringLiteral("test")});
    QVERIFY(repositoryPackage.value(QStringLiteral("available")).toBool());
    const auto missingRepositoryPackage = pacsmith::SystemInformationBroker::execute(
        {QStringLiteral("2d"), QStringLiteral("repository-package"),
         QStringLiteral("pacsmith-package-that-does-not-exist"), QStringLiteral("test")});
    QVERIFY(!missingRepositoryPackage.value(QStringLiteral("available")).toBool());
    const auto architecture = pacsmith::SystemInformationBroker::execute(
        {QStringLiteral("3"), QStringLiteral("architecture"), {}, QStringLiteral("test")});
    QVERIFY(!architecture.value(QStringLiteral("value")).toString().isEmpty());
}

void CoreTests::parsesAptSourceFormats() {
    const QByteArray oneLine =
        "# vendor repository\n"
        "deb [arch=amd64 signed-by=/usr/share/keyrings/vendor.gpg] https://packages.example/app stable main extras\n";
    const auto listCandidates = pacsmith::AptSourcesParser::parse(
        QByteArrayView(oneLine), QStringLiteral("etc/apt/sources.list.d/vendor.list"));
    QCOMPARE(listCandidates.size(), 1);
    QCOMPARE(listCandidates.first().uri, QStringLiteral("https://packages.example/app"));
    QCOMPARE(listCandidates.first().suite, QStringLiteral("stable"));
    QCOMPARE(listCandidates.first().components, QStringList({QStringLiteral("main"), QStringLiteral("extras")}));
    QCOMPARE(listCandidates.first().architectures, QStringList({QStringLiteral("amd64")}));
    QCOMPARE(listCandidates.first().signedBy, QStringLiteral("/usr/share/keyrings/vendor.gpg"));

    const QByteArray deb822 =
        "Types: deb\nURIs: https://repo.example/vendor\nSuites: production\n"
        "Components: main\nArchitectures: amd64 arm64\nSigned-By: /etc/apt/keyrings/vendor.gpg\n";
    const auto sourceCandidates = pacsmith::AptSourcesParser::parse(
        QByteArrayView(deb822), QStringLiteral("etc/apt/sources.list.d/vendor.sources"));
    QCOMPARE(sourceCandidates.size(), 1);
    QCOMPARE(sourceCandidates.first().suite, QStringLiteral("production"));
    QCOMPARE(sourceCandidates.first().architectures.size(), 2);
}

void CoreTests::comparesDebianVersions() {
    QVERIFY(pacsmith::DebianVersion::compare(QStringLiteral("1.0~rc1"), QStringLiteral("1.0")) < 0);
    QVERIFY(pacsmith::DebianVersion::compare(QStringLiteral("1:1.0"), QStringLiteral("2.0")) > 0);
    QVERIFY(pacsmith::DebianVersion::compare(QStringLiteral("1.0-2"), QStringLiteral("1.0-1")) > 0);
    QCOMPARE(pacsmith::DebianVersion::compare(QStringLiteral("1.0"), QStringLiteral("1.0-0")), 0);
    QVERIFY(pacsmith::DebianVersion::compare(QStringLiteral("1.0+git1"), QStringLiteral("1.0")) > 0);
    QVERIFY(pacsmith::DebianVersion::compare(QStringLiteral("1.10"), QStringLiteral("1.9")) > 0);
}

void CoreTests::parsesAptRepositoryMetadata() {
    const auto indexHash = QString(64, QLatin1Char('a'));
    const QByteArray release = QStringLiteral(
        "Origin: Vendor\nSHA256:\n %1 123 main/binary-amd64/Packages.xz\n %2 45 main/binary-arm64/Packages.gz\n")
                                   .arg(indexHash, QString(64, QLatin1Char('b')))
                                   .toUtf8();
    QString error;
    const auto entries = pacsmith::AptRepositoryMetadata::parseRelease(QByteArrayView(release), &error);
    QCOMPARE(entries.size(), 2);
    const auto index = pacsmith::AptRepositoryMetadata::selectPackagesIndex(
        entries, QStringLiteral("main"), QStringLiteral("amd64"), false);
    QVERIFY(index.has_value());
    QCOMPARE(index->path, QStringLiteral("main/binary-amd64/Packages.xz"));
    QCOMPARE(index->sha256, indexHash);

    const auto packageHash = QString(64, QLatin1Char('c'));
    const QByteArray packages = QStringLiteral(
        "Package: vendor-app\nVersion: 1.9-1\nArchitecture: amd64\nFilename: pool/v/vendor-app_1.9_amd64.deb\nSize: 40\nSHA256: %1\n\n"
        "Package: vendor-app\nVersion: 2.0~rc1-1\nArchitecture: amd64\nFilename: pool/v/vendor-app_2.0rc1_amd64.deb\nSize: 42\nSHA256: %1\n\n"
        "Package: other\nVersion: 9\nArchitecture: amd64\nFilename: pool/o/other.deb\nSize: 1\nSHA256: %1\n")
                                    .arg(packageHash)
                                    .toUtf8();
    const auto latest = pacsmith::AptRepositoryMetadata::latestPackage(
        QByteArrayView(packages), QStringLiteral("vendor-app"), QStringLiteral("amd64"), &error);
    QVERIFY2(latest.has_value(), qPrintable(error));
    QCOMPARE(latest->version, QStringLiteral("2.0~rc1-1"));
    QCOMPARE(latest->sha256, packageHash);

    const auto raw = pacsmith::AptRepositoryMetadata::decompressIndex(packages, &error);
    QVERIFY2(raw.has_value(), qPrintable(error));
    QCOMPARE(*raw, packages);

    const QByteArray unsafePackage = QStringLiteral(
        "Package: vendor-app\nVersion: 99\nArchitecture: amd64\nFilename: https://evil.example/package.deb\n"
        "Size: 1\nSHA256: %1\n")
                                         .arg(packageHash)
                                         .toUtf8();
    QVERIFY(!pacsmith::AptRepositoryMetadata::latestPackage(
        QByteArrayView(unsafePackage), QStringLiteral("vendor-app"), QStringLiteral("amd64"), &error));
}

void CoreTests::verifiesPinnedAptRepositorySignatures() {
    const auto gpg = QStandardPaths::findExecutable(QStringLiteral("gpg"));
    const auto gpgv = QStandardPaths::findExecutable(QStringLiteral("gpgv"));
    if (gpg.isEmpty() || gpgv.isEmpty()) QSKIP("gpg and gpgv are required");
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto keyHome = temporary.path() + QStringLiteral("/gnupg");
    QVERIFY(QDir{}.mkpath(keyHome));
    QVERIFY(QFile::setPermissions(keyHome, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                           QFileDevice::ExeOwner));
    const auto identity = QStringLiteral("PacSmith APT Test <apt-test@example.invalid>");
    auto runGpg = [&](const QStringList &arguments) {
        QProcess process;
        process.setProgram(gpg);
        process.setArguments(arguments);
        process.start();
        if (!process.waitForStarted(5000) || !process.waitForFinished(20000) ||
            process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
            qWarning().noquote() << process.readAllStandardError();
            return false;
        }
        return true;
    };
    if (!runGpg({QStringLiteral("--batch"), QStringLiteral("--quiet"),
                 QStringLiteral("--homedir"), keyHome, QStringLiteral("--pinentry-mode"),
                 QStringLiteral("loopback"), QStringLiteral("--passphrase"), QString{},
                 QStringLiteral("--quick-generate-key"), identity, QStringLiteral("ed25519"),
                 QStringLiteral("sign"), QStringLiteral("1d")})) {
        QSKIP("gpg-agent is unavailable in this test environment");
    }

    const auto keyring = temporary.path() + QStringLiteral("/vendor.gpg");
    QVERIFY(runGpg({QStringLiteral("--batch"), QStringLiteral("--homedir"), keyHome,
                    QStringLiteral("--output"), keyring, QStringLiteral("--export"), identity}));
    QString error;
    const auto fingerprints = pacsmith::RepositoryTrust::fingerprints(
        std::filesystem::path(keyring.toUtf8().constData()), &error);
    QVERIFY2(!fingerprints.isEmpty(), qPrintable(error));
    QFile exportedKey(keyring);
    QVERIFY(exportedKey.open(QIODevice::ReadOnly));
    const auto inspection = pacsmith::RepositoryTrust::inspectKey(exportedKey.readAll(), &error);
    QVERIFY2(inspection.has_value(), qPrintable(error));
    QCOMPARE(inspection->fingerprints, fingerprints);
    QCOMPARE(inspection->sha256.size(), 64);

    const auto releasePath = temporary.path() + QStringLiteral("/Release");
    QFile releaseFile(releasePath);
    QVERIFY(releaseFile.open(QIODevice::WriteOnly));
    const QByteArray release =
        "Origin: PacSmith Test\n"
        "SHA256:\n"
        " aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa 12 main/binary-amd64/Packages\n";
    QCOMPARE(releaseFile.write(release), release.size());
    releaseFile.close();
    const auto inReleasePath = temporary.path() + QStringLiteral("/InRelease");
    QVERIFY(runGpg({QStringLiteral("--batch"), QStringLiteral("--yes"),
                    QStringLiteral("--homedir"), keyHome, QStringLiteral("--pinentry-mode"),
                    QStringLiteral("loopback"), QStringLiteral("--passphrase"), QString{},
                    QStringLiteral("--output"), inReleasePath, QStringLiteral("--clearsign"),
                    releasePath}));
    QFile inReleaseFile(inReleasePath);
    QVERIFY(inReleaseFile.open(QIODevice::ReadOnly));
    auto inRelease = inReleaseFile.readAll();
    QVERIFY2(pacsmith::AptSignatureVerifier::verifyInRelease(
                 inRelease, keyring, fingerprints, error), qPrintable(error));
    QVERIFY(!pacsmith::AptSignatureVerifier::verifyInRelease(
        inRelease, keyring, {QString(40, QLatin1Char('0'))}, error));

    const auto signaturePath = temporary.path() + QStringLiteral("/Release.gpg");
    QVERIFY(runGpg({QStringLiteral("--batch"), QStringLiteral("--yes"),
                    QStringLiteral("--homedir"), keyHome, QStringLiteral("--pinentry-mode"),
                    QStringLiteral("loopback"), QStringLiteral("--passphrase"), QString{},
                    QStringLiteral("--output"), signaturePath, QStringLiteral("--detach-sign"),
                    releasePath}));
    QFile signatureFile(signaturePath);
    QVERIFY(signatureFile.open(QIODevice::ReadOnly));
    QVERIFY2(pacsmith::AptSignatureVerifier::verifyDetachedRelease(
                 release, signatureFile.readAll(), keyring, fingerprints, error), qPrintable(error));
    const auto payloadIndex = inRelease.indexOf("Origin: PacSmith Test");
    QVERIFY(payloadIndex >= 0);
    inRelease[payloadIndex] = 'X';
    QVERIFY(!pacsmith::AptSignatureVerifier::verifyInRelease(
        inRelease, keyring, fingerprints, error));
}

void CoreTests::validatesRepositorySigningKeyUrls() {
    QVERIFY(pacsmith::isAcceptableRepositoryKeyUrl(
        QUrl(QStringLiteral("https://downloads.typora.io/typora.gpg"))));
    QVERIFY(pacsmith::isAcceptableRepositoryKeyUrl(
        QUrl(QStringLiteral("https://vendor.example/keys/repository.asc?version=2"))));
    QVERIFY(!pacsmith::isAcceptableRepositoryKeyUrl(
        QUrl(QStringLiteral("http://downloads.typora.io/typora.gpg"))));
    QVERIFY(!pacsmith::isAcceptableRepositoryKeyUrl(
        QUrl(QStringLiteral("file:///tmp/vendor.gpg"))));
    QVERIFY(!pacsmith::isAcceptableRepositoryKeyUrl(
        QUrl(QStringLiteral("https://user:password@vendor.example/key.gpg"))));
    QVERIFY(!pacsmith::isAcceptableRepositoryKeyUrl(
        QUrl(QStringLiteral("https://vendor.example/key.gpg#fingerprint"))));
    QVERIFY(!pacsmith::isAcceptableRepositoryKeyUrl(QUrl(QStringLiteral("https:///key.gpg"))));
}

void CoreTests::tracksContentSpecificPayloadDecisions() {
    pacsmith::PackageRelease project;
    pacsmith::PayloadEntry profile;
    profile.path = QStringLiteral("etc/apparmor.d/vendor-app");
    profile.type = QStringLiteral("file");
    profile.size = 42;
    profile.requiresReview = true;
    profile.reviewReason = QStringLiteral("AppArmor policy");
    profile.contentSha256 = QString(64, QLatin1Char('a'));
    project.payload.append(profile);

    QVERIFY(pacsmith::PayloadReview::state(project, project.payload.first()).needsReview);
    pacsmith::PayloadReview::decide(project, profile.path, false);
    const auto kept = pacsmith::PayloadReview::state(project, project.payload.first());
    QVERIFY(!kept.needsReview);
    QCOMPARE(kept.disposition, pacsmith::PayloadDisposition::Included);

    project.payload.first().contentSha256 = QString(64, QLatin1Char('b'));
    QVERIFY(pacsmith::PayloadReview::state(project, project.payload.first()).needsReview);
    pacsmith::PayloadReview::decide(project, profile.path, true);
    const auto excluded = pacsmith::PayloadReview::state(project, project.payload.first());
    QVERIFY(!excluded.needsReview);
    QCOMPARE(excluded.disposition, pacsmith::PayloadDisposition::Excluded);

    project.archPackageName = QStringLiteral("vendor-app-bin");
    project.originalSourceFilename = QStringLiteral("vendor.deb");
    project.sourceSha256 = QString(64, QLatin1Char('c'));
    project.debian.version = QStringLiteral("1.0");
    project.debian.architecture = QStringLiteral("amd64");
    const auto pkgbuild = pacsmith::PkgbuildGenerator::generate(project);
    QVERIFY(pkgbuild.contains(QStringLiteral("rm -rf -- \"${pkgdir}/etc/apparmor.d/vendor-app\"")));

    pacsmith::PackageRelease aptProject;
    auto aptSource = profile;
    aptSource.path = QStringLiteral("etc/apt/sources.list.d/vendor.sources");
    aptProject.payload.append(aptSource);
    aptProject.payloadRules.append({QStringLiteral("etc/apt"), true,
                                    QStringLiteral("Excluded by safety default"), false, {}});
    const auto defaultState = pacsmith::PayloadReview::state(aptProject, aptProject.payload.first());
    QCOMPARE(defaultState.disposition, pacsmith::PayloadDisposition::ExcludedByDefault);
    QCOMPARE(defaultState.decisionPath, QStringLiteral("etc/apt"));
    pacsmith::PayloadReview::decide(aptProject, defaultState.decisionPath, false);
    const auto includedApt = pacsmith::PayloadReview::state(aptProject, aptProject.payload.first());
    QVERIFY(!includedApt.needsReview);
    QCOMPARE(includedApt.disposition, pacsmith::PayloadDisposition::Included);
    QVERIFY(!aptProject.payloadRules.first().excluded);
}

void CoreTests::serializesProjectsAndOverrides() {
    pacsmith::PackageRelease project;
    project.id = QStringLiteral("vendor-app");
    project.displayName = QStringLiteral("Vendor App");
    project.iconPath = QStringLiteral("files/icon.png");
    project.iconSourcePath = QStringLiteral("usr/share/icons/hicolor/128x128/apps/vendor-app.png");
    project.iconSha256 = QString(64, QLatin1Char('f'));
    project.archPackageName = QStringLiteral("vendor-app-bin");
    project.sourceType = pacsmith::SourcePackageType::Archive;
    project.acquisition.kind = pacsmith::AcquisitionKind::GitHubRelease;
    project.acquisition.canonicalIdentity = QStringLiteral("github:vendor/vendor-app");
    project.acquisition.githubOwner = QStringLiteral("vendor");
    project.acquisition.githubRepository = QStringLiteral("vendor-app");
    project.acquisition.githubReleaseId = 42;
    project.acquisition.githubPrerelease = true;
    project.acquisition.githubAssetId = 84;
    project.installMapping.archiveLayout = pacsmith::ArchiveLayout::OptBundle;
    project.installMapping.optDirectory = QStringLiteral("vendor-app");
    project.installMapping.commonPrefix = QStringLiteral("vendor-app-1.0");
    project.installMapping.stripCommonPrefix = true;
    project.installMapping.binarySourcePath = QStringLiteral("bin/vendor-app");
    project.installMapping.binaryDestination = QStringLiteral("/usr/bin/vendor-app");
    pacsmith::LauncherMapping serializedLauncher;
    serializedLauncher.sourcePath = QStringLiteral("vendor-app-1.0/bin/vendor-app");
    serializedLauncher.commandName = QStringLiteral("vendor-app");
    serializedLauncher.destination = QStringLiteral("/usr/bin/vendor-app");
    serializedLauncher.sourceFingerprint = QStringLiteral("launcher-fingerprint");
    serializedLauncher.provenance.origin = pacsmith::ValueOrigin::User;
    project.installMapping.launchers.append(serializedLauncher);
    pacsmith::DesktopEntryConfiguration serializedDesktop;
    serializedDesktop.id = QStringLiteral("vendor-app");
    serializedDesktop.sourcePath = QStringLiteral("vendor-app-1.0/vendor-app.desktop");
    serializedDesktop.destination =
        QStringLiteral("/usr/share/applications/vendor-app.desktop");
    serializedDesktop.contents = QStringLiteral(
        "[Desktop Entry]\nType=Application\nName=Vendor App\nExec=vendor-app\n");
    serializedDesktop.sourceSha256 = QStringLiteral("desktop-source-sha");
    serializedDesktop.originalContentsSha256 = QStringLiteral("desktop-original-sha");
    serializedDesktop.userModified = true;
    serializedDesktop.provenance.origin = pacsmith::ValueOrigin::User;
    project.installMapping.desktopEntries.append(serializedDesktop);
    project.installMapping.appRun.present = true;
    project.installMapping.appRun.script = true;
    project.installMapping.appRun.contents =
        QStringLiteral("#!/bin/sh\nexec \"$APPDIR/vendor-app\" \"$@\"\n");
    project.installMapping.appRun.originalContents =
        QStringLiteral("#!/bin/bash\nBINARY_NAME=$(basename \"$0\")\n");
    project.installMapping.appRun.userModified = true;
    project.installMapping.appRun.reviewReason = QStringLiteral("filename dispatch");
    project.installMapping.appRun.provenance.origin = pacsmith::ValueOrigin::User;
    project.installMapping.icon.sourceKind = pacsmith::IconSourceKind::Payload;
    project.installMapping.icon.sourcePath =
        QStringLiteral("vendor-app-1.0/share/icons/vendor-app.png");
    project.installMapping.icon.projectPath = QStringLiteral("files/integration/icon.png");
    project.installMapping.icon.sha256 = QString(64, QLatin1Char('e'));
    project.installMapping.icon.format = QStringLiteral("png");
    project.installMapping.icon.dimensions = QSize(256, 256);
    project.installMapping.icon.iconName = QStringLiteral("vendor-app");
    project.debian.package = QStringLiteral("vendor-app");
    project.debian.version = QStringLiteral("1.0-1");
    auto dependencies = pacsmith::DependencyParser::parse(QStringLiteral("libfoo (>= 2)"));
    dependencies.first().archPackage = QStringLiteral("custom-libfoo");
    dependencies.first().status = pacsmith::MappingStatus::Resolved;
    dependencies.first().userOverride = true;
    dependencies.first().mappingSource = QStringLiteral("user override");
    project.dependencies = dependencies;
    project.maintainerScripts.append({QStringLiteral("postinst"), QStringLiteral("#!/bin/sh\ntrue\n"), {}});
    project.maintainerScripts.first().acknowledge();
    project.update.strategy = pacsmith::UpdateStrategy::AptRepository;
    project.update.url = QStringLiteral("https://repo.example/vendor");
    project.update.aptSuite = QStringLiteral("stable");
    project.update.aptComponent = QStringLiteral("main");
    project.update.aptArchitecture = QStringLiteral("amd64");
    project.update.aptPackageName = QStringLiteral("vendor-app");
    project.update.githubOwner = QStringLiteral("vendor");
    project.update.githubRepository = QStringLiteral("vendor-app");
    project.update.githubAssetRegex = QStringLiteral("vendor-app-.*-linux-x86_64\\.tar\\.gz");
    project.update.githubIncludePrereleases = true;
    project.update.aptCandidates.append({project.update.url, project.update.aptSuite,
                                         {project.update.aptComponent}, {project.update.aptArchitecture},
                                         QStringLiteral("/usr/share/keyrings/vendor.gpg"),
                                         QStringLiteral("etc/apt/sources.list.d/vendor.list")});
    project.update.trustedSigningFingerprint = QString(40, QLatin1Char('a'));
    project.update.signingKeys.append(
        {QStringLiteral("files/keys/vendor.gpg"), QString(64, QLatin1Char('b')),
         {project.update.trustedSigningFingerprint}, QStringLiteral("control/postinst:KEY"),
         QStringLiteral("source-fingerprint"), true,
         {pacsmith::ValueOrigin::Deterministic, {}, {}, QStringLiteral("source-fingerprint"),
          QStringLiteral("vendor key"), QDateTime::currentDateTimeUtc(), false}});
    project.scriptFindings.append(
        {QStringLiteral("postinst"), QStringLiteral("apt-repository"), QStringLiteral("handled"),
         QStringLiteral("evidence"), QStringLiteral("finding-sha"),
         pacsmith::ScriptDisposition::HandledByPacSmith,
         {pacsmith::ValueOrigin::Deterministic, {}, {}, QStringLiteral("finding-sha"),
          QStringLiteral("static"), QDateTime::currentDateTimeUtc(), false}});
    project.lifecycleScript.fileName = QStringLiteral("vendor-app-bin.install");
    project.lifecycleScript.contents = QStringLiteral("post_install() { /usr/bin/true; }\n");
    project.lifecycleScript.validationPassed = true;
    project.lifecycleScript.acknowledge();
    project.fieldProvenance.insert(
        QStringLiteral("update.url"),
        {pacsmith::ValueOrigin::Ai, QStringLiteral("openai"), QStringLiteral("test-model"),
         QStringLiteral("evidence"), QStringLiteral("reason"), QDateTime::currentDateTimeUtc(), false});
    project.aiChanges.append({QDateTime::currentDateTimeUtc(), QStringLiteral("update.url"), {},
                              project.update.url, QStringLiteral("openai"), QStringLiteral("test-model"),
                              QStringLiteral("reason")});
    project.history.append({QDateTime::currentDateTimeUtc(), QStringLiteral("created"), QStringLiteral("test")});
    project.buildStatus = pacsmith::BuildStatus::Canceled;

    const auto json = QJsonDocument(project.toJson()).toJson();
    const auto restored = pacsmith::PackageRelease::fromJson(QJsonDocument::fromJson(json).object());
    QCOMPARE(restored.id, project.id);
    QCOMPARE(restored.sourceType, pacsmith::SourcePackageType::Archive);
    QCOMPARE(restored.acquisition.kind, pacsmith::AcquisitionKind::GitHubRelease);
    QCOMPARE(restored.acquisition.canonicalIdentity, project.acquisition.canonicalIdentity);
    QCOMPARE(restored.acquisition.githubAssetId, 84);
    QVERIFY(restored.acquisition.githubPrerelease);
    QCOMPARE(restored.installMapping.optDirectory, QStringLiteral("vendor-app"));
    QCOMPARE(restored.installMapping.commonPrefix, QStringLiteral("vendor-app-1.0"));
    QVERIFY(restored.installMapping.stripCommonPrefix);
    QCOMPARE(restored.installMapping.binaryDestination, QStringLiteral("/usr/bin/vendor-app"));
    QCOMPARE(restored.installMapping.launchers.size(), 1);
    QCOMPARE(restored.installMapping.launchers.first().commandName,
             QStringLiteral("vendor-app"));
    QCOMPARE(restored.installMapping.desktopEntries.size(), 1);
    QVERIFY(restored.installMapping.desktopEntries.first().userModified);
    QVERIFY(restored.installMapping.appRun.present);
    QVERIFY(restored.installMapping.appRun.script);
    QVERIFY(restored.installMapping.appRun.userModified);
    QCOMPARE(restored.installMapping.appRun.contents,
             QStringLiteral("#!/bin/sh\nexec \"$APPDIR/vendor-app\" \"$@\"\n"));
    QCOMPARE(restored.installMapping.icon.sourceKind,
             pacsmith::IconSourceKind::Payload);
    QCOMPARE(restored.installMapping.icon.dimensions, QSize(256, 256));
    QCOMPARE(restored.iconPath, project.iconPath);
    QCOMPARE(restored.iconSourcePath, project.iconSourcePath);
    QCOMPARE(restored.iconSha256, project.iconSha256);
    QCOMPARE(restored.debian.version, project.debian.version);
    QCOMPARE(restored.dependencies.first().archPackage, QStringLiteral("custom-libfoo"));
    QVERIFY(restored.dependencies.first().userOverride);
    QVERIFY(!restored.maintainerScripts.first().requiresReview());
    QCOMPARE(restored.update.aptSuite, QStringLiteral("stable"));
    QCOMPARE(restored.update.githubAssetRegex, project.update.githubAssetRegex);
    QVERIFY(restored.update.githubIncludePrereleases);
    QCOMPARE(restored.update.aptCandidates.size(), 1);
    QCOMPARE(restored.update.signingKeys.size(), 1);
    QCOMPARE(restored.update.trustedSigningFingerprint, project.update.trustedSigningFingerprint);
    QCOMPARE(restored.scriptFindings.first().disposition,
             pacsmith::ScriptDisposition::HandledByPacSmith);
    QVERIFY(!restored.lifecycleScript.requiresAcknowledgement());
    QCOMPARE(restored.fieldProvenance.value(QStringLiteral("update.url")).origin,
             pacsmith::ValueOrigin::Ai);
    QCOMPARE(restored.aiChanges.size(), 1);
    QCOMPARE(restored.history.size(), 1);
    QCOMPARE(restored.buildStatus, pacsmith::BuildStatus::Canceled);
}

void CoreTests::persistsAiSettingsOutsideProjectData() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    pacsmith::AppSettingsStore store(temporary.path() + QStringLiteral("/pacsmith-config"));
    pacsmith::AiSettings settings;
    settings.provider = pacsmith::AiProviderKind::Xai;
    settings.model = QStringLiteral("grok-test");
    settings.reasoningEffort = pacsmith::AiReasoningEffort::High;
    settings.executionMode = pacsmith::AiExecutionMode::Fast;
    settings.automaticallyResolveReviewItems = true;
    settings.credentialSources.insert(QStringLiteral("xai"), pacsmith::CredentialSource::Age);
    QString error;
    QVERIFY2(store.save(settings, &error), qPrintable(error));
    const auto restored = store.load(&error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(restored.provider, pacsmith::AiProviderKind::Xai);
    QCOMPARE(restored.model, QStringLiteral("grok-test"));
    QCOMPARE(restored.reasoningEffort, pacsmith::AiReasoningEffort::High);
    QCOMPARE(restored.executionMode, pacsmith::AiExecutionMode::Fast);
    const auto requestOptions = pacsmith::aiRequestOptions(restored);
    QCOMPARE(requestOptions.value(QStringLiteral("reasoning")).toObject()
                 .value(QStringLiteral("effort")).toString(),
             QStringLiteral("high"));
    QCOMPARE(requestOptions.value(QStringLiteral("service_tier")).toString(),
             QStringLiteral("priority"));
    QCOMPARE(requestOptions.value(QStringLiteral("max_output_tokens")).toInt(), 16384);

    auto chatGptSettings = restored;
    chatGptSettings.provider = pacsmith::AiProviderKind::ChatGpt;
    const auto chatGptRequestOptions = pacsmith::aiRequestOptions(chatGptSettings);
    QVERIFY2(!chatGptRequestOptions.contains(QStringLiteral("max_output_tokens")),
             "ChatGPT's subscription transport rejects the public Responses API max_output_tokens field");
    QCOMPARE(chatGptRequestOptions.value(QStringLiteral("service_tier")).toString(),
             QStringLiteral("priority"));

    const auto chatGptInput = pacsmith::aiRequestInput(
        pacsmith::AiProviderKind::ChatGpt, QStringLiteral("package evidence"));
    QVERIFY(chatGptInput.isArray());
    const auto messages = chatGptInput.toArray();
    QCOMPARE(messages.size(), 1);
    const auto message = messages.first().toObject();
    QCOMPARE(message.value(QStringLiteral("role")).toString(), QStringLiteral("user"));
    const auto content = message.value(QStringLiteral("content")).toArray();
    QCOMPARE(content.size(), 1);
    QCOMPARE(content.first().toObject().value(QStringLiteral("type")).toString(),
             QStringLiteral("input_text"));
    QCOMPARE(content.first().toObject().value(QStringLiteral("text")).toString(),
             QStringLiteral("package evidence"));
    QCOMPARE(pacsmith::aiRequestInput(pacsmith::AiProviderKind::OpenAi,
                                     QStringLiteral("package evidence")).toString(),
             QStringLiteral("package evidence"));

    QVERIFY(restored.automaticallyResolveReviewItems);
    QCOMPARE(restored.credentialSources.value(QStringLiteral("xai")),
             pacsmith::CredentialSource::Age);
    QVERIFY(store.ageSecretsPath().startsWith(temporary.path()));
    QCOMPARE(pacsmith::aiProviderFromName(QStringLiteral("codex")),
             pacsmith::AiProviderKind::None);
    QCOMPARE(pacsmith::aiProviderFromName(QStringLiteral("chatgpt")),
             pacsmith::AiProviderKind::ChatGpt);
}

void CoreTests::persistsBackgroundUpdateSettings() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    pacsmith::AppSettingsStore store(temporary.path());
    pacsmith::AiSettings settings;
    settings.updates.enabled = true;
    settings.updates.daily = false;
    settings.updates.weekDay = 5;
    settings.updates.localTime = QTime(4, 25);
    settings.updates.automaticallyPrepare = true;
    settings.updates.retainedPackageVersions = 5;
    settings.updates.retainedCompleteReleases = 3;
    settings.updates.trayMode = pacsmith::TrayMode::ActivityOrUpdates;
    settings.debAssociationPrompted = true;
    settings.selfTrackingPrompted = true;
    QString error;
    QVERIFY2(store.save(settings, &error), qPrintable(error));
    const auto restored = store.load(&error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY(restored.updates.enabled);
    QVERIFY(!restored.updates.daily);
    QCOMPARE(restored.updates.weekDay, 5);
    QCOMPARE(restored.updates.localTime, QTime(4, 25));
    QVERIFY(restored.updates.automaticallyPrepare);
    QCOMPARE(restored.updates.retainedPackageVersions, 5);
    QCOMPARE(restored.updates.retainedCompleteReleases, 5);
    QCOMPARE(restored.updates.trayMode, pacsmith::TrayMode::ActivityOrUpdates);
    QVERIFY(restored.debAssociationPrompted);
    QVERIFY(restored.selfTrackingPrompted);
}

void CoreTests::buildsSystemdCalendarSchedules() {
    pacsmith::BackgroundUpdateSettings settings;
    settings.daily = true;
    settings.localTime = QTime(2, 0);
    QCOMPARE(pacsmith::BackgroundUpdateManager::calendar(settings),
             QStringLiteral("*-*-* 02:00:00"));
    settings.daily = false;
    settings.weekDay = 7;
    settings.localTime = QTime(23, 45);
    QCOMPARE(pacsmith::BackgroundUpdateManager::calendar(settings),
             QStringLiteral("Sun *-*-* 23:45:00"));
}

void CoreTests::persistsMultipleReleases() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    pacsmith::ProjectStore store(
        std::filesystem::path(temporary.path().toUtf8().constData()) / "projects");
    pacsmith::Project project;
    project.id = QStringLiteral("timeline");
    project.displayName = QStringLiteral("Timeline");
    project.archPackageName = QStringLiteral("pacsmith-test-timeline-not-installed");
    for (const auto &version : {QStringLiteral("1.0"), QStringLiteral("2.0")}) {
        pacsmith::PackageRelease release;
        release.id = version + QStringLiteral("-aaaaaaaaaaaa");
        release.projectId = project.id;
        release.archPackageName = project.archPackageName;
        release.debian.package = QStringLiteral("timeline");
        release.debian.version = version;
        release.sourceSha256 = QString(64, version == QStringLiteral("1.0") ? QLatin1Char('a') : QLatin1Char('b'));
        project.releases.append(release);
    }
    QString error;
    QVERIFY2(store.save(project, &error), qPrintable(error));
    const auto restored = store.load(project.id, &error);
    QVERIFY2(restored.has_value(), qPrintable(error));
    QCOMPARE(restored->releases.size(), 2);
    QCOMPARE(restored->newestRelease()->debian.version, QStringLiteral("2.0"));
    QVERIFY(QFileInfo::exists(QString::fromUtf8(
        (store.releasePath(project.id, project.releases.first().id) / "release.json").string().c_str())));
}

void CoreTests::selectsActiveTrackingRelease() {
    pacsmith::Project project;
    project.id = QStringLiteral("tracking");
    for (const auto &version : {QStringLiteral("1.0"), QStringLiteral("2.0")}) {
        pacsmith::PackageRelease release;
        release.id = version;
        release.projectId = project.id;
        release.debian.version = version;
        release.state = pacsmith::ReleaseState::Ready;
        project.releases.append(release);
    }
    pacsmith::PackageRelease discovered;
    discovered.id = QStringLiteral("3.0");
    discovered.projectId = project.id;
    discovered.debian.version = QStringLiteral("3.0");
    discovered.state = pacsmith::ReleaseState::Discovered;
    project.releases.append(discovered);

    QVERIFY(project.activeTrackingRelease() != nullptr);
    QCOMPARE(project.activeTrackingRelease()->id, QStringLiteral("2.0"));

    project.installedReleaseId = QStringLiteral("1.0");
    project.installedVersion = QStringLiteral("1.0-1");
    QCOMPARE(project.activeTrackingRelease()->id, QStringLiteral("1.0"));

    project.installedReleaseId.clear();
    project.externallyInstalled = true;
    QVERIFY(project.activeTrackingRelease() == nullptr);

    project.externallyInstalled = false;
    project.installedVersion.clear();
    project.releases[1].state = pacsmith::ReleaseState::Preparing;
    QCOMPARE(project.activeTrackingRelease()->id, QStringLiteral("1.0"));
}

void CoreTests::reportsInstalledUpdateStatus() {
    pacsmith::Project project;
    for (const auto &version : {QStringLiteral("1.0"), QStringLiteral("2.0")}) {
        pacsmith::PackageRelease release;
        release.id = version;
        release.debian.version = version;
        release.state = pacsmith::ReleaseState::Ready;
        project.releases.append(release);
    }

    QVERIFY(!project.hasAvailableUpdate());
    project.installedReleaseId = QStringLiteral("1.0");
    project.installedVersion = QStringLiteral("1.0-1");
    QVERIFY(project.hasAvailableUpdate());

    project.installedReleaseId = QStringLiteral("2.0");
    project.installedVersion = QStringLiteral("2.0-1");
    QVERIFY(!project.hasAvailableUpdate());
    project.releases[1].update.detectedVersion = QStringLiteral("3.0");
    QVERIFY(project.hasAvailableUpdate());

    project.installedReleaseId.clear();
    project.externallyInstalled = true;
    QVERIFY(!project.hasAvailableUpdate());
}

void CoreTests::recordsUninspectedGitHubDiscoveries() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    pacsmith::ProjectStore store(
        std::filesystem::path(temporary.path().toUtf8().constData()) / "projects");
    pacsmith::Project project;
    project.id = QStringLiteral("discovery");
    project.displayName = QStringLiteral("Discovery");
    project.archPackageName = QStringLiteral("discovery-bin");
    pacsmith::PackageRelease tracker;
    tracker.id = QStringLiteral("1.0-aaaaaaaaaaaa");
    tracker.projectId = project.id;
    tracker.archPackageName = project.archPackageName;
    tracker.sourceType = pacsmith::SourcePackageType::Debian;
    tracker.debian.package = QStringLiteral("discovery");
    tracker.debian.version = QStringLiteral("1.0");
    tracker.debian.architecture = QStringLiteral("amd64");
    tracker.update.strategy = pacsmith::UpdateStrategy::GitHubRelease;
    tracker.update.githubOwner = QStringLiteral("vendor");
    tracker.update.githubRepository = QStringLiteral("discovery");
    project.releases.append(tracker);
    QString error;
    QVERIFY2(store.save(project, &error), qPrintable(error));
    auto *discovered = store.recordDiscoveredRelease(
        project, tracker, QStringLiteral("2.0"), QStringLiteral("discovery-2.0.tar.gz"), {},
        QStringLiteral("https://github.com/vendor/discovery/releases/download/v2.0/discovery-2.0.tar.gz"),
        &error, 200, 201, QStringLiteral("v2.0"), {});
    QVERIFY2(discovered != nullptr, qPrintable(error));
    QCOMPARE(discovered->sourceType, pacsmith::SourcePackageType::Unknown);
    QCOMPARE(discovered->state, pacsmith::ReleaseState::Discovered);
    QCOMPARE(discovered->acquisition.kind, pacsmith::AcquisitionKind::GitHubRelease);
    QCOMPARE(discovered->update.strategy, pacsmith::UpdateStrategy::GitHubRelease);
    QCOMPARE(discovered->update.githubOwner, QStringLiteral("vendor"));
    QCOMPARE(discovered->update.githubRepository, QStringLiteral("discovery"));
    QVERIFY(discovered->history.last().detail.contains(QStringLiteral("without a publisher digest")));

    // A successor owns an independent update snapshot. Editing it later must not
    // rewrite the release from which it was discovered.
    discovered->update.githubAssetRegex = QStringLiteral("discovery-.*-x86_64\\.tar\\.gz");
    QCOMPARE(project.release(tracker.id)->update.githubAssetRegex, QString{});
    QVERIFY2(store.save(project, &error), qPrintable(error));

    const auto restored = store.load(project.id, &error);
    QVERIFY2(restored.has_value(), qPrintable(error));
    const auto *restoredDiscovery = restored->release(discovered->id);
    QVERIFY(restoredDiscovery != nullptr);
    QCOMPARE(restoredDiscovery->sourceType, pacsmith::SourcePackageType::Unknown);
    QCOMPARE(restoredDiscovery->update.githubAssetRegex,
             QStringLiteral("discovery-.*-x86_64\\.tar\\.gz"));
    QCOMPARE(restored->release(tracker.id)->update.githubAssetRegex, QString{});
}

void CoreTests::carriesInstallMappingAcrossGitHubVersions() {
    const auto executable = QStandardPaths::findExecutable(QStringLiteral("true"));
    QVERIFY(!executable.isEmpty());
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto firstPath = temporary.filePath(
        QStringLiteral("vendorctl-1.0.0-linux-x86_64"));
    const auto secondPath = temporary.filePath(
        QStringLiteral("vendorctl-2.0.0-linux-x86_64"));
    QVERIFY(QFile::copy(executable, firstPath));
    QVERIFY(QFile::copy(executable, secondPath));
    QFile second(secondPath);
    QVERIFY(second.open(QIODevice::Append));
    QCOMPARE(second.write("\0", 1), 1);
    second.close();

    pacsmith::ProjectStore store(
        std::filesystem::path(temporary.path().toUtf8().constData()) / "projects");
    pacsmith::ImportOptions firstOptions;
    firstOptions.version = QStringLiteral("1.0.0");
    firstOptions.githubAssetRegex = QStringLiteral("vendorctl-.*-linux-x86_64");
    firstOptions.acquisition.kind = pacsmith::AcquisitionKind::GitHubRelease;
    firstOptions.acquisition.canonicalIdentity = QStringLiteral("github:vendor/vendorctl");
    firstOptions.acquisition.originalUrl = QStringLiteral("https://example.invalid/v1");
    firstOptions.acquisition.githubOwner = QStringLiteral("vendor");
    firstOptions.acquisition.githubRepository = QStringLiteral("vendorctl");
    firstOptions.acquisition.githubReleaseId = 100;
    firstOptions.acquisition.githubAssetId = 101;
    QString error;
    auto first = store.importSource(
        std::filesystem::path(firstPath.toUtf8().constData()), firstOptions, &error);
    QVERIFY2(first.has_value(), qPrintable(error));
    auto *tracker = first->project.release(first->releaseId);
    QVERIFY(tracker != nullptr);
    tracker->installMapping.binaryDestination = QStringLiteral("/usr/bin/vendorctl-custom");
    tracker->lifecycleScript.fileName = QStringLiteral("vendorctl-bin.install");
    tracker->lifecycleScript.contents = QStringLiteral("post_install() { /usr/bin/true; }\n");
    QVERIFY2(store.saveLifecycle(first->project, *tracker, &error), qPrintable(error));
    tracker->lifecycleScript.acknowledge();
    QVERIFY2(store.save(first->project, &error), qPrintable(error));

    const auto *discovered = store.recordDiscoveredRelease(
        first->project, *tracker, QStringLiteral("2.0.0"),
        QStringLiteral("vendorctl-2.0.0-linux-x86_64"), {},
        QStringLiteral("https://example.invalid/v2"), &error, 200, 201,
        QStringLiteral("v2.0.0"), {});
    QVERIFY2(discovered != nullptr, qPrintable(error));

    pacsmith::ImportOptions secondOptions = firstOptions;
    secondOptions.version = QStringLiteral("2.0.0");
    secondOptions.acquisition.originalUrl = QStringLiteral("https://example.invalid/v2");
    secondOptions.acquisition.githubReleaseId = 200;
    secondOptions.acquisition.githubAssetId = 201;
    auto imported = store.importSource(
        std::filesystem::path(secondPath.toUtf8().constData()), secondOptions, &error);
    QVERIFY2(imported.has_value(), qPrintable(error));
    QCOMPARE(imported->project.releases.size(), 2);
    const auto *updated = imported->project.release(imported->releaseId);
    QVERIFY(updated != nullptr);
    QCOMPARE(updated->sourceType, pacsmith::SourcePackageType::ElfBinary);
    QCOMPARE(updated->installMapping.binaryDestination,
             QStringLiteral("/usr/bin/vendorctl-custom"));
    QCOMPARE(updated->lifecycleScript.contents,
             QStringLiteral("post_install() { /usr/bin/true; }\n"));
    QVERIFY(updated->generatedPkgbuild.contains(
        QStringLiteral("install='vendorctl-bin.install'")));
    QFile lifecycle(QString::fromUtf8(store.lifecyclePath(*updated).string().c_str()));
    QVERIFY(lifecycle.open(QIODevice::ReadOnly));
    QCOMPARE(QString::fromUtf8(lifecycle.readAll()), updated->lifecycleScript.contents);
}

void CoreTests::preservesRepositoryFirstImportConfiguration() {
    const auto executable = QStandardPaths::findExecutable(QStringLiteral("true"));
    QVERIFY(!executable.isEmpty());
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto source = temporary.filePath(QStringLiteral("vendor-tool"));
    QVERIFY(QFile::copy(executable, source));

    pacsmith::ProjectStore store(
        std::filesystem::path(temporary.path().toUtf8().constData()) / "projects");
    pacsmith::ImportOptions options;
    options.packageName = QStringLiteral("vendor-tool");
    options.version = QStringLiteral("2.4.1-3");
    options.architecture = QStringLiteral("x86_64");
    options.acquisition.kind = pacsmith::AcquisitionKind::RpmRepository;
    options.acquisition.canonicalIdentity =
        QStringLiteral("rpm:https://packages.vendor.invalid/stable:x86_64:vendor-tool");
    options.acquisition.originalUrl =
        QStringLiteral("https://packages.vendor.invalid/stable/vendor-tool.rpm");
    pacsmith::UpdateConfiguration update;
    update.strategy = pacsmith::UpdateStrategy::RpmRepository;
    update.url = QStringLiteral("https://packages.vendor.invalid/stable");
    update.rpmArchitecture = QStringLiteral("x86_64");
    update.rpmPackageName = QStringLiteral("vendor-tool");
    update.detectedVersion = options.version;
    update.detectedFilename = QStringLiteral("vendor-tool.rpm");
    update.detectedSha256 = QString(64, QLatin1Char('a'));
    update.detectedUrl = options.acquisition.originalUrl;
    update.signatureVerified = true;
    options.initialUpdate = update;

    QString error;
    const auto imported = store.importSource(
        std::filesystem::path(source.toUtf8().constData()), options, &error);
    QVERIFY2(imported.has_value(), qPrintable(error));
    QCOMPARE(imported->project.sourceIdentity,
             options.acquisition.canonicalIdentity);
    const auto *release = imported->project.release(imported->releaseId);
    QVERIFY(release != nullptr);
    QCOMPARE(release->acquisition.kind, pacsmith::AcquisitionKind::RpmRepository);
    QCOMPARE(release->update.strategy, pacsmith::UpdateStrategy::RpmRepository);
    QCOMPARE(release->update.url, update.url);
    QCOMPARE(release->update.rpmPackageName, update.rpmPackageName);
    QCOMPARE(release->update.detectedSha256, update.detectedSha256);
    QVERIFY(release->update.signatureVerified);

    const auto restored = store.load(imported->project.id, &error);
    QVERIFY2(restored.has_value(), qPrintable(error));
    const auto *restoredRelease = restored->release(imported->releaseId);
    QVERIFY(restoredRelease != nullptr);
    QCOMPARE(restoredRelease->update.strategy,
             pacsmith::UpdateStrategy::RpmRepository);
    QCOMPARE(restoredRelease->update.detectedUrl, update.detectedUrl);
}

void CoreTests::parsesAiModelCatalog() {
    QString error;
    const auto models = pacsmith::AiModelCatalogService::parseModelIds(
        QByteArrayLiteral(R"({"object":"list","data":[{"id":"gpt-z"},{"id":"gpt-a"},{"id":"gpt-a"}]})"),
        &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(models, QStringList({QStringLiteral("gpt-a"), QStringLiteral("gpt-z")}));

    const auto invalid = pacsmith::AiModelCatalogService::parseModelIds(
        QByteArrayLiteral(R"({"object":"list"})"), &error);
    QVERIFY(invalid.isEmpty());
    QVERIFY(!error.isEmpty());
}

void CoreTests::parsesChatGptCredentialsAndCatalog() {
    const QJsonObject claims{
        {QStringLiteral("https://api.openai.com/profile"),
         QJsonObject{{QStringLiteral("email"), QStringLiteral("user@example.com")}}},
        {QStringLiteral("https://api.openai.com/auth"),
         QJsonObject{{QStringLiteral("chatgpt_account_id"), QStringLiteral("acct-test")},
                     {QStringLiteral("chatgpt_plan_type"), QStringLiteral("plus")}}}};
    const auto payload = QJsonDocument(claims).toJson(QJsonDocument::Compact).toBase64(
        QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
    const auto token = QByteArrayLiteral("header.") + payload + QByteArrayLiteral(".signature");
    const QJsonObject response{{QStringLiteral("access_token"), QString::fromLatin1(token)},
                               {QStringLiteral("refresh_token"), QStringLiteral("refresh-test")},
                               {QStringLiteral("expires_in"), 3600}};
    QString error;
    const auto credentials = pacsmith::parseChatGptTokenResponse(
        QJsonDocument(response).toJson(QJsonDocument::Compact), {}, &error);
    QVERIFY2(credentials.has_value(), qPrintable(error));
    QCOMPARE(credentials->accountId, QStringLiteral("acct-test"));
    QCOMPARE(credentials->email, QStringLiteral("user@example.com"));
    QCOMPARE(credentials->planType, QStringLiteral("plus"));
    QVERIFY(credentials->expiresAtMs > QDateTime::currentMSecsSinceEpoch());
    const auto restored = pacsmith::ChatGptCredentials::fromSerialized(credentials->serialize(), &error);
    QVERIFY2(restored.has_value(), qPrintable(error));
    QCOMPARE(restored->refreshToken, QStringLiteral("refresh-test"));

    const auto models = pacsmith::AiModelCatalogService::parseChatGptModelIds(
        QByteArrayLiteral(
            R"({"models":[{"slug":"gpt-visible","visibility":"list"},{"slug":"gpt-hidden","visibility":"hide"},{"slug":"gpt-disabled","show_in_picker":false},{"id":"gpt-fallback"}]})"),
        &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(models, QStringList({QStringLiteral("gpt-visible"), QStringLiteral("gpt-fallback")}));
}

void CoreTests::buildsExternalTerminalCommandsSafely() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QStringList terminalNames{
        QStringLiteral("xdg-terminal-exec"), QStringLiteral("konsole"),
        QStringLiteral("gnome-terminal"), QStringLiteral("xfce4-terminal"),
        QStringLiteral("mate-terminal"), QStringLiteral("kitty"),
        QStringLiteral("alacritty"), QStringLiteral("foot"), QStringLiteral("xterm")};
    const auto packageArgument = QStringLiteral("/tmp/vendor package;$(touch should-not-run).pkg.tar.zst");
    const QStringList helperArguments{QStringLiteral("_install-session"),
                                      QStringLiteral("--package"), packageArgument};
    for (const auto &terminalName : terminalNames) {
        const auto terminalPath = temporary.filePath(terminalName);
        QFile terminal(terminalPath);
        QVERIFY(terminal.open(QIODevice::WriteOnly));
        QCOMPARE(terminal.write("#!/bin/sh\n"), 10);
        terminal.close();
        QVERIFY(QFile::setPermissions(terminalPath, QFileDevice::ReadOwner |
                                                       QFileDevice::WriteOwner |
                                                       QFileDevice::ExeOwner));
        QString error;
        const auto command = pacsmith::TerminalLauncher::commandFor(
            terminalPath, {}, QStringLiteral("/usr/bin/true"), helperArguments, &error);
        QVERIFY2(command.has_value(), qPrintable(error));
        QCOMPARE(command->program, terminalPath);
        QVERIFY(command->arguments.size() >= helperArguments.size() + 1);
        const auto helperIndex = command->arguments.size() - helperArguments.size() - 1;
        QCOMPARE(command->arguments.at(helperIndex), QStringLiteral("/usr/bin/true"));
        QCOMPARE(command->arguments.sliced(helperIndex + 1), helperArguments);
        QVERIFY(!command->arguments.contains(QStringLiteral("sh")));
        QVERIFY(!command->arguments.contains(QStringLiteral("-c")));
        QCOMPARE(command->arguments.last(), packageArgument);
    }

    const auto kittyPath = temporary.filePath(QStringLiteral("kitty"));
    QProcessEnvironment environment;
    environment.insert(QStringLiteral("TERMINAL"), kittyPath);
    QString error;
    const auto resolved = pacsmith::TerminalLauncher::resolve(
        QStringLiteral("/usr/bin/true"), helperArguments, environment, &error);
    QVERIFY2(resolved.has_value(), qPrintable(error));
    QCOMPARE(resolved->program, kittyPath);
}

void CoreTests::buildsNonInteractivePacmanArgumentsSafely() {
    const auto hostilePath = QStringLiteral("/tmp/vendor package;$(touch nope).pkg.tar.zst");
    QCOMPARE(pacsmith::InstallService::installArguments(hostilePath, true),
             QStringList({QStringLiteral("/usr/bin/pacman"), QStringLiteral("--noconfirm"),
                          QStringLiteral("-U"), QStringLiteral("--"), hostilePath}));
    QCOMPARE(pacsmith::InstallService::uninstallArguments(QStringLiteral("vendor-bin"), true),
             QStringList({QStringLiteral("/usr/bin/pacman"), QStringLiteral("--noconfirm"),
                          QStringLiteral("-R"), QStringLiteral("--"),
                          QStringLiteral("vendor-bin")}));
    const auto interactive = pacsmith::InstallService::installArguments(hostilePath, false);
    QVERIFY(!interactive.contains(QStringLiteral("--noconfirm")));
    QVERIFY(!interactive.contains(QStringLiteral("sh")));
    QVERIFY(!interactive.contains(QStringLiteral("-c")));
    QCOMPARE(interactive.last(), hostilePath);
}

void CoreTests::buildsRebuildableMakepkgArguments() {
    const auto arguments = pacsmith::BuildService::makepkgArguments();
    QCOMPARE(arguments, QStringList({QStringLiteral("--force"), QStringLiteral("--nodeps")}));
    QVERIFY(!arguments.contains(QStringLiteral("--skipchecksums")));
    QVERIFY(!arguments.contains(QStringLiteral("--skippgpcheck")));
}

void CoreTests::validatesInstallSessionProtocol() {
    const auto token = QString(64, QLatin1Char('a'));
    const pacsmith::InstallSessionEvent output{QStringLiteral("output"), token,
                                               QStringLiteral("line one\n$() ; quotes '\"\n")};
    QString error;
    const auto restoredOutput = pacsmith::InstallSessionProtocol::decode(
        QByteArrayView(pacsmith::InstallSessionProtocol::encode(output).trimmed()), &error);
    QVERIFY2(restoredOutput.has_value(), qPrintable(error));
    QCOMPARE(restoredOutput->type, output.type);
    QCOMPARE(restoredOutput->token, token);
    QCOMPARE(restoredOutput->text, output.text);

    const pacsmith::InstallSessionEvent finished{QStringLiteral("finished"), token, {}, 17,
                                                 QProcess::CrashExit, true};
    const auto restoredFinished = pacsmith::InstallSessionProtocol::decode(
        QByteArrayView(pacsmith::InstallSessionProtocol::encode(finished).trimmed()), &error);
    QVERIFY2(restoredFinished.has_value(), qPrintable(error));
    QCOMPARE(restoredFinished->exitCode, 17);
    QCOMPARE(restoredFinished->exitStatus, QProcess::CrashExit);
    QVERIFY(restoredFinished->canceled);

    QVERIFY(!pacsmith::InstallSessionProtocol::decode(
        QByteArrayView(QByteArrayLiteral(R"({"type":"output","token":"short","text":"x"})")),
        &error));
    QVERIFY(error.contains(QStringLiteral("token")));
    QVERIFY(!pacsmith::InstallSessionProtocol::decode(
        QByteArrayView(QByteArrayLiteral(
            R"({"type":"run-command","token":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"})")),
        &error));
    QVERIFY(error.contains(QStringLiteral("Unknown")));
}

void CoreTests::encryptsCredentialsWithAge() {
    if (!pacsmith::CredentialStore::ageAvailable()) QSKIP("age is not installed");
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto path = temporary.path() + QStringLiteral("/secrets.age");
    const QString password = QStringLiteral("pacsmith automated test password");
    {
        pacsmith::CredentialStore store(path);
        QString error;
        QVERIFY(!store.unlockAge(password, &error));
        QVERIFY2(store.createAge(password, &error), qPrintable(error));
        QVERIFY(store.hasAgeFile());
        QVERIFY(store.ageUnlocked());
        QVERIFY2(store.store(QStringLiteral("openai"), pacsmith::CredentialSource::Age,
                             QStringLiteral("test-secret-value"), password, &error), qPrintable(error));
        QVERIFY(QFileInfo::exists(path));
        const auto encrypted = QFileInfo(path).size();
        QVERIFY(encrypted > 0);
    }
    {
        pacsmith::CredentialStore store(path);
        QString error;
        QVERIFY2(store.unlockAge(password, &error), qPrintable(error));
        const auto secret = store.load(QStringLiteral("openai"), pacsmith::CredentialSource::Age, &error);
        QVERIFY2(secret.has_value(), qPrintable(error));
        QCOMPARE(*secret, QStringLiteral("test-secret-value"));
        QVERIFY2(store.remove(QStringLiteral("openai"), pacsmith::CredentialSource::Age,
                              password, &error), qPrintable(error));
        QVERIFY(!store.load(QStringLiteral("openai"), pacsmith::CredentialSource::Age, &error));
        store.lockAge();
        QVERIFY(!store.load(QStringLiteral("openai"), pacsmith::CredentialSource::Age, &error));
    }
}

void CoreTests::migratesScriptEvidenceForExistingProjects() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    pacsmith::ProjectStore store(
        std::filesystem::path(temporary.path().toUtf8().constData()) / "projects");
    pacsmith::PackageRelease legacy;
    legacy.formatVersion = 3;
    legacy.id = QStringLiteral("legacy-vendor");
    legacy.displayName = QStringLiteral("Legacy Vendor");
    legacy.archPackageName = QStringLiteral("pacsmith-test-package-that-is-not-installed");
    legacy.debian.package = QStringLiteral("legacy-vendor");
    legacy.debian.version = QStringLiteral("1.0");
    legacy.debian.architecture = QStringLiteral("amd64");
    legacy.sourceSha256 = QString(64, QLatin1Char('a'));
    legacy.maintainerScripts.append(
        {QStringLiteral("postinst"),
         QStringLiteral("cat > /etc/apt/sources.list.d/vendor.sources <<'EOF'\n"
                        "Types: deb\nURIs: https://repo.vendor.example/deb\nSuites: stable\n"
                        "Components: main\nArchitectures: amd64\nEOF\n"
                        "update-mime-database /usr/share/mime\n"
                        "apparmor_parser -r /etc/apparmor.d/vendor\n"), {}});
    const auto duplicatedLegacyFingerprint = QString(64, QLatin1Char('d'));
    legacy.scriptFindings.append(
        {QStringLiteral("postinst"), QStringLiteral("arch-cache-hook"),
         QStringLiteral("Legacy cache finding"), {}, duplicatedLegacyFingerprint,
         pacsmith::ScriptDisposition::HandledByArch, {}});
    legacy.scriptFindings.append(
        {QStringLiteral("postinst"), QStringLiteral("apparmor"),
         QStringLiteral("Legacy AppArmor finding"), {}, duplicatedLegacyFingerprint,
         pacsmith::ScriptDisposition::LifecycleRequired, {}});
    legacy.payload.append(
        {QStringLiteral("etc/cron.daily/vendor"), QStringLiteral("file"), {}, 180, true,
         QStringLiteral("System configuration should be reviewed"), QStringLiteral("payload-hash"),
         QStringLiteral("REPOCONFIG='https://packages.vendor.example/fedora/40'\n"
                        "DEFAULT_ARCH='x86_64'\n"
                        "baseurl=$REPOCONFIG/$DEFAULT_ARCH\n"), false});
    const auto legacyDirectory = store.projectPath(legacy.id);
    QVERIFY(QDir{}.mkpath(QString::fromUtf8(legacyDirectory.string().c_str())));
    QFile legacyFile(QString::fromUtf8((legacyDirectory / "project.json").string().c_str()));
    QVERIFY(legacyFile.open(QIODevice::WriteOnly));
    const auto legacyJson = QJsonDocument(legacy.toJson()).toJson(QJsonDocument::Indented);
    QCOMPARE(legacyFile.write(legacyJson), legacyJson.size());
    legacyFile.close();
    QString error;
    const auto loaded = store.load(legacy.id, &error);
    QVERIFY2(loaded.has_value(), qPrintable(error));
    QCOMPARE(loaded->formatVersion, 5);
    QCOMPARE(loaded->releases.size(), 1);
    const auto &release = loaded->releases.first();
    QCOMPARE(release.update.strategy, pacsmith::UpdateStrategy::AptRepository);
    QCOMPARE(release.update.url, QStringLiteral("https://repo.vendor.example/deb"));
    QCOMPARE(release.update.aptSuite, QStringLiteral("stable"));
    QCOMPARE(release.update.rpmCandidates.size(), 1);
    QCOMPARE(release.update.rpmCandidates.first().baseUrl,
             QStringLiteral("https://packages.vendor.example/fedora/40/x86_64"));
    QVERIFY(!release.scriptFindings.isEmpty());
    QCOMPARE(release.scriptFindings.first().disposition,
             pacsmith::ScriptDisposition::HandledByPacSmith);
    QSet<QString> migratedFindingFingerprints;
    for (const auto &finding : release.scriptFindings) {
        QVERIFY(!migratedFindingFingerprints.contains(finding.evidenceFingerprint));
        migratedFindingFingerprints.insert(finding.evidenceFingerprint);
    }
    QCOMPARE(migratedFindingFingerprints.size(), release.scriptFindings.size());
    QCOMPARE(release.fieldProvenance.value(QStringLiteral("update.url")).origin,
             pacsmith::ValueOrigin::Deterministic);
}

void CoreTests::handlesProjectPaths() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    pacsmith::ProjectStore store(std::filesystem::path(temporary.path().toUtf8().constData()) / "projects");
    pacsmith::Project project;
    project.id = QStringLiteral("safe-id");
    project.displayName = QStringLiteral("Safe Project");
    project.archPackageName = QStringLiteral("safe-id-bin");
    project.createdAt = QDateTime::currentDateTimeUtc();
    QString error;
    QVERIFY2(store.save(project, &error), qPrintable(error));
    QCOMPARE(QString::fromUtf8(store.projectPath(project.id).filename().string().c_str()), QStringLiteral("safe-id"));
    const auto loaded = store.load(QStringLiteral("safe-id"), &error);
    QVERIFY2(loaded.has_value(), qPrintable(error));
    QCOMPARE(loaded->displayName, QStringLiteral("Safe Project"));
}

void CoreTests::detectsManualPkgbuildEdits() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    pacsmith::ProjectStore store(std::filesystem::path(temporary.path().toUtf8().constData()) / "projects");
    pacsmith::Project project;
    project.id = QStringLiteral("editable");
    project.displayName = QStringLiteral("Editable");
    project.archPackageName = QStringLiteral("editable-bin");
    pacsmith::PackageRelease release;
    release.id = QStringLiteral("1.0-aaaaaaaaaaaa");
    release.projectId = project.id;
    release.archPackageName = project.archPackageName;
    release.generatedPkgbuild = QStringLiteral("pkgname='editable-bin'\n");
    release.generatedPkgbuildSha256 = pacsmith::sha256Hex(release.generatedPkgbuild.toUtf8());
    project.releases.append(release);
    QString error;
    QVERIFY2(store.save(project, &error), qPrintable(error));
    auto &savedRelease = project.releases.first();
    QVERIFY2(store.savePkgbuild(project, savedRelease, savedRelease.generatedPkgbuild, &error), qPrintable(error));
    QVERIFY(!savedRelease.pkgbuildManuallyModified);
    QVERIFY2(store.savePkgbuild(project, savedRelease,
                               savedRelease.generatedPkgbuild + QStringLiteral("# user edit\n"), &error),
             qPrintable(error));
    QVERIFY(savedRelease.pkgbuildManuallyModified);
    const auto reopened = store.load(project.id, &error);
    QVERIFY2(reopened.has_value(), qPrintable(error));
    QVERIFY(reopened->releases.first().pkgbuildManuallyModified);
    QCOMPARE(reopened->releases.first().customPkgbuild,
             savedRelease.generatedPkgbuild + QStringLiteral("# user edit\n"));
}

void CoreTests::roundTripsGuidedAndCustomPkgbuilds() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    pacsmith::ProjectStore store(
        std::filesystem::path(temporary.path().toUtf8().constData()) / "projects");
    pacsmith::Project project;
    project.id = QStringLiteral("roundtrip");
    project.displayName = QStringLiteral("Roundtrip");
    project.archPackageName = QStringLiteral("roundtrip-bin");
    pacsmith::PackageRelease release;
    release.id = QStringLiteral("1.0-cccccccccccc");
    release.projectId = project.id;
    release.archPackageName = project.archPackageName;
    release.generatedPkgbuild = QStringLiteral("pkgname='roundtrip-bin'\npkgver=1\n");
    release.generatedPkgbuildSha256 = pacsmith::sha256Hex(release.generatedPkgbuild.toUtf8());
    project.releases.append(release);
    QString error;
    QVERIFY2(store.save(project, &error), qPrintable(error));
    auto &saved = project.releases.first();
    QVERIFY2(store.activateGuidedPkgbuild(project, saved, &error), qPrintable(error));
    QVERIFY(!saved.pkgbuildManuallyModified);

    QVERIFY2(store.activateCustomPkgbuild(project, saved, &error), qPrintable(error));
    QVERIFY(saved.pkgbuildManuallyModified);
    QCOMPARE(saved.customPkgbuild, saved.generatedPkgbuild);
    QCOMPARE(store.readPkgbuild(saved, &error).value_or(QString{}), saved.generatedPkgbuild);

    const auto edited = QStringLiteral("pkgname='roundtrip-bin'\npkgver=1\n# custom\n");
    QVERIFY2(store.saveCustomPkgbuild(project, saved, edited, &error), qPrintable(error));
    QCOMPARE(saved.customPkgbuild, edited);
    QVERIFY(saved.pkgbuildManuallyModified);

    saved.generatedPkgbuild = QStringLiteral("pkgname='roundtrip-bin'\npkgver=1\n# regenerated\n");
    saved.generatedPkgbuildSha256 = pacsmith::sha256Hex(saved.generatedPkgbuild.toUtf8());
    QVERIFY2(store.save(project, &error), qPrintable(error));
    QCOMPARE(saved.customPkgbuild, edited);

    QVERIFY2(store.activateGuidedPkgbuild(project, saved, &error), qPrintable(error));
    QVERIFY(!saved.pkgbuildManuallyModified);
    QCOMPARE(saved.customPkgbuild, edited);
    QCOMPARE(store.readPkgbuild(saved, &error).value_or(QString{}), saved.generatedPkgbuild);

    QVERIFY2(store.activateCustomPkgbuild(project, saved, &error), qPrintable(error));
    QVERIFY(saved.pkgbuildManuallyModified);
    QCOMPARE(store.readPkgbuild(saved, &error).value_or(QString{}), edited);

    const auto reopened = store.load(project.id, &error);
    QVERIFY2(reopened.has_value(), qPrintable(error));
    QCOMPARE(reopened->releases.first().customPkgbuild, edited);
    QVERIFY(reopened->releases.first().pkgbuildManuallyModified);
}

void CoreTests::detectsExternalLifecycleEdits() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    pacsmith::ProjectStore store(
        std::filesystem::path(temporary.path().toUtf8().constData()) / "projects");
    pacsmith::Project project;
    project.id = QStringLiteral("lifecycle-edit");
    project.displayName = QStringLiteral("Lifecycle Edit");
    project.archPackageName = QStringLiteral("lifecycle-edit-bin");
    pacsmith::PackageRelease release;
    release.id = QStringLiteral("1.0-bbbbbbbbbbbb");
    release.projectId = project.id;
    release.archPackageName = project.archPackageName;
    release.lifecycleScript.fileName = QStringLiteral("lifecycle-edit-bin.install");
    release.lifecycleScript.contents = QStringLiteral("post_install() { /usr/bin/true; }\n");
    project.releases.append(release);
    QString error;
    QVERIFY2(store.save(project, &error), qPrintable(error));
    auto &savedRelease = project.releases.first();
    QVERIFY2(store.saveLifecycle(project, savedRelease, &error), qPrintable(error));
    QVERIFY(savedRelease.lifecycleScript.validationPassed);
    savedRelease.lifecycleScript.acknowledge();
    savedRelease.buildStatus = pacsmith::BuildStatus::Succeeded;
    savedRelease.producedPackages.append(QStringLiteral("/tmp/lifecycle-edit.pkg.tar.zst"));
    QVERIFY2(store.save(project, &error), qPrintable(error));

    const auto lifecyclePath = store.lifecyclePath(savedRelease);
    QVERIFY(QFile::remove(QString::fromUtf8(lifecyclePath.string().c_str())));
    bool changed = false;
    QVERIFY2(store.synchronizeLifecycle(project, savedRelease, &changed, &error), qPrintable(error));
    QVERIFY(changed);
    QVERIFY(QFileInfo::exists(QString::fromUtf8(lifecyclePath.string().c_str())));
    QVERIFY(savedRelease.lifecycleScript.validationPassed);
    QVERIFY(!savedRelease.lifecycleScript.requiresAcknowledgement());

    savedRelease.buildStatus = pacsmith::BuildStatus::Succeeded;
    savedRelease.producedPackages.append(QStringLiteral("/tmp/lifecycle-edit.pkg.tar.zst"));
    QVERIFY2(store.save(project, &error), qPrintable(error));
    QFile lifecycleFile(QString::fromUtf8(lifecyclePath.string().c_str()));
    QVERIFY(lifecycleFile.open(QIODevice::WriteOnly | QIODevice::Truncate));
    const auto changedContents = QByteArrayLiteral("post_install() { /usr/bin/false || true; }\n");
    QCOMPARE(lifecycleFile.write(changedContents), changedContents.size());
    lifecycleFile.close();
    changed = false;
    QVERIFY2(store.synchronizeLifecycle(project, savedRelease, &changed, &error), qPrintable(error));
    QVERIFY(changed);
    QVERIFY(savedRelease.lifecycleScript.manuallyModified);
    QVERIFY(savedRelease.lifecycleScript.requiresAcknowledgement());
    QVERIFY(savedRelease.lifecycleScript.validationPassed);
    QCOMPARE(savedRelease.buildStatus, pacsmith::BuildStatus::NeverBuilt);
    QVERIFY(savedRelease.producedPackages.isEmpty());

    QVERIFY2(store.removeLifecycle(project, savedRelease, &error), qPrintable(error));
    QVERIFY(savedRelease.lifecycleScript.contents.isEmpty());
    QVERIFY(!QFileInfo::exists(QString::fromUtf8(lifecyclePath.string().c_str())));
}

void CoreTests::reanalyzesReleaseFromBlankPackageSetup() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto fixtureRoot =
        std::filesystem::path(temporary.path().toUtf8().constData()) / "fixture";
    const auto writeFixture = [&](const std::filesystem::path &relative,
                                  const QByteArray &contents) {
        const auto path = fixtureRoot / relative;
        std::filesystem::create_directories(path.parent_path());
        QFile file(QString::fromUtf8(path.string().c_str()));
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(contents), contents.size());
    };
    writeFixture("opt/vendor/vendor", QByteArrayLiteral("#!/bin/sh\nexit 0\n"));
    std::filesystem::permissions(
        fixtureRoot / "opt/vendor/vendor",
        std::filesystem::perms::owner_read | std::filesystem::perms::owner_write |
            std::filesystem::perms::owner_exec | std::filesystem::perms::group_read |
            std::filesystem::perms::group_exec | std::filesystem::perms::others_read |
            std::filesystem::perms::others_exec,
        std::filesystem::perm_options::replace);
    writeFixture(
        "usr/share/applications/vendor.desktop",
        QByteArrayLiteral("[Desktop Entry]\nType=Application\nName=Vendor\n"
                          "Exec=vendor\nIcon=vendor\n"));
    writeFixture(
        "usr/share/icons/hicolor/1x1/apps/vendor.png",
        QByteArray::fromBase64(
            "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNk+A8AAQUBAScY42YAAAAASUVORK5CYII="));

    const auto archive = temporary.filePath(QStringLiteral("vendor-2.0-x86_64.tar"));
    QProcess tar;
    tar.setProgram(QStringLiteral("/usr/bin/bsdtar"));
    tar.setArguments({QStringLiteral("-cf"), archive, QStringLiteral("-C"),
                      QString::fromUtf8(fixtureRoot.string().c_str()),
                      QStringLiteral("opt"), QStringLiteral("usr")});
    tar.start();
    QVERIFY(tar.waitForFinished(10000));
    QCOMPARE(tar.exitCode(), 0);

    pacsmith::ProjectStore store(
        std::filesystem::path(temporary.path().toUtf8().constData()) / "projects");
    pacsmith::Project project;
    project.id = QStringLiteral("vendor");
    project.displayName = QStringLiteral("Vendor");
    project.archPackageName = QStringLiteral("vendor-bin");
    pacsmith::PackageRelease release;
    release.id = QStringLiteral("2.0-aaaaaaaaaaaa");
    release.projectId = project.id;
    release.displayName = project.displayName;
    release.archPackageName = project.archPackageName;
    release.sourceType = pacsmith::SourcePackageType::Archive;
    release.originalSourceFilename = QFileInfo(archive).fileName();
    QString error;
    release.sourceSha256 = pacsmith::sha256File(
        std::filesystem::path(archive.toUtf8().constData()), &error);
    QVERIFY2(!release.sourceSha256.isEmpty(), qPrintable(error));
    release.debian.package = QStringLiteral("vendor");
    release.debian.version = QStringLiteral("2.0");
    release.debian.architecture = QStringLiteral("amd64");
    release.dependencies.append(
        {QStringLiteral("obsolete"), {}, QStringLiteral("made-up-package"),
         pacsmith::MappingStatus::Resolved, QStringLiteral("user override"), 1.0,
         true, false, false, false});
    release.installMapping.desktopEntries.clear();
    release.installMapping.launchers.clear();
    release.lifecycleScript.fileName = QStringLiteral("vendor-bin.install");
    release.lifecycleScript.contents = QStringLiteral("post_install() { /usr/bin/true; }\n");
    release.pkgbuildManuallyModified = true;
    release.generatedPkgbuild = QStringLiteral("# stale generated recipe\n");
    release.previousManualPkgbuild = QStringLiteral("# user-owned recipe\n");
    release.update.strategy = pacsmith::UpdateStrategy::DirectUrl;
    release.update.url = QStringLiteral("https://vendor.example/releases/latest.tar");
    release.buildStatus = pacsmith::BuildStatus::Succeeded;
    release.producedPackages.append(QStringLiteral("/tmp/vendor-old.pkg.tar.zst"));
    pacsmith::BuildRecord oldBuild;
    oldBuild.id = QStringLiteral("old-build");
    oldBuild.status = pacsmith::BuildStatus::Succeeded;
    release.builds.append(oldBuild);
    release.createdAt = QDateTime::currentDateTimeUtc();
    project.releases.append(release);

    const auto storedSource = store.sourcePath(project.releases.first());
    std::filesystem::create_directories(storedSource.parent_path());
    QVERIFY(QFile::copy(archive, QString::fromUtf8(storedSource.string().c_str())));
    const auto lifecycle = store.lifecyclePath(project.releases.first());
    QFile lifecycleFile(QString::fromUtf8(lifecycle.string().c_str()));
    QVERIFY(lifecycleFile.open(QIODevice::WriteOnly));
    lifecycleFile.write(release.lifecycleScript.contents.toUtf8());
    lifecycleFile.close();
    QVERIFY2(store.save(project, &error), qPrintable(error));

    const auto result = store.reanalyzeRelease(project.id, release.id, &error);
    QVERIFY2(result.has_value(), qPrintable(error));
    const auto *reset = result->project.release(release.id);
    QVERIFY(reset != nullptr);
    QVERIFY(!reset->installMapping.launchers.isEmpty());
    QCOMPARE(reset->installMapping.launchers.first().sourcePath,
             QStringLiteral("opt/vendor/vendor"));
    QVERIFY(reset->installMapping.launchers.first().enabled);
    QCOMPARE(reset->installMapping.launchers.first().commandName, QStringLiteral("vendor"));
    QCOMPARE(reset->installMapping.launchers.first().destination,
             QStringLiteral("/usr/bin/vendor"));
    QCOMPARE(reset->installMapping.desktopEntries.size(), 1);
    QCOMPARE(reset->installMapping.desktopEntries.first().sourcePath,
             QStringLiteral("usr/share/applications/vendor.desktop"));
    QVERIFY(reset->dependencies.isEmpty());
    QVERIFY(reset->lifecycleScript.contents.isEmpty());
    QVERIFY(!reset->pkgbuildManuallyModified);
    QVERIFY(reset->previousManualPkgbuild.isEmpty());
    QCOMPARE(reset->update.url,
             QStringLiteral("https://vendor.example/releases/latest.tar"));
    QCOMPARE(reset->buildStatus, pacsmith::BuildStatus::NeverBuilt);
    QVERIFY(reset->producedPackages.isEmpty());
    QCOMPARE(reset->builds.size(), 1);
    QVERIFY(!QFileInfo::exists(QString::fromUtf8(lifecycle.string().c_str())));
    QFile pkgbuild(QString::fromUtf8(store.pkgbuildPath(*reset).string().c_str()));
    QVERIFY(pkgbuild.open(QIODevice::ReadOnly));
    const auto regenerated = pkgbuild.readAll();
    QVERIFY(regenerated.contains("pkgname='vendor-bin'"));
    QVERIFY(!regenerated.contains("user-owned recipe"));
}

void CoreTests::detectsDebDeclaredOptCommandWithoutExecutingScript() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto root = std::filesystem::path(temporary.path().toUtf8().constData());
    const auto writeFixture = [&](const std::filesystem::path &path,
                                  const QByteArray &contents) {
        std::filesystem::create_directories(path.parent_path());
        QFile file(QString::fromUtf8(path.string().c_str()));
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(contents), contents.size());
    };
    const auto controlRoot = root / "control";
    const auto dataRoot = root / "data";
    writeFixture(controlRoot / "control",
                 QByteArrayLiteral("Package: signal-fixture\nVersion: 1.0\n"
                                   "Architecture: amd64\nDescription: Fixture\n"));
    writeFixture(controlRoot / "postinst",
                 QByteArrayLiteral("#!/bin/sh\n"
                                   "update-alternatives --install '/usr/bin/signal-fixture' "
                                   "'signal-fixture' '/opt/SignalFixture/signal-fixture' 100 "
                                   "|| ln -sf '/opt/SignalFixture/signal-fixture' "
                                   "'/usr/bin/signal-fixture'\n"));
    writeFixture(dataRoot / "opt/SignalFixture/signal-fixture",
                 QByteArrayLiteral("#!/bin/sh\nexit 0\n"));
    std::filesystem::permissions(
        dataRoot / "opt/SignalFixture/signal-fixture",
        std::filesystem::perms::owner_read | std::filesystem::perms::owner_write |
            std::filesystem::perms::owner_exec | std::filesystem::perms::group_read |
            std::filesystem::perms::group_exec | std::filesystem::perms::others_read |
            std::filesystem::perms::others_exec,
        std::filesystem::perm_options::replace);
    writeFixture(dataRoot / "usr/share/applications/signal-fixture.desktop",
                 QByteArrayLiteral("[Desktop Entry]\nType=Application\nName=Signal Fixture\n"
                                   "Exec=signal-fixture\n"));
    writeFixture(root / "debian-binary", QByteArrayLiteral("2.0\n"));

    const auto makeTar = [&](const QString &output, const std::filesystem::path &directory) {
        QProcess tar;
        tar.setProgram(QStringLiteral("/usr/bin/bsdtar"));
        tar.setArguments({QStringLiteral("-cf"), output, QStringLiteral("-C"),
                          QString::fromUtf8(directory.string().c_str()), QStringLiteral(".")});
        tar.start();
        QVERIFY(tar.waitForFinished(10000));
        QCOMPARE(tar.exitCode(), 0);
    };
    const auto controlTar = temporary.filePath(QStringLiteral("control.tar"));
    const auto dataTar = temporary.filePath(QStringLiteral("data.tar"));
    makeTar(controlTar, controlRoot);
    makeTar(dataTar, dataRoot);
    const auto deb = temporary.filePath(QStringLiteral("signal-fixture_1.0_amd64.deb"));
    QProcess ar;
    ar.setWorkingDirectory(temporary.path());
    ar.setProgram(QStringLiteral("/usr/bin/ar"));
    ar.setArguments({QStringLiteral("r"), QFileInfo(deb).fileName(),
                     QStringLiteral("debian-binary"), QStringLiteral("control.tar"),
                     QStringLiteral("data.tar")});
    ar.start();
    QVERIFY(ar.waitForFinished(10000));
    QCOMPARE(ar.exitCode(), 0);

    pacsmith::AnalysisError error;
    const auto analysis = pacsmith::DebAnalyzer{}.analyze(
        std::filesystem::path(deb.toUtf8().constData()), error);
    QVERIFY2(analysis.has_value(), qPrintable(error.message));
    QCOMPARE(analysis->installMapping.desktopEntries.size(), 1);
    const auto launcher = std::find_if(
        analysis->installMapping.launchers.cbegin(),
        analysis->installMapping.launchers.cend(), [](const auto &candidate) {
            return candidate.sourcePath ==
                   QStringLiteral("opt/SignalFixture/signal-fixture");
        });
    QVERIFY(launcher != analysis->installMapping.launchers.cend());
    QVERIFY(launcher->enabled);
    QCOMPARE(launcher->destination, QStringLiteral("/usr/bin/signal-fixture"));
    QVERIFY(launcher->provenance.rationale.contains(
        QStringLiteral("maintainer script explicitly exposes")));
}

void CoreTests::inspectsDebAndAppImagePayloadContents() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto root = std::filesystem::path(temporary.path().toUtf8().constData());
    const auto writeFixture = [&](const std::filesystem::path &path,
                                  const QByteArray &contents) {
        std::filesystem::create_directories(path.parent_path());
        QFile file(QString::fromUtf8(path.string().c_str()));
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(contents), contents.size());
    };
    writeFixture(root / "control/control",
                 QByteArrayLiteral("Package: vendor-app\nVersion: 1.0\nArchitecture: amd64\n"
                                   "Description: Fixture\n"));
    writeFixture(root / "data/etc/vendor.conf",
                 QByteArrayLiteral("# vendor configuration\nsetting=true\n"));
    writeFixture(root / "data/usr/share/pixmaps/vendor.png",
                 QByteArray::fromBase64(
                     "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNk+A8AAQUBAScY42YAAAAASUVORK5CYII="));
    writeFixture(root / "debian-binary", QByteArrayLiteral("2.0\n"));

    const auto makeTar = [&](const QString &output, const std::filesystem::path &directory) {
        QProcess tar;
        tar.setProgram(QStringLiteral("/usr/bin/bsdtar"));
        tar.setArguments({QStringLiteral("-cf"), output, QStringLiteral("-C"),
                          QString::fromUtf8(directory.string().c_str()), QStringLiteral(".")});
        tar.start();
        QVERIFY(tar.waitForFinished(10000));
        QCOMPARE(tar.exitCode(), 0);
    };
    makeTar(temporary.filePath(QStringLiteral("control.tar")), root / "control");
    makeTar(temporary.filePath(QStringLiteral("data.tar")), root / "data");
    const auto deb = temporary.filePath(QStringLiteral("vendor-app_1.0_amd64.deb"));
    QProcess ar;
    ar.setWorkingDirectory(temporary.path());
    ar.setProgram(QStringLiteral("/usr/bin/ar"));
    ar.setArguments({QStringLiteral("r"), QFileInfo(deb).fileName(),
                     QStringLiteral("debian-binary"), QStringLiteral("control.tar"),
                     QStringLiteral("data.tar")});
    ar.start();
    QVERIFY(ar.waitForFinished(10000));
    QCOMPARE(ar.exitCode(), 0);

    const auto debPath = std::filesystem::path(deb.toUtf8().constData());
    QString error;
    const auto text = pacsmith::PayloadInspector::inspectFile(
        debPath, QStringLiteral("etc/vendor.conf"), &error);
    QVERIFY2(text.has_value(), qPrintable(error));
    QVERIFY(text->textPreview.contains(QStringLiteral("setting=true")));
    QVERIFY(!text->contentSha256.isEmpty());
    const auto icon = pacsmith::PayloadInspector::readFileBytes(
        debPath, QStringLiteral("usr/share/pixmaps/vendor.png"), 4 * 1024 * 1024, &error);
    QVERIFY2(icon.has_value(), qPrintable(error));
    QVERIFY(icon->startsWith("\x89PNG"));
}

void CoreTests::deletesUninstalledProject() {
    if (!QFileInfo::exists(QStringLiteral("/usr/bin/pacman"))) QSKIP("pacman is required for this Arch-specific test");
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    pacsmith::ProjectStore store(std::filesystem::path(temporary.path().toUtf8().constData()) / "projects");
    pacsmith::Project project;
    project.id = QStringLiteral("deletable-project");
    project.displayName = QStringLiteral("Deletable Project");
    project.archPackageName = QStringLiteral("pacsmith-test-package-that-is-not-installed");
    QString error;
    QVERIFY2(store.save(project, &error), qPrintable(error));
    QVERIFY(std::filesystem::exists(store.projectPath(project.id)));
    QVERIFY2(store.deleteProject(project, &error), qPrintable(error));
    QVERIFY(!std::filesystem::exists(store.projectPath(project.id)));
}

void CoreTests::refusesToDeleteInstalledProject() {
    if (!QFileInfo::exists(QStringLiteral("/usr/bin/pacman"))) QSKIP("pacman is required for this Arch-specific test");
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    pacsmith::ProjectStore store(std::filesystem::path(temporary.path().toUtf8().constData()) / "projects");
    pacsmith::Project project;
    project.id = QStringLiteral("installed-project");
    project.displayName = QStringLiteral("Installed Project");
    project.archPackageName = QStringLiteral("pacman");
    QString error;
    QVERIFY2(store.save(project, &error), qPrintable(error));
    QVERIFY(!store.deleteProject(project, &error));
    QVERIFY(error.contains(QStringLiteral("is installed")));
    QVERIFY(std::filesystem::exists(store.projectPath(project.id)));
}

void CoreTests::generatesPkgbuild() {
    pacsmith::PackageRelease project;
    project.archPackageName = QStringLiteral("vendor-app-bin");
    project.displayName = QStringLiteral("Vendor App");
    project.originalSourceFilename = QStringLiteral("vendor app_1.2_amd64.deb");
    project.sourceSha256 = QString(64, QLatin1Char('a'));
    project.debian.version = QStringLiteral("2:1.2.3-4");
    project.debian.architecture = QStringLiteral("amd64");
    project.debian.description = QStringLiteral("Vendor desktop application");
    project.debian.homepage = QStringLiteral("https://vendor.example");
    pacsmith::DependencyMapping mapped;
    mapped.archPackage = QStringLiteral("gtk3");
    mapped.status = pacsmith::MappingStatus::Resolved;
    project.dependencies.append(mapped);
    project.payloadRules.append({QStringLiteral("etc/apt/sources.list.d/vendor.list"), true,
                                 QStringLiteral("APT configuration"), false, {}});
    project.lifecycleScript.fileName = QStringLiteral("vendor-app-bin.install");
    project.lifecycleScript.contents = QStringLiteral("post_install() {\n  :\n}\n");
    project.lifecycleScript.validationPassed = true;
    const auto pkgbuild = pacsmith::PkgbuildGenerator::generate(project);
    QVERIFY(pkgbuild.contains(QStringLiteral("pkgname='vendor-app-bin'")));
    QVERIFY(pkgbuild.contains(QStringLiteral("epoch=2")));
    QVERIFY(pkgbuild.contains(QStringLiteral("pkgver='1.2.3'")));
    QVERIFY(pkgbuild.contains(QStringLiteral("arch=('x86_64')")));
    QVERIFY(pkgbuild.contains(QStringLiteral("depends=('gtk3')")));
    QVERIFY(pkgbuild.contains(QStringLiteral("options=('!strip' '!debug')")));
    QVERIFY(pkgbuild.contains(QStringLiteral(
        "source=('vendor app_1.2_amd64.deb') # primary source -> sources/vendor app_1.2_amd64.deb")));
    QVERIFY(pkgbuild.contains(QStringLiteral("data.tar|data.tar.*")));
    QVERIFY(pkgbuild.contains(QStringLiteral("--no-same-owner")));
    QVERIFY(pkgbuild.contains(QStringLiteral("install='vendor-app-bin.install'")));
    QVERIFY(pkgbuild.contains(QStringLiteral("${pkgdir}/etc/apt/sources.list.d/vendor.list")));
    QVERIFY(!pkgbuild.contains(QStringLiteral("postinst")));

    project.lifecycleScript.validationPassed = false;
    const auto blockedLifecycle = pacsmith::PkgbuildGenerator::generate(project);
    QVERIFY(!blockedLifecycle.contains(QStringLiteral("install='vendor-app-bin.install'")));
}

void CoreTests::generatesMultiSourcePkgbuilds() {
    pacsmith::PackageRelease release;
    release.projectId = QStringLiteral("vendor-tool");
    release.id = QStringLiteral("2.1-aaaaaaaaaaaa");
    release.archPackageName = QStringLiteral("vendor-tool-bin");
    release.displayName = QStringLiteral("Vendor Tool");
    release.originalSourceFilename = QStringLiteral("vendor-tool-2.1-linux-x86_64.tar.gz");
    release.sourceSha256 = QString(64, QLatin1Char('a'));
    release.debian.version = QStringLiteral("2.1");
    release.debian.architecture = QStringLiteral("amd64");
    release.acquisition.kind = pacsmith::AcquisitionKind::GitHubRelease;
    release.acquisition.canonicalIdentity = QStringLiteral("github:vendor/tool");
    release.sourceType = pacsmith::SourcePackageType::Archive;
    release.installMapping.archiveLayout = pacsmith::ArchiveLayout::OptBundle;
    release.installMapping.optDirectory = QStringLiteral("vendor-tool");
    release.installMapping.commonPrefix = QStringLiteral("vendor-tool-2.1");
    release.installMapping.stripCommonPrefix = true;
    pacsmith::LauncherMapping archiveLauncher;
    archiveLauncher.sourcePath = QStringLiteral("vendor-tool-2.1/bin/tool");
    archiveLauncher.commandName = QStringLiteral("tool");
    archiveLauncher.destination = QStringLiteral("/usr/bin/tool");
    release.installMapping.launchers.append(archiveLauncher);
    pacsmith::DesktopEntryConfiguration archiveDesktop;
    archiveDesktop.id = QStringLiteral("vendor-tool");
    archiveDesktop.destination =
        QStringLiteral("/usr/share/applications/vendor-tool.desktop");
    archiveDesktop.contents = QStringLiteral(
        "[Desktop Entry]\nType=Application\nName=Vendor Tool\nExec=tool\nIcon=vendor-tool\n");
    release.installMapping.desktopEntries.append(archiveDesktop);
    release.installMapping.icon.sourceKind = pacsmith::IconSourceKind::LocalFile;
    release.installMapping.icon.projectPath = QStringLiteral("files/integration/icon.svg");
    release.installMapping.icon.sha256 = QString(64, QLatin1Char('b'));
    release.installMapping.icon.format = QStringLiteral("svg");
    release.installMapping.icon.iconName = QStringLiteral("vendor-tool");
    const auto archive = pacsmith::PkgbuildGenerator::generate(release);
    QVERIFY(archive.contains(QStringLiteral("options=('!strip' '!debug')")));
    QVERIFY(archive.contains(QStringLiteral("pacsmith.schema=1")));
    QVERIFY(archive.contains(QStringLiteral("pacsmith.source=github%3Avendor%2Ftool")));
    QVERIFY(archive.contains(QStringLiteral("$pkgdir/opt/vendor-tool")));
    QCOMPARE(pacsmith::PkgbuildGenerator::installedPayloadPath(
                 release, QStringLiteral("vendor-tool-2.1/bin/tool")),
             QStringLiteral("/opt/vendor-tool/bin/tool"));
    QVERIFY(archive.contains(QStringLiteral("--strip-components 1")));
    QVERIFY(archive.contains(QStringLiteral("../../opt/vendor-tool/bin/tool")));
    QVERIFY(archive.contains(QStringLiteral("$pkgdir/usr/bin/tool")));
    QVERIFY(archive.contains(QStringLiteral("'pacsmith-icon.svg'")));
    QVERIFY(archive.contains(QStringLiteral("/usr/share/applications/vendor-tool.desktop")));
    QVERIFY(archive.contains(QStringLiteral("/usr/share/icons/hicolor/scalable/apps/vendor-tool.svg")));
    QVERIFY(archive.contains(QStringLiteral("--no-same-owner")));

    release.sourceType = pacsmith::SourcePackageType::AppImage;
    release.originalSourceFilename = QStringLiteral("VendorTool.AppImage");
    release.installMapping.appImageOffset = 4096;
    release.installMapping.launchers.first().sourcePath = QStringLiteral("AppRun");
    release.installMapping.launchers.first().kind = pacsmith::LauncherKind::Wrapper;
    release.installMapping.desktopEntries.first().sourcePath =
        QStringLiteral("vendor-tool.desktop");
    pacsmith::DesktopEntryConfiguration internalDesktop;
    internalDesktop.id = QStringLiteral("python3.10");
    internalDesktop.enabled = false;
    internalDesktop.sourcePath =
        QStringLiteral("usr/share/applications/python3.10.desktop");
    internalDesktop.destination =
        QStringLiteral("/usr/share/applications/python3.10.desktop");
    internalDesktop.contents = QStringLiteral(
        "[Desktop Entry]\nType=Application\nName=Python\nExec=python3.10\nNoDisplay=true\n");
    release.installMapping.desktopEntries.append(internalDesktop);
    release.payloadRules.append(
        {QStringLiteral("etc"), true, QStringLiteral("legacy AppImage decision"), true,
         QString(64, QLatin1Char('c'))});
    const auto appImage = pacsmith::PkgbuildGenerator::generate(release);
    QVERIFY(appImage.contains(QStringLiteral("makedepends=('squashfs-tools')")));
    QVERIFY(appImage.contains(QStringLiteral("Preserve the complete AppDir below /opt")));
    QVERIFY(appImage.contains(QStringLiteral("unsquashfs -no-progress -no-xattrs -f -o 4096")));
    QVERIFY(appImage.contains(QStringLiteral("-type f -exec chmod u-s,g-s")));
    QVERIFY(appImage.contains(QStringLiteral("APPDIR='/opt/vendor-tool'")));
    QVERIFY(appImage.contains(QStringLiteral("unset APPIMAGE")));
    QVERIFY(appImage.contains(QStringLiteral("exec \"/opt/vendor-tool/AppRun\" \"$@\"")));
    QVERIFY(!appImage.contains(QStringLiteral("exec -a")));
    QVERIFY(!appImage.contains(QStringLiteral("APPIMAGE=extracted")));
    QVERIFY(!appImage.contains(QStringLiteral("$pkgdir/opt/vendor-tool/AppRun")));
    QVERIFY(appImage.contains(QStringLiteral("printf '%s'")));
    QVERIFY(!appImage.contains(QStringLiteral("printf '%%s'")));
    QVERIFY(!appImage.contains(QStringLiteral("$pkgdir/opt/vendor-tool/vendor-tool.desktop")));
    QVERIFY(!appImage.contains(QStringLiteral("python3.10.desktop")));
    QVERIFY(!appImage.contains(QStringLiteral("${pkgdir}/etc")));

    release.installMapping.appRun.present = true;
    release.installMapping.appRun.script = true;
    release.installMapping.appRun.originalContents = QStringLiteral(
        "#!/bin/bash\nBINARY_NAME=$(basename \"$0\")\nexec \"$HERE/$BINARY_NAME\" \"$@\"\n");
    release.installMapping.appRun.contents =
        QStringLiteral("#!/bin/sh\nexec \"$APPDIR/vendor-tool\" \"$@\"\n");
    release.installMapping.appRun.userModified = true;
    const auto overlaid = pacsmith::PkgbuildGenerator::generate(release);
    QVERIFY(overlaid.contains(QStringLiteral("$pkgdir/opt/vendor-tool/AppRun")));
    QVERIFY(overlaid.contains(QStringLiteral("exec \"$APPDIR/vendor-tool\" \"$@\"")));
    QVERIFY(overlaid.contains(QStringLiteral("unset APPIMAGE")));
    QVERIFY(overlaid.contains(QStringLiteral("exec \"/opt/vendor-tool/AppRun\" \"$@\"")));

    release.sourceType = pacsmith::SourcePackageType::ElfBinary;
    release.originalSourceFilename = QStringLiteral("tool");
    release.installMapping.binaryDestination = QStringLiteral("/usr/bin/tool");
    release.installMapping.launchers.clear();
    const auto elf = pacsmith::PkgbuildGenerator::generate(release);
    QVERIFY(elf.contains(QStringLiteral("install -Dm755 \"$srcdir/tool\" \"$pkgdir/usr/bin/tool\"")));

    release.sourceType = pacsmith::SourcePackageType::ArchPackage;
    release.originalSourceFilename = QStringLiteral("vendor-tool-2.1-1-x86_64.pkg.tar.zst");
    release.archPkgrelOverride = QStringLiteral("1.1");
    const auto archPackage = pacsmith::PkgbuildGenerator::generate(release);
    QVERIFY(archPackage.contains(QStringLiteral("pkgrel=1.1")));
    QVERIFY(archPackage.contains(QStringLiteral("--exclude './.PKGINFO'")));
    QVERIFY(archPackage.contains(QStringLiteral("--exclude './.INSTALL'")));
    QVERIFY(archPackage.contains(QStringLiteral("--no-same-owner")));
}

void CoreTests::parsesPkgbuildInstallPlans() {
    const auto hasPath = [](const pacsmith::InstallPlan &plan, const QString &path) {
        return std::any_of(plan.entries.cbegin(), plan.entries.cend(), [&](const auto &entry) {
            return entry.path == path && !entry.excluded;
        });
    };
    const auto excludedPath = [](const pacsmith::InstallPlan &plan, const QString &path) {
        return std::any_of(plan.entries.cbegin(), plan.entries.cend(), [&](const auto &entry) {
            return entry.path == path && entry.excluded;
        });
    };

    pacsmith::PackageRelease release;
    release.archPackageName = QStringLiteral("vendor-tool-bin");
    release.originalSourceFilename = QStringLiteral("vendor-tool-2.1-linux-x86_64.tar.gz");
    release.sourceSha256 = QString(64, QLatin1Char('a'));
    release.debian.version = QStringLiteral("2.1");
    release.debian.architecture = QStringLiteral("amd64");
    release.sourceType = pacsmith::SourcePackageType::Archive;
    release.installMapping.archiveLayout = pacsmith::ArchiveLayout::OptBundle;
    release.installMapping.optDirectory = QStringLiteral("vendor-tool");
    release.installMapping.commonPrefix = QStringLiteral("vendor-tool-2.1");
    release.installMapping.stripCommonPrefix = true;
    pacsmith::PayloadEntry tool;
    tool.path = QStringLiteral("vendor-tool-2.1/bin/tool");
    tool.type = QStringLiteral("file");
    tool.size = 12;
    release.payload.append(tool);
    pacsmith::LauncherMapping launcher;
    launcher.sourcePath = QStringLiteral("vendor-tool-2.1/bin/tool");
    launcher.commandName = QStringLiteral("tool");
    launcher.destination = QStringLiteral("/usr/bin/tool");
    release.installMapping.launchers.append(launcher);
    pacsmith::DesktopEntryConfiguration desktop;
    desktop.id = QStringLiteral("vendor-tool");
    desktop.destination = QStringLiteral("/usr/share/applications/vendor-tool.desktop");
    desktop.contents = QStringLiteral("[Desktop Entry]\nType=Application\nName=Vendor Tool\nExec=tool\n");
    release.installMapping.desktopEntries.append(desktop);
    const auto archive = pacsmith::PkgbuildGenerator::generate(release);
    const auto archivePlan = pacsmith::PkgbuildInstallPlan::parse(archive, release);
    QVERIFY2(archivePlan.warnings.isEmpty(), qPrintable(archivePlan.warnings.join(QLatin1Char('\n'))));
    QVERIFY(hasPath(archivePlan, QStringLiteral("/opt/vendor-tool/bin/tool")));
    QVERIFY(hasPath(archivePlan, QStringLiteral("/usr/bin/tool")));
    QVERIFY(hasPath(archivePlan, QStringLiteral("/usr/share/applications/vendor-tool.desktop")));

    const auto installFile = pacsmith::PkgbuildInstallPlan::parse(
        QStringLiteral("pkgname='tool'\npackage() {\n  install -Dm755 \"$srcdir/tool\" \"$pkgdir/usr/bin/tool\"\n}\n"),
        release);
    QVERIFY(hasPath(installFile, QStringLiteral("/usr/bin/tool")));

    const auto symlink = pacsmith::PkgbuildInstallPlan::parse(
        QStringLiteral("pkgname='tool'\npackage() {\n  ln -sf /opt/vendor-tool/bin/tool \"$pkgdir/usr/bin/tool\"\n}\n"),
        release);
    QVERIFY(hasPath(symlink, QStringLiteral("/usr/bin/tool")));

    pacsmith::PackageRelease deb = release;
    deb.sourceType = pacsmith::SourcePackageType::Debian;
    deb.originalSourceFilename = QStringLiteral("vendor.deb");
    deb.payload.clear();
    pacsmith::PayloadEntry listed;
    listed.path = QStringLiteral("etc/apt/sources.list.d/vendor.list");
    listed.type = QStringLiteral("file");
    deb.payload.append(listed);
    deb.payloadRules.append({QStringLiteral("etc/apt/sources.list.d/vendor.list"), true,
                             QStringLiteral("APT configuration"), false, {}});
    const auto debPkgbuild = pacsmith::PkgbuildGenerator::generate(deb);
    const auto debPlan = pacsmith::PkgbuildInstallPlan::parse(debPkgbuild, deb);
    QVERIFY(excludedPath(debPlan, QStringLiteral("/etc/apt/sources.list.d/vendor.list")));

    pacsmith::PackageRelease appImage = release;
    appImage.sourceType = pacsmith::SourcePackageType::AppImage;
    appImage.originalSourceFilename = QStringLiteral("VendorTool.AppImage");
    appImage.installMapping.appImageOffset = 4096;
    appImage.payload.clear();
    pacsmith::PayloadEntry appRun;
    appRun.path = QStringLiteral("AppRun");
    appRun.type = QStringLiteral("file");
    appImage.payload.append(appRun);
    const auto squash = pacsmith::PkgbuildGenerator::generate(appImage);
    const auto squashPlan = pacsmith::PkgbuildInstallPlan::parse(squash, appImage);
    QVERIFY(hasPath(squashPlan, QStringLiteral("/opt/vendor-tool/AppRun")));

    const auto opaque = pacsmith::PkgbuildInstallPlan::parse(
        QStringLiteral("pkgname='tool'\npackage() {\n  make DESTDIR=\"$pkgdir\" install\n}\n"),
        release);
    QVERIFY(!opaque.warnings.isEmpty());
    QVERIFY(opaque.warnings.first().contains(QStringLiteral("DESTDIR")) ||
            opaque.warnings.first().contains(QStringLiteral("build-system")));
}

void CoreTests::synchronizesIntegrationIconSource() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    pacsmith::ProjectStore store(
        std::filesystem::path(temporary.path().toUtf8().constData()) / "projects");
    pacsmith::Project project;
    project.id = QStringLiteral("icon-source-test");
    project.displayName = QStringLiteral("Icon Source Test");
    project.archPackageName = QStringLiteral("icon-source-test-bin");
    pacsmith::PackageRelease release;
    release.projectId = project.id;
    release.id = QStringLiteral("1.0-aaaaaaaaaaaa");
    release.displayName = project.displayName;
    release.archPackageName = project.archPackageName;
    release.debian.package = QStringLiteral("icon-source-test");
    release.debian.version = QStringLiteral("1.0");
    release.installMapping.icon.sourceKind = pacsmith::IconSourceKind::LocalFile;
    release.installMapping.icon.projectPath = QStringLiteral("files/integration/icon.svg");
    release.installMapping.icon.format = QStringLiteral("svg");
    release.installMapping.icon.iconName = QStringLiteral("icon-source-test");
    const QByteArray icon = QByteArrayLiteral("<svg xmlns=\"http://www.w3.org/2000/svg\"/>\n");
    release.installMapping.icon.sha256 = pacsmith::sha256Hex(icon);
    const auto iconPath = store.releasePath(release) / "files" / "integration" / "icon.svg";
    std::filesystem::create_directories(iconPath.parent_path());
    QFile file(QString::fromStdString(iconPath.string()));
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(icon), icon.size());
    file.close();
    project.releases.append(release);
    QString error;
    QVERIFY2(store.save(project, &error), qPrintable(error));
    const auto alias = store.releasePath(project.releases.first()) / "pacsmith-icon.svg";
    QVERIFY(std::filesystem::exists(alias));
    QCOMPARE(pacsmith::sha256File(alias, &error), pacsmith::sha256Hex(icon));
}

void CoreTests::parsesRpmHeadersWithoutExecutingScripts() {
    QCOMPARE(pacsmith::PkgbuildGenerator::translateArchitecture(QStringLiteral("noarch")),
             QStringLiteral("any"));
    struct Entry {
        quint32 tag;
        quint32 type;
        quint32 offset;
        quint32 count;
    };
    const auto appendBe32 = [](QByteArray &bytes, const quint32 value) {
        bytes.append(static_cast<char>((value >> 24U) & 0xffU));
        bytes.append(static_cast<char>((value >> 16U) & 0xffU));
        bytes.append(static_cast<char>((value >> 8U) & 0xffU));
        bytes.append(static_cast<char>(value & 0xffU));
    };
    QList<Entry> entries;
    QByteArray store;
    const auto addString = [&](const quint32 tag, const QString &value,
                               const quint32 type = 6U) {
        const auto offset = static_cast<quint32>(store.size());
        store.append(value.toUtf8());
        store.append('\0');
        entries.append({tag, type, offset, 1});
    };
    const auto addStrings = [&](const quint32 tag, const QStringList &values) {
        const auto offset = static_cast<quint32>(store.size());
        for (const auto &value : values) {
            store.append(value.toUtf8());
            store.append('\0');
        }
        entries.append({tag, 8, offset, static_cast<quint32>(values.size())});
    };
    const auto addIntegers = [&](const quint32 tag, const QList<quint32> &values) {
        const auto offset = static_cast<quint32>(store.size());
        for (const auto value : values) appendBe32(store, value);
        entries.append({tag, 4, offset, static_cast<quint32>(values.size())});
    };
    addString(1000, QStringLiteral("vendor-rpm"));
    addString(1001, QStringLiteral("2.4.1"));
    addString(1002, QStringLiteral("3.el9"));
    addIntegers(1003, {2});
    addString(1004, QStringLiteral("Vendor RPM application"), 9);
    addString(1005, QStringLiteral("Long RPM description"), 9);
    addString(1011, QStringLiteral("Example Vendor"));
    addString(1014, QStringLiteral("Vendor"));
    addString(1020, QStringLiteral("https://vendor.example/rpm"));
    addString(1022, QStringLiteral("x86_64"));
    addString(1024, QStringLiteral("update-desktop-database || true"));
    addStrings(1049, {QStringLiteral("gtk3"), QStringLiteral("rpmlib(CompressedFileNames)"),
                      QStringLiteral("/bin/sh")});
    addIntegers(1048, {0, 0, 1U << 10U});
    addStrings(1050, {QString{}, QStringLiteral("3.0.4-1"), QString{}});
    addStrings(5046, {QStringLiteral("optional-helper")});
    addStrings(5047, {QStringLiteral("2.0")});
    addIntegers(5048, {(1U << 2U) | (1U << 3U)});
    addStrings(5066, {QStringLiteral("update-mime-database /usr/share/mime || true")});
    addStrings(5067, {QStringLiteral("/bin/sh")});
    addIntegers(1116, {0});
    addStrings(1117, {QStringLiteral("vendor-helper")});
    addStrings(1118, {QStringLiteral("/usr/bin/")});
    addStrings(5010, {QStringLiteral("cap_net_bind_service=ep")});
    addString(1124, QStringLiteral("cpio"));
    addString(1125, QStringLiteral("zstd"));

    const auto buildHeader = [&](const QList<Entry> &headerEntries,
                                 const QByteArray &headerStore) {
        QByteArray header;
        header.append(QByteArray::fromHex("8eade801"));
        appendBe32(header, 0);
        appendBe32(header, static_cast<quint32>(headerEntries.size()));
        appendBe32(header, static_cast<quint32>(headerStore.size()));
        for (const auto &entry : headerEntries) {
            appendBe32(header, entry.tag);
            appendBe32(header, entry.type);
            appendBe32(header, entry.offset);
            appendBe32(header, entry.count);
        }
        header.append(headerStore);
        return header;
    };

    QByteArray rpm(96, '\0');
    rpm.replace(0, 4, QByteArray::fromHex("edabeedb"));
    rpm.append(buildHeader({}, {})); // Empty but valid signature header; total is 8-byte aligned.
    rpm.append(buildHeader(entries, store));
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto path = temporary.filePath(QStringLiteral("vendor.rpm"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(rpm), rpm.size());
    file.close();

    QString error;
    const auto type = pacsmith::SourceAnalyzer::detect(
        std::filesystem::path(path.toUtf8().constData()), &error);
    QVERIFY2(type.has_value(), qPrintable(error));
    QCOMPARE(*type, pacsmith::SourcePackageType::Rpm);
    const auto analyzed = pacsmith::RpmAnalyzer::analyzeHeader(
        std::filesystem::path(path.toUtf8().constData()), &error);
    QVERIFY2(analyzed.has_value(), qPrintable(error));
    QCOMPARE(analyzed->metadata.package, QStringLiteral("vendor-rpm"));
    QCOMPARE(analyzed->metadata.version, QStringLiteral("2:2.4.1-3.el9"));
    QCOMPARE(analyzed->metadata.architecture, QStringLiteral("x86_64"));
    QCOMPARE(analyzed->metadata.description,
             QStringLiteral("Vendor RPM application\nLong RPM description"));
    QCOMPARE(analyzed->dependencies.size(), 1);
    QCOMPARE(analyzed->dependencies.first().rawExpression, QStringLiteral("gtk3"));
    QCOMPARE(analyzed->dependencies.first().archPackage, QStringLiteral("gtk3"));
    QCOMPARE(analyzed->metadata.recommends,
             QStringLiteral("optional-helper (>= 2.0)"));
    QCOMPARE(analyzed->maintainerScripts.size(), 2);
    QCOMPARE(analyzed->maintainerScripts.first().name, QStringLiteral("postin"));
    QCOMPARE(analyzed->maintainerScripts.last().name, QStringLiteral("file-trigger-1"));
    QCOMPARE(analyzed->metadata.rawFields.value(QStringLiteral("File-Trigger-Interpreters")),
             QStringLiteral("/bin/sh"));
    QCOMPARE(analyzed->fileCapabilities.value(QStringLiteral("usr/bin/vendor-helper")),
             QStringLiteral("cap_net_bind_service=ep"));
    QVERIFY(!analyzed->scriptFindings.isEmpty());

    pacsmith::PackageRelease release;
    release.projectId = QStringLiteral("vendor-rpm");
    release.id = QStringLiteral("2.4.1-test");
    release.archPackageName = QStringLiteral("vendor-rpm-bin");
    release.displayName = QStringLiteral("Vendor RPM");
    release.originalSourceFilename = QStringLiteral("vendor.rpm");
    release.sourceSha256 = QString(64, QLatin1Char('a'));
    release.sourceType = pacsmith::SourcePackageType::Rpm;
    release.debian = analyzed->metadata;
    const auto pkgbuild = pacsmith::PkgbuildGenerator::generate(release);
    QVERIFY(pkgbuild.contains(QStringLiteral("pacsmith.artifact=rpm")));
    QVERIFY(pkgbuild.contains(QStringLiteral("bsdtar -xpf \"$srcdir/vendor.rpm\"")));
}

void CoreTests::inspectsArchiveIconsRepositoryEvidenceAndPrivilegedModes() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto root = std::filesystem::path(temporary.path().toUtf8().constData()) / "root";
    const auto writeFixture = [&](const std::filesystem::path &relative,
                                  const QByteArray &contents) {
        const auto path = root / relative;
        std::filesystem::create_directories(path.parent_path());
        QFile file(QString::fromUtf8(path.string().c_str()));
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(contents), contents.size());
    };
    writeFixture("usr/share/applications/vendor.desktop",
                 QByteArrayLiteral("[Desktop Entry]\nName=Vendor\nIcon=vendor\n"));
    writeFixture("usr/share/pixmaps/vendor.png",
                 QByteArray::fromBase64(
                     "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNk+A8AAQUBAScY42YAAAAASUVORK5CYII="));
    writeFixture("etc/yum.repos.d/vendor.repo",
                 QByteArrayLiteral("[vendor]\nbaseurl=https://packages.example/rpm/x86_64\n"
                                   "gpgkey=https://packages.example/key.asc\n"));
    writeFixture("etc/pki/rpm-gpg/VENDOR-KEY",
                 QByteArrayLiteral("-----BEGIN PGP PUBLIC KEY BLOCK-----\nfixture\n"
                                   "-----END PGP PUBLIC KEY BLOCK-----\n"));
    const auto sandbox = root / "usr/lib/vendor/vendor-sandbox";
    writeFixture("usr/lib/vendor/vendor-sandbox", QByteArrayLiteral("fixture"));
    std::filesystem::permissions(
        sandbox,
        std::filesystem::perms::owner_read | std::filesystem::perms::owner_write |
            std::filesystem::perms::owner_exec | std::filesystem::perms::group_read |
            std::filesystem::perms::group_exec | std::filesystem::perms::others_read |
            std::filesystem::perms::others_exec | std::filesystem::perms::set_uid,
        std::filesystem::perm_options::replace);

    const auto archive = temporary.filePath(QStringLiteral("vendor-2.0-x86_64.tar"));
    QProcess tar;
    tar.setProgram(QStringLiteral("/usr/bin/bsdtar"));
    tar.setArguments({QStringLiteral("-cf"), archive, QStringLiteral("-C"),
                      QString::fromUtf8(root.string().c_str()), QStringLiteral("usr"),
                      QStringLiteral("etc")});
    tar.start();
    QVERIFY(tar.waitForFinished(10000));
    QCOMPARE(tar.exitCode(), 0);

    QString error;
    const auto analyzed = pacsmith::SourceAnalyzer::analyze(
        std::filesystem::path(archive.toUtf8().constData()), &error);
    QVERIFY2(analyzed.has_value(), qPrintable(error));
    QVERIFY(analyzed->icon.has_value());
    QCOMPARE(analyzed->icon->sourcePath, QStringLiteral("usr/share/pixmaps/vendor.png"));
    QVERIFY(!analyzed->signingKeys.isEmpty());
    QVERIFY(analyzed->signingKeys.first().sourcePath.startsWith(
        QStringLiteral("etc/pki/rpm-gpg/VENDOR-KEY")));
    QVERIFY(!analyzed->rpmCandidates.isEmpty());
    QCOMPARE(analyzed->rpmCandidates.first().baseUrl,
             QStringLiteral("https://packages.example/rpm/x86_64"));

    const auto privileged = std::find_if(
        analyzed->payload.cbegin(), analyzed->payload.cend(), [](const auto &entry) {
            return entry.path == QStringLiteral("usr/lib/vendor/vendor-sandbox");
        });
    QVERIFY(privileged != analyzed->payload.cend());
    QVERIFY(privileged->requiresReview);
    QVERIFY(privileged->reviewReason.contains(QStringLiteral("Set-user-ID")));
    const auto repositoryRule = std::find_if(
        analyzed->payloadRules.cbegin(), analyzed->payloadRules.cend(), [](const auto &rule) {
            return rule.path == QStringLiteral("etc/yum.repos.d");
        });
    QVERIFY(repositoryRule != analyzed->payloadRules.cend());
    QVERIFY(repositoryRule->excluded);
}

void CoreTests::reviewsUnsafeArchiveSymlinksWithoutFailingImport() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto root = std::filesystem::path(temporary.path().toUtf8().constData()) / "root";
    std::filesystem::create_directories(root / "etc/cron.daily");
    std::filesystem::create_directories(root / "usr/bin");
    std::filesystem::create_directories(root / "opt/vendor/cron");
    QFile payload(QString::fromUtf8((root / "opt/vendor/cron/vendor").string().c_str()));
    QVERIFY(payload.open(QIODevice::WriteOnly));
    QCOMPARE(payload.write(QByteArrayLiteral("#!/bin/sh\nexit 0\n")), 17);
    payload.close();
    std::error_code linkError;
    std::filesystem::create_symlink("/opt/vendor/cron/vendor",
                                    root / "etc/cron.daily/vendor", linkError);
    QVERIFY2(!linkError, linkError.message().c_str());
    std::filesystem::create_symlink("/tmp/escape", root / "usr/bin/evil", linkError);
    QVERIFY2(!linkError, linkError.message().c_str());

    const auto archive = temporary.filePath(QStringLiteral("vendor-unsafe-links.tar"));
    QProcess tar;
    tar.setProgram(QStringLiteral("/usr/bin/bsdtar"));
    tar.setArguments({QStringLiteral("-cf"), archive, QStringLiteral("-C"),
                      QString::fromUtf8(root.string().c_str()), QStringLiteral("etc"),
                      QStringLiteral("usr"), QStringLiteral("opt")});
    tar.start();
    QVERIFY(tar.waitForFinished(10000));
    QCOMPARE(tar.exitCode(), 0);

    QString error;
    const auto analyzed = pacsmith::SourceAnalyzer::analyze(
        std::filesystem::path(archive.toUtf8().constData()), &error);
    QVERIFY2(analyzed.has_value(), qPrintable(error));

    const auto cron = std::find_if(
        analyzed->payload.cbegin(), analyzed->payload.cend(), [](const auto &entry) {
            return entry.path == QStringLiteral("etc/cron.daily/vendor");
        });
    QVERIFY(cron != analyzed->payload.cend());
    QCOMPARE(cron->symlinkTarget, QStringLiteral("/opt/vendor/cron/vendor"));
    QVERIFY(pacsmith::PathSafety::symlinkReviewReason(cron->path, cron->symlinkTarget).isEmpty());
    QVERIFY(cron->requiresReview);
    QVERIFY(cron->reviewReason.contains(QStringLiteral("System configuration")));

    const auto evil = std::find_if(
        analyzed->payload.cbegin(), analyzed->payload.cend(), [](const auto &entry) {
            return entry.path == QStringLiteral("usr/bin/evil");
        });
    QVERIFY(evil != analyzed->payload.cend());
    QCOMPARE(evil->symlinkTarget, QStringLiteral("/tmp/escape"));
    QVERIFY(evil->requiresReview);
    QVERIFY(evil->reviewReason.contains(QStringLiteral("/tmp/escape")));
    const auto evilRule = std::find_if(
        analyzed->payloadRules.cbegin(), analyzed->payloadRules.cend(), [](const auto &rule) {
            return rule.path == QStringLiteral("usr/bin/evil");
        });
    QVERIFY(evilRule != analyzed->payloadRules.cend());
    QVERIFY(evilRule->excluded);
    QVERIFY(!evilRule->userDecision);

    pacsmith::PackageRelease release;
    release.archPackageName = QStringLiteral("vendor");
    release.displayName = QStringLiteral("Vendor");
    release.originalSourceFilename = QStringLiteral("vendor-unsafe-links.tar");
    release.sourceSha256 = QString(64, QLatin1Char('a'));
    release.sourceType = pacsmith::SourcePackageType::Archive;
    release.payload = analyzed->payload;
    release.payloadRules = analyzed->payloadRules;
    release.installMapping = analyzed->installMapping;
    const auto pkgbuild = pacsmith::PkgbuildGenerator::generate(release);
    QVERIFY(pkgbuild.contains(QStringLiteral("rm -rf -- \"${pkgdir}/usr/bin/evil\"")));
}

void CoreTests::mapsArchiveDesktopExecToUsrBinCommand() {
    pacsmith::InstallMapping mapping;
    mapping.archiveLayout = pacsmith::ArchiveLayout::OptBundle;
    mapping.optDirectory = QStringLiteral("letos");
    mapping.commonPrefix = QStringLiteral("Letos");
    mapping.stripCommonPrefix = true;
    pacsmith::DesktopEntryConfiguration desktop;
    desktop.enabled = true;
    desktop.contents = QStringLiteral(
        "[Desktop Entry]\nType=Application\nName=Letos\nExec=letos %f\nIcon=letos\n");
    mapping.desktopEntries.append(desktop);
    QList<pacsmith::PayloadEntry> payload;
    payload.append({QStringLiteral("Letos/letos"), QStringLiteral("file"), {}, 211976, false, {},
                    {}, {}, false, false});
    payload.append({QStringLiteral("Letos/letos.desktop"), QStringLiteral("file"), {}, 80, false, {},
                    {}, {}, false, false});
    payload.append({QStringLiteral("Letos/imageformats/libqjpeg.so"), QStringLiteral("file"), {},
                    4096, false, {}, {}, {}, false, true});
    QVERIFY(pacsmith::SourceAnalyzer::inferArchiveLaunchers(
        mapping, payload, QStringLiteral("letos")));
    QCOMPARE(mapping.launchers.size(), 1);
    QCOMPARE(mapping.launchers.first().sourcePath, QStringLiteral("Letos/letos"));
    QVERIFY(mapping.launchers.first().enabled);
    QVERIFY(!mapping.launchers.first().missing);
    QCOMPARE(mapping.launchers.first().commandName, QStringLiteral("letos"));
    QCOMPARE(mapping.launchers.first().destination, QStringLiteral("/usr/bin/letos"));
    QCOMPARE(mapping.binarySourcePath, QStringLiteral("Letos/letos"));
    QCOMPARE(mapping.binaryDestination, QStringLiteral("/usr/bin/letos"));

    const auto executable = QStandardPaths::findExecutable(QStringLiteral("true"));
    QVERIFY(!executable.isEmpty());
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto root = std::filesystem::path(temporary.path().toUtf8().constData()) / "root";
    const auto binary = root / "Letos" / "letos";
    std::filesystem::create_directories(binary.parent_path());
    QVERIFY(QFile::copy(executable, QString::fromUtf8(binary.string().c_str())));
    QVERIFY(QFile::setPermissions(
        QString::fromUtf8(binary.string().c_str()),
        QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ReadGroup |
            QFileDevice::ReadOther));
    QFile desktopFile(QString::fromUtf8((root / "Letos" / "letos.desktop").string().c_str()));
    QVERIFY(desktopFile.open(QIODevice::WriteOnly));
    const auto desktopContents = QByteArrayLiteral(
        "[Desktop Entry]\nType=Application\nName=Letos\nExec=letos %f\nIcon=letos\n");
    QCOMPARE(desktopFile.write(desktopContents), desktopContents.size());
    desktopFile.close();

    const auto archive = temporary.filePath(QStringLiteral("letos-4.0.3-linux-x64.tar"));
    QProcess tar;
    tar.setProgram(QStringLiteral("/usr/bin/bsdtar"));
    tar.setArguments({QStringLiteral("-cf"), archive, QStringLiteral("-C"),
                      QString::fromUtf8(root.string().c_str()), QStringLiteral("Letos")});
    tar.start();
    QVERIFY(tar.waitForFinished(10000));
    QCOMPARE(tar.exitCode(), 0);

    QString error;
    const auto analyzed = pacsmith::SourceAnalyzer::analyze(
        std::filesystem::path(archive.toUtf8().constData()), &error);
    QVERIFY2(analyzed.has_value(), qPrintable(error));
    QCOMPARE(analyzed->type, pacsmith::SourcePackageType::Archive);
    QCOMPARE(analyzed->installMapping.archiveLayout, pacsmith::ArchiveLayout::OptBundle);
    QCOMPARE(analyzed->installMapping.commonPrefix, QStringLiteral("Letos"));
    const auto letos = std::find_if(
        analyzed->payload.cbegin(), analyzed->payload.cend(), [](const auto &entry) {
            return entry.path == QStringLiteral("Letos/letos");
        });
    QVERIFY(letos != analyzed->payload.cend());
    QVERIFY(letos->executable);
    QCOMPARE(analyzed->installMapping.launchers.size(), 1);
    QCOMPARE(analyzed->installMapping.launchers.first().sourcePath,
             QStringLiteral("Letos/letos"));
    QVERIFY(analyzed->installMapping.launchers.first().enabled);
    QVERIFY(!analyzed->installMapping.launchers.first().missing);
    QCOMPARE(analyzed->installMapping.launchers.first().commandName, QStringLiteral("letos"));
    QCOMPARE(analyzed->installMapping.launchers.first().destination,
             QStringLiteral("/usr/bin/letos"));
    pacsmith::PackageRelease release;
    release.sourceType = pacsmith::SourcePackageType::Archive;
    release.archPackageName = QStringLiteral("letos-bin");
    release.originalSourceFilename = QFileInfo(archive).fileName();
    release.installMapping = analyzed->installMapping;
    const auto pkgbuild = pacsmith::PkgbuildGenerator::generate(release);
    QVERIFY(pkgbuild.contains(QStringLiteral("/usr/bin/letos")));
}

void CoreTests::flagsMissingArchiveDesktopCommandForReview() {
    pacsmith::InstallMapping mapping;
    mapping.archiveLayout = pacsmith::ArchiveLayout::OptBundle;
    mapping.commonPrefix = QStringLiteral("Letos");
    pacsmith::DesktopEntryConfiguration desktop;
    desktop.enabled = true;
    desktop.contents = QStringLiteral(
        "[Desktop Entry]\nType=Application\nName=Letos\nExec=missingcmd %f\n");
    mapping.desktopEntries.append(desktop);
    QList<pacsmith::PayloadEntry> payload;
    payload.append({QStringLiteral("Letos/other"), QStringLiteral("file"), {}, 128, false, {}, {},
                    {}, false, true});
    QVERIFY(pacsmith::SourceAnalyzer::inferArchiveLaunchers(
        mapping, payload, QStringLiteral("letos")));
    QVERIFY(!mapping.launchers.isEmpty());
    const auto missing = std::find_if(
        mapping.launchers.cbegin(), mapping.launchers.cend(), [](const auto &launcher) {
            return launcher.commandName == QStringLiteral("missingcmd");
        });
    QVERIFY(missing != mapping.launchers.cend());
    QVERIFY(missing->enabled);
    QVERIFY(missing->missing);
    QVERIFY(missing->sourcePath.isEmpty());
    QCOMPARE(missing->destination, QStringLiteral("/usr/bin/missingcmd"));
}

void CoreTests::detectsStandaloneElfWithoutExecutingIt() {
    const auto executable = QStandardPaths::findExecutable(QStringLiteral("true"));
    QVERIFY(!executable.isEmpty());
    QString error;
    const auto detected = pacsmith::SourceAnalyzer::detect(
        std::filesystem::path(executable.toUtf8().constData()), &error);
    QVERIFY2(detected.has_value(), qPrintable(error));
    QCOMPARE(*detected, pacsmith::SourcePackageType::ElfBinary);
    const auto analyzed = pacsmith::SourceAnalyzer::analyze(
        std::filesystem::path(executable.toUtf8().constData()), &error);
    QVERIFY2(analyzed.has_value(), qPrintable(error));
    QCOMPARE(analyzed->type, pacsmith::SourcePackageType::ElfBinary);
    QVERIFY(!analyzed->metadata.package.isEmpty());
    QVERIFY(analyzed->installMapping.binaryDestination.startsWith(QStringLiteral("/usr/bin/")));
    QCOMPARE(analyzed->installMapping.launchers.size(), 1);
    QCOMPARE(analyzed->installMapping.launchers.first().sourcePath,
             QFileInfo(executable).fileName());
    QCOMPARE(analyzed->installMapping.launchers.first().destination,
             analyzed->installMapping.binaryDestination);

    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto versionedPath = temporary.filePath(
        QStringLiteral("vendorctl-2.0.0-linux-x86_64"));
    QVERIFY(QFile::copy(executable, versionedPath));
    const auto versioned = pacsmith::SourceAnalyzer::analyze(
        std::filesystem::path(versionedPath.toUtf8().constData()), &error);
    QVERIFY2(versioned.has_value(), qPrintable(error));
    QCOMPARE(versioned->metadata.package, QStringLiteral("vendorctl"));
    QCOMPARE(versioned->metadata.version, QStringLiteral("2.0.0"));
}

void CoreTests::rejectsAppImagesBeforeElfDetection() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto path = temporary.filePath(QStringLiteral("vendor.AppImage"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QByteArray header(12, '\0');
    header.replace(0, 4, QByteArrayLiteral("\x7f" "ELF"));
    header.replace(8, 3, QByteArrayLiteral("AI\x02"));
    QCOMPARE(file.write(header), header.size());
    file.close();

    QString error;
    const auto detected = pacsmith::SourceAnalyzer::detect(
        std::filesystem::path(path.toUtf8().constData()), &error);
    QVERIFY2(detected.has_value(), qPrintable(error));
    QCOMPARE(*detected, pacsmith::SourcePackageType::AppImage);
    const auto analyzed = pacsmith::SourceAnalyzer::analyze(
        std::filesystem::path(path.toUtf8().constData()), &error);
    QVERIFY(!analyzed.has_value());
    QVERIFY(error.contains(QStringLiteral("AppImage")));
    QVERIFY(error.contains(QStringLiteral("SquashFS")) ||
            error.contains(QStringLiteral("squashfs-tools")));
}

void CoreTests::acceptsStandardExternalAppImageRuntimeSymlinks() {
    const auto mksquashfs = QStandardPaths::findExecutable(QStringLiteral("mksquashfs"));
    const auto unsquashfs = QStandardPaths::findExecutable(QStringLiteral("unsquashfs"));
    if (mksquashfs.isEmpty() || unsquashfs.isEmpty()) {
        QSKIP("squashfs-tools is required for the AppImage analyzer regression test");
    }

    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto appDir = temporary.filePath(QStringLiteral("AppDir"));
    QVERIFY(QDir().mkpath(appDir + QStringLiteral("/runtime/compat/usr/bin")));
    QFile appRun(appDir + QStringLiteral("/AppRun"));
    QVERIFY(appRun.open(QIODevice::WriteOnly));
    QCOMPARE(appRun.write(QByteArrayLiteral("#!/bin/sh\nexit 0\n")), 17);
    appRun.close();
    QVERIFY(QFile::setPermissions(
        appRun.fileName(), QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                               QFileDevice::ExeOwner | QFileDevice::ReadGroup |
                               QFileDevice::ExeGroup | QFileDevice::ReadOther |
                               QFileDevice::ExeOther));
    const auto writeAppDirFile = [&](const QString &relative, const QByteArray &contents,
                                     const bool executable = false) {
        const auto fileName = appDir + QLatin1Char('/') + relative;
        QVERIFY(QDir().mkpath(QFileInfo(fileName).path()));
        QFile file(fileName);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(contents), contents.size());
        file.close();
        if (executable) {
            QVERIFY(QFile::setPermissions(
                fileName, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                              QFileDevice::ExeOwner | QFileDevice::ReadGroup |
                              QFileDevice::ExeGroup | QFileDevice::ReadOther |
                              QFileDevice::ExeOther));
        }
    };
    writeAppDirFile(
        QStringLiteral("vendor-tool.desktop"),
        QByteArrayLiteral("[Desktop Entry]\nType=Application\nName=Vendor Tool\n"
                          "Exec=/usr/bin/python3 %U\nIcon=vendor-tool\n"));
    writeAppDirFile(
        QStringLiteral("usr/share/applications/vendor-tool-open.desktop"),
        QByteArrayLiteral("[Desktop Entry]\nType=Application\nName=Open in Vendor Tool\n"
                          "Exec=Vendor-Tool --open %U\nIcon=vendor-tool\n"));
    writeAppDirFile(
        QStringLiteral("usr/share/applications/python3.10.desktop"),
        QByteArrayLiteral("[Desktop Entry]\nType=Application\nName=Python\n"
                          "Exec=python3.10\nNoDisplay=true\n"));
    writeAppDirFile(
        QStringLiteral("usr/lib/settings/gtk-modules/canberra.desktop"),
        QByteArrayLiteral("[GTK Module]\nName=canberra\n"));
    writeAppDirFile(QStringLiteral("usr/bin/python3.10"),
                    QByteArrayLiteral("#!/bin/sh\nexit 0\n"), true);
    writeAppDirFile(QStringLiteral("etc/vendor-tool.conf"),
                    QByteArrayLiteral("private AppDir configuration\n"));

    std::error_code linkError;
    const auto compatibilityLink = std::filesystem::path(
        (appDir + QStringLiteral("/runtime/compat/usr/bin/env")).toUtf8().constData());
    std::filesystem::create_symlink(std::filesystem::path("/usr/bin/env"),
                                    compatibilityLink, linkError);
    QVERIFY2(!linkError, linkError.message().c_str());

    const auto squashfs = temporary.filePath(QStringLiteral("payload.squashfs"));
    QProcess pack;
    pack.setProgram(mksquashfs);
    pack.setArguments({appDir, squashfs, QStringLiteral("-noappend"),
                       QStringLiteral("-processors"), QStringLiteral("1"),
                       QStringLiteral("-quiet")});
    pack.start();
    QVERIFY(pack.waitForFinished(30000));
    QCOMPARE(pack.exitStatus(), QProcess::NormalExit);
    QCOMPARE(pack.exitCode(), 0);

    QFile payload(squashfs);
    QVERIFY(payload.open(QIODevice::ReadOnly));
    const auto appImagePath = temporary.filePath(
        QStringLiteral("VendorTool-1.0-x86_64.AppImage"));
    QFile appImage(appImagePath);
    QVERIFY(appImage.open(QIODevice::WriteOnly));
    QByteArray header(4096, '\0');
    header.replace(0, 4, QByteArrayLiteral("\x7f" "ELF"));
    header.replace(8, 3, QByteArrayLiteral("AI\x02"));
    QCOMPARE(appImage.write(header), header.size());
    const auto squashfsContents = payload.readAll();
    QCOMPARE(appImage.write(squashfsContents), squashfsContents.size());
    appImage.close();

    QString error;
    const auto analyzed = pacsmith::SourceAnalyzer::analyze(
        std::filesystem::path(appImagePath.toUtf8().constData()), &error);
    QVERIFY2(analyzed.has_value(), qPrintable(error));
    QCOMPARE(analyzed->type, pacsmith::SourcePackageType::AppImage);
    const auto link = std::find_if(
        analyzed->payload.cbegin(), analyzed->payload.cend(), [](const auto &entry) {
            return entry.path == QStringLiteral("runtime/compat/usr/bin/env");
        });
    QVERIFY(link != analyzed->payload.cend());
    QCOMPARE(link->type, QStringLiteral("symlink"));
    QCOMPARE(link->symlinkTarget, QStringLiteral("/usr/bin/env"));
    QCOMPARE(analyzed->installMapping.launchers.size(), 1);
    const auto &launcher = analyzed->installMapping.launchers.first();
    QCOMPARE(launcher.sourcePath, QStringLiteral("AppRun"));
    QVERIFY(launcher.enabled);
    QCOMPARE(launcher.commandName, QStringLiteral("vendor-tool"));
    QCOMPARE(launcher.destination, QStringLiteral("/usr/bin/vendor-tool"));
    QCOMPARE(analyzed->installMapping.desktopEntries.size(), 2);
    const auto primaryDesktop = std::find_if(
        analyzed->installMapping.desktopEntries.cbegin(),
        analyzed->installMapping.desktopEntries.cend(), [](const auto &desktop) {
            return desktop.sourcePath == QStringLiteral("vendor-tool.desktop");
        });
    QVERIFY(primaryDesktop != analyzed->installMapping.desktopEntries.cend());
    QVERIFY(primaryDesktop->enabled);
    const auto secondaryDesktop = std::find_if(
        analyzed->installMapping.desktopEntries.cbegin(),
        analyzed->installMapping.desktopEntries.cend(), [](const auto &desktop) {
            return desktop.sourcePath ==
                   QStringLiteral("usr/share/applications/vendor-tool-open.desktop");
        });
    QVERIFY(secondaryDesktop != analyzed->installMapping.desktopEntries.cend());
    QVERIFY(!secondaryDesktop->enabled);
    QVERIFY(std::none_of(analyzed->installMapping.desktopEntries.cbegin(),
                         analyzed->installMapping.desktopEntries.cend(), [](const auto &desktop) {
        return desktop.sourcePath.contains(QStringLiteral("python3.10")) ||
               desktop.sourcePath.contains(QStringLiteral("canberra"));
    }));
    QVERIFY(analyzed->payloadRules.isEmpty());
    QString inspectError;
    const auto inspectedAppRun = pacsmith::PayloadInspector::inspectFile(
        std::filesystem::path(appImagePath.toUtf8().constData()),
        QStringLiteral("AppRun"), &inspectError);
    QVERIFY2(inspectedAppRun.has_value(), qPrintable(inspectError));
    QVERIFY(inspectedAppRun->textPreview.contains(QStringLiteral("#!/bin/sh")));
    QVERIFY(!inspectedAppRun->contentSha256.isEmpty());
    QVERIFY(std::none_of(analyzed->payload.cbegin(), analyzed->payload.cend(),
                         [](const auto &entry) { return entry.requiresReview; }));
    QVERIFY(analyzed->installMapping.appRun.present);
    QVERIFY(analyzed->installMapping.appRun.script);
    QCOMPARE(analyzed->installMapping.appRun.contents, QStringLiteral("#!/bin/sh\nexit 0\n"));
    QVERIFY(analyzed->installMapping.appRun.requiresReview());
}

void CoreTests::flagsAppRunFilenameDispatchForReview() {
    const auto mksquashfs = QStandardPaths::findExecutable(QStringLiteral("mksquashfs"));
    const auto unsquashfs = QStandardPaths::findExecutable(QStringLiteral("unsquashfs"));
    if (mksquashfs.isEmpty() || unsquashfs.isEmpty()) {
        QSKIP("squashfs-tools is required for the AppImage analyzer regression test");
    }

    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto appDir = temporary.filePath(QStringLiteral("AppDir"));
    QVERIFY(QDir().mkpath(appDir));
    QFile appRun(appDir + QStringLiteral("/AppRun"));
    QVERIFY(appRun.open(QIODevice::WriteOnly));
    const auto vendor = QByteArrayLiteral(
        "#!/bin/bash\n"
        "HERE=\"$(dirname \"$(readlink -f \"${0}\")\")\"\n"
        "if [ ! -z \"$APPIMAGE\" ]; then\n"
        "  BINARY_NAME=$(basename \"$ARGV0\")\n"
        "else\n"
        "  BINARY_NAME=$(basename \"$0\")\n"
        "fi\n"
        "if [ -x \"$HERE/$BINARY_NAME\" ]; then\n"
        "  MAIN=\"$HERE/$BINARY_NAME\"\n"
        "else\n"
        "  MAIN=\"$HERE/freac\"\n"
        "fi\n"
        "export LD_LIBRARY_PATH=\"$HERE\"\n"
        "exec \"$MAIN\" \"$@\"\n");
    QCOMPARE(appRun.write(vendor), vendor.size());
    appRun.close();
    QVERIFY(QFile::setPermissions(
        appRun.fileName(), QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                               QFileDevice::ExeOwner | QFileDevice::ReadGroup |
                               QFileDevice::ExeGroup | QFileDevice::ReadOther |
                               QFileDevice::ExeOther));

    const auto squashfs = temporary.filePath(QStringLiteral("payload.squashfs"));
    QProcess pack;
    pack.setProgram(mksquashfs);
    pack.setArguments({appDir, squashfs, QStringLiteral("-noappend"),
                       QStringLiteral("-processors"), QStringLiteral("1"),
                       QStringLiteral("-quiet")});
    pack.start();
    QVERIFY(pack.waitForFinished(30000));
    QCOMPARE(pack.exitStatus(), QProcess::NormalExit);
    QCOMPARE(pack.exitCode(), 0);

    QFile payload(squashfs);
    QVERIFY(payload.open(QIODevice::ReadOnly));
    const auto appImagePath = temporary.filePath(QStringLiteral("freac-1.1.7-x86_64.AppImage"));
    QFile appImage(appImagePath);
    QVERIFY(appImage.open(QIODevice::WriteOnly));
    QByteArray header(4096, '\0');
    header.replace(0, 4, QByteArrayLiteral("\x7f" "ELF"));
    header.replace(8, 3, QByteArrayLiteral("AI\x02"));
    QCOMPARE(appImage.write(header), header.size());
    const auto squashfsContents = payload.readAll();
    QCOMPARE(appImage.write(squashfsContents), squashfsContents.size());
    appImage.close();

    QString error;
    const auto analyzed = pacsmith::SourceAnalyzer::analyze(
        std::filesystem::path(appImagePath.toUtf8().constData()), &error);
    QVERIFY2(analyzed.has_value(), qPrintable(error));
    QCOMPARE(analyzed->type, pacsmith::SourcePackageType::AppImage);
    QVERIFY(analyzed->installMapping.appRun.present);
    QVERIFY(analyzed->installMapping.appRun.script);
    QVERIFY(analyzed->installMapping.appRun.contents.contains(QStringLiteral("BINARY_NAME")));
    QVERIFY(analyzed->installMapping.appRun.requiresReview());
    QVERIFY(analyzed->installMapping.appRun.reviewReason.contains(QStringLiteral("APPIMAGE")));
}

void CoreTests::selectsGitHubReleaseAssets() {
    const auto asset = [](const qint64 id, const QString &name, const QString &digest = {}) {
        return QJsonObject{{QStringLiteral("id"), id}, {QStringLiteral("name"), name},
                           {QStringLiteral("browser_download_url"),
                            QStringLiteral("https://github.com/vendor/tool/releases/download/v2.0.0/%1").arg(name)},
                           {QStringLiteral("digest"), digest}};
    };
    const auto releaseObject = [&](const qint64 id, const QString &tag, const bool prerelease,
                                   const bool draft, const QJsonArray &assets) {
        return QJsonObject{{QStringLiteral("id"), id}, {QStringLiteral("tag_name"), tag},
                           {QStringLiteral("prerelease"), prerelease},
                           {QStringLiteral("draft"), draft}, {QStringLiteral("assets"), assets}};
    };
    const auto stableAsset = QStringLiteral("tool-2.0.0-linux-x86_64.tar.gz");
    const auto prereleaseAsset = QStringLiteral("tool-3.0.0-rc1-linux-x86_64.tar.gz");
    const QJsonArray releases{
        releaseObject(30, QStringLiteral("v3.0.0-rc1"), true, false,
                      QJsonArray{asset(301, prereleaseAsset)}),
        releaseObject(25, QStringLiteral("v2.5.0"), false, true,
                      QJsonArray{asset(251, QStringLiteral("tool-2.5.0-linux-x86_64.tar.gz"))}),
        releaseObject(20, QStringLiteral("v2.0.0"), false, false,
                      QJsonArray{asset(201, stableAsset,
                                       QStringLiteral("sha256:%1").arg(QString(64, QLatin1Char('b')))),
                                 asset(202, QStringLiteral("tool-2.0.0-linux-aarch64.tar.gz"))})};
    pacsmith::PackageRelease current;
    current.debian.version = QStringLiteral("1.0.0");
    current.update.githubAssetRegex = QStringLiteral("tool-.*-linux-x86_64\\.tar\\.gz");
    QString error;
    const auto selected = pacsmith::GitHubUpdateService::selectRelease(releases, current, &error);
    QVERIFY2(selected.success, qPrintable(error));
    QCOMPARE(selected.tag, QStringLiteral("v2.0.0"));
    QCOMPARE(selected.filename, stableAsset);
    QCOMPARE(selected.sha256, QString(64, QLatin1Char('b')));
    QVERIFY(selected.updateAvailable);

    const QJsonArray prereleaseOnly{
        releaseObject(30, QStringLiteral("v3.0.0-rc1"), true, false,
                      QJsonArray{asset(301, prereleaseAsset)})};
    const auto automaticPrerelease = pacsmith::GitHubUpdateService::selectRelease(
        prereleaseOnly, current, &error);
    QVERIFY2(automaticPrerelease.success, qPrintable(error));
    QCOMPARE(automaticPrerelease.tag, QStringLiteral("v3.0.0-rc1"));
    QVERIFY(automaticPrerelease.prerelease);
    QVERIFY(automaticPrerelease.message.contains(QStringLiteral("No matching stable")));

    auto previewTracking = current;
    previewTracking.update.githubIncludePrereleases = true;
    const auto explicitlySelectedPrerelease = pacsmith::GitHubUpdateService::selectRelease(
        releases, previewTracking, &error);
    QVERIFY2(explicitlySelectedPrerelease.success, qPrintable(error));
    QCOMPARE(explicitlySelectedPrerelease.tag, QStringLiteral("v3.0.0-rc1"));
    QVERIFY(explicitlySelectedPrerelease.prerelease);

    auto prereleaseInstall = current;
    prereleaseInstall.debian.version = QStringLiteral("4.0.0_rc1");
    prereleaseInstall.acquisition.githubReleaseId = 40;
    prereleaseInstall.acquisition.githubTag = QStringLiteral("v4.0.0-rc1");
    prereleaseInstall.acquisition.githubPrerelease = true;
    const QJsonArray promotedStable{
        releaseObject(41, QStringLiteral("v4.0.0"), false, false,
                      QJsonArray{asset(411, QStringLiteral("tool-4.0.0-linux-x86_64.tar.gz"))})};
    const auto stablePromotion = pacsmith::GitHubUpdateService::selectRelease(
        promotedStable, prereleaseInstall, &error);
    QVERIFY2(stablePromotion.success, qPrintable(error));
    QVERIFY(!stablePromotion.prerelease);
    QVERIFY(stablePromotion.updateAvailable);

    const auto pinnedPrerelease = pacsmith::GitHubUpdateService::selectRelease(
        releases, current, &error, QStringLiteral("v3.0.0-rc1"));
    QVERIFY2(pinnedPrerelease.success, qPrintable(error));
    QCOMPARE(pinnedPrerelease.filename, prereleaseAsset);
    QVERIFY(pinnedPrerelease.prerelease);

    const QJsonArray ambiguous{releaseObject(
        40, QStringLiteral("v4.0.0"), false, false,
        QJsonArray{asset(401, QStringLiteral("tool-a-linux-x86_64.tar.gz")),
                   asset(402, QStringLiteral("tool-b-linux-x86_64.tar.gz"))})};
    const auto blocked = pacsmith::GitHubUpdateService::selectRelease(ambiguous, current, &error);
    QVERIFY(!blocked.success);
    QVERIFY(blocked.message.contains(QStringLiteral("exactly one")));

    current.update.githubAssetRegex = QStringLiteral("tool-2\\.0\\.0_amd64\\.deb\\.sig");
    const QJsonArray sidecarRelease{releaseObject(
        50, QStringLiteral("v2.0.0"), false, false,
        QJsonArray{asset(501, QStringLiteral("tool-2.0.0_amd64.deb")),
                   asset(502, QStringLiteral("tool-2.0.0_amd64.deb.sig"))})};
    const auto sidecar = pacsmith::GitHubUpdateService::selectRelease(
        sidecarRelease, current, &error);
    QVERIFY(!sidecar.success);
    QVERIFY(sidecar.message.contains(QStringLiteral("sidecar")));

    current.update.githubAssetRegex = QStringLiteral(".*");
    const QJsonArray manifestFirstRelease{releaseObject(
        60, QStringLiteral("v2.0.0"), false, false,
        QJsonArray{asset(601, QStringLiteral("manifest.json")),
                   asset(602, QStringLiteral("tool-2.0.0.x86_64.rpm")),
                   asset(603, QStringLiteral("tool_2.0.0_amd64.deb")),
                   asset(604, QStringLiteral("tool_2.0.0_amd64.deb.sig"))})};
    const auto manifestFirst = pacsmith::GitHubUpdateService::selectRelease(
        manifestFirstRelease, current, &error);
    QVERIFY(!manifestFirst.success);
    QVERIFY(manifestFirst.message.contains(QStringLiteral("exactly one")));
    QCOMPARE(manifestFirst.availableAssets.size(), 4);
    QCOMPARE(manifestFirst.matchingAssets.size(), 2);
    QVERIFY(manifestFirst.matchingAssets.contains(QStringLiteral("tool-2.0.0.x86_64.rpm")));
    QVERIFY(manifestFirst.matchingAssets.contains(QStringLiteral("tool_2.0.0_amd64.deb")));
}

void CoreTests::sanitizesPackageNames_data() {
    QTest::addColumn<QString>("input");
    QTest::addColumn<QString>("expected");
    QTest::newRow("spaces") << QStringLiteral("Some Vendor Tool") << QStringLiteral("some-vendor-tool");
    QTest::newRow("punctuation") << QStringLiteral("--Hello!!World--") << QStringLiteral("hello-world");
    QTest::newRow("valid") << QStringLiteral("libfoo++_bin") << QStringLiteral("libfoo++_bin");
    QTest::newRow("empty") << QStringLiteral("!!!") << QStringLiteral("vendor-package-bin");
}

void CoreTests::sanitizesPackageNames() {
    QFETCH(QString, input);
    QFETCH(QString, expected);
    QCOMPARE(pacsmith::PkgbuildGenerator::sanitizePackageName(input), expected);
}

void CoreTests::translatesVersions_data() {
    QTest::addColumn<QString>("input");
    QTest::addColumn<QString>("expected");
    QTest::newRow("debian revision") << QStringLiteral("1.2.3-4") << QStringLiteral("1.2.3");
    QTest::newRow("epoch") << QStringLiteral("2:4.5.0-1") << QStringLiteral("4.5.0");
    QTest::newRow("prerelease") << QStringLiteral("1.0~beta1-2") << QStringLiteral("1.0.beta1");
}

void CoreTests::translatesVersions() {
    QFETCH(QString, input);
    QFETCH(QString, expected);
    QCOMPARE(pacsmith::PkgbuildGenerator::translateVersion(input), expected);
}

void CoreTests::validatesArchivePaths() {
    QCOMPARE(pacsmith::PathSafety::normalizedArchivePath(QStringLiteral("./usr/bin/tool")),
             std::optional<QString>(QStringLiteral("usr/bin/tool")));
    QVERIFY(!pacsmith::PathSafety::normalizedArchivePath(QStringLiteral("../../etc/passwd")));
    QVERIFY(!pacsmith::PathSafety::normalizedArchivePath(QStringLiteral("/etc/passwd")));
    QVERIFY(pacsmith::PathSafety::safeSymlinkTarget(QStringLiteral("usr/bin/tool"), QStringLiteral("../lib/tool")));
    QVERIFY(!pacsmith::PathSafety::safeSymlinkTarget(QStringLiteral("usr/bin/tool"), QStringLiteral("../../../etc/passwd")));
    QVERIFY(!pacsmith::PathSafety::safeSymlinkTarget(QStringLiteral("usr/bin/tool"), QStringLiteral("/tmp/escape")));
    QVERIFY(pacsmith::PathSafety::safePackageSymlinkTarget(
        QStringLiteral("etc/cron.daily/brave-browser"),
        QStringLiteral("/opt/brave.com/brave/cron/brave-browser")));
    QVERIFY(pacsmith::PathSafety::safePackageSymlinkTarget(
        QStringLiteral("usr/bin/brave-browser"),
        QStringLiteral("/opt/brave.com/brave/brave-browser")));
    QVERIFY(!pacsmith::PathSafety::safePackageSymlinkTarget(
        QStringLiteral("usr/bin/tool"), QStringLiteral("/tmp/escape")));
    QVERIFY(!pacsmith::PathSafety::safePackageSymlinkTarget(
        QStringLiteral("usr/bin/tool"), QStringLiteral("/etc/passwd")));
    QVERIFY(!pacsmith::PathSafety::safePackageSymlinkTarget(
        QStringLiteral("usr/bin/tool"), QStringLiteral("/opt/../tmp/escape")));
    QVERIFY(pacsmith::PathSafety::symlinkReviewReason(
                QStringLiteral("etc/cron.daily/brave-browser"),
                QStringLiteral("/opt/brave.com/brave/cron/brave-browser"))
                .isEmpty());
    QVERIFY(pacsmith::PathSafety::symlinkReviewReason(
                QStringLiteral("usr/bin/tool"), QStringLiteral("/tmp/escape"))
                .contains(QStringLiteral("/tmp/escape")));
    QVERIFY(pacsmith::PathSafety::safeAppImageSymlinkTarget(
        QStringLiteral("runtime/compat/usr/bin/env"), QStringLiteral("/usr/bin/env")));
    QVERIFY(pacsmith::PathSafety::safeAppImageSymlinkTarget(
        QStringLiteral("runtime/compat/usr/lib/libc.so.6"),
        QStringLiteral("/usr/lib/libc.so.6")));
    QVERIFY(!pacsmith::PathSafety::safeAppImageSymlinkTarget(
        QStringLiteral("runtime/compat/tmp/state"), QStringLiteral("/tmp/state")));
    QVERIFY(!pacsmith::PathSafety::safeAppImageSymlinkTarget(
        QStringLiteral("runtime/compat/etc/passwd"), QStringLiteral("/etc/passwd")));
    QVERIFY(!pacsmith::PathSafety::safeAppImageSymlinkTarget(
        QStringLiteral("runtime/compat/usr/bin/disguised"),
        QStringLiteral("/usr/bin/../../tmp/state")));
    QVERIFY(pacsmith::PathSafety::isDebianSpecificPath(QStringLiteral("etc/apt")));
    QVERIFY(pacsmith::PathSafety::isDebianSpecificPath(QStringLiteral("usr/share/keyrings")));
    QVERIFY(pacsmith::PathSafety::isDebianSpecificPath(
        QStringLiteral("usr/share/lintian/overrides/mongodb-compass")));
    QVERIFY(pacsmith::PathSafety::reviewReason(
                QStringLiteral("usr/share/lintian/overrides/mongodb-compass"))
                .contains(QStringLiteral("Lintian")));
}

void CoreTests::preservesUserMappingOverrides() {
    auto dependencies = pacsmith::DependencyParser::parse(QStringLiteral("libgtk-3-0"));
    dependencies.first().archPackage = QStringLiteral("my-gtk-provider");
    dependencies.first().status = pacsmith::MappingStatus::Resolved;
    dependencies.first().userOverride = true;
    QVERIFY(!pacsmith::DependencyParser::applyVerifiedMappings(
        dependencies, {{QStringLiteral("libgtk-3-0"), QStringLiteral("gtk3")}}));
    QCOMPARE(dependencies.first().archPackage, QStringLiteral("my-gtk-provider"));
    QCOMPARE(dependencies.first().mappingSource, QString{});
}

void CoreTests::extractsPayloadRpmRepositoryEvidence() {
    const QString script = QStringLiteral(R"(#!/bin/sh
REPOCONFIG="https://packagecloud.io/slacktechnologies/slack/fedora/21"
DEFAULT_ARCH="x86_64"
cat > /etc/yum.repos.d/slack.repo <<EOF
[slack]
baseurl=$REPOCONFIG/$DEFAULT_ARCH
gpgkey=https://packagecloud.io/gpg.key
EOF
)");
    const auto evidence = pacsmith::ScriptEvidenceAnalyzer::analyze(
        {{QStringLiteral("payload/etc/cron.daily/slack"), script, {}}});
    QCOMPARE(evidence.rpmCandidates.size(), 1);
    QCOMPARE(evidence.rpmCandidates.first().baseUrl,
             QStringLiteral("https://packagecloud.io/slacktechnologies/slack/fedora/21/x86_64"));
    QCOMPARE(evidence.rpmCandidates.first().architecture, QStringLiteral("x86_64"));
    QCOMPARE(evidence.rpmCandidates.first().keyUrls,
             QStringList{QStringLiteral("https://packagecloud.io/gpg.key")});
    QCOMPARE(evidence.rpmCandidates.first().sourcePath,
             QStringLiteral("etc/cron.daily/slack"));

    pacsmith::UpdateConfiguration update;
    update.strategy = pacsmith::UpdateStrategy::RpmRepository;
    update.rpmArchitecture = QStringLiteral("x86_64");
    update.rpmPackageName = QStringLiteral("slack");
    update.rpmCandidates = evidence.rpmCandidates;
    const auto restored = pacsmith::UpdateConfiguration::fromJson(update.toJson());
    QCOMPARE(restored.strategy, pacsmith::UpdateStrategy::RpmRepository);
    QCOMPARE(restored.rpmArchitecture, QStringLiteral("x86_64"));
    QCOMPARE(restored.rpmCandidates.first().baseUrl,
             evidence.rpmCandidates.first().baseUrl);
}

void CoreTests::parsesRpmRepositoryMetadata() {
    const QByteArray repomd = R"(<?xml version="1.0"?>
<repomd xmlns="http://linux.duke.edu/metadata/repo">
  <data type="filelists"><location href="repodata/filelists.xml.gz"/></data>
  <data type="primary">
    <checksum type="sha256">aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa</checksum>
    <location href="repodata/primary.xml.gz"/>
  </data>
</repomd>)";
    QString error;
    const auto primary = pacsmith::RpmRepositoryMetadata::parseRepomd(
        QByteArrayView(repomd), &error);
    QVERIFY2(primary.has_value(), qPrintable(error));
    QCOMPARE(primary->path, QStringLiteral("repodata/primary.xml.gz"));
    QCOMPARE(primary->checksumType, QStringLiteral("sha256"));

    const QByteArray metadata = R"(<metadata xmlns="http://linux.duke.edu/metadata/common" packages="3">
<package type="rpm"><name>slack</name><arch>x86_64</arch>
<version epoch="0" ver="4.50.0" rel="0.1.el8"/>
<checksum type="sha256" pkgid="YES">bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb</checksum>
<location href="packages/slack-4.50.rpm"/></package>
<package type="rpm"><name>slack</name><arch>x86_64</arch>
<version epoch="0" ver="4.51.180" rel="0.1.el8"/>
<checksum type="sha256" pkgid="YES">cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc</checksum>
<location href="packages/slack-4.51.rpm"/></package>
<package type="rpm"><name>other</name><arch>x86_64</arch>
<version epoch="0" ver="99" rel="1"/>
<checksum type="sha256">dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd</checksum>
<location href="packages/other.rpm"/></package>
</metadata>)";
    const auto package = pacsmith::RpmRepositoryMetadata::latestPackage(
        QByteArrayView(metadata), QStringLiteral("slack"), QStringLiteral("x86_64"), &error);
    QVERIFY2(package.has_value(), qPrintable(error));
    QCOMPARE(package->evr(), QStringLiteral("4.51.180-0.1.el8"));
    QCOMPARE(package->filename, QStringLiteral("packages/slack-4.51.rpm"));
    QVERIFY(!pacsmith::RpmRepositoryMetadata::parseRepomd(
        QByteArrayView("<repomd><data type='primary'><checksum type='sha256'>aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa</checksum><location href='../escape'/></data></repomd>")));
}

void CoreTests::comparesRpmVersions() {
    QVERIFY(pacsmith::RpmVersion::compare(QStringLiteral("4.51.180-0.1.el8"),
                                          QStringLiteral("4.50.0-1")) > 0);
    QVERIFY(pacsmith::RpmVersion::compare(QStringLiteral("2:1.0-1"),
                                          QStringLiteral("1:99.0-9")) > 0);
    QVERIFY(pacsmith::RpmVersion::compare(QStringLiteral("1.0~rc1-1"),
                                          QStringLiteral("1.0-1")) < 0);
    QCOMPARE(pacsmith::RpmVersion::compare(QStringLiteral("1.0-01"),
                                           QStringLiteral("1.0-1")), 0);
}

QTEST_GUILESS_MAIN(CoreTests)
#include "core_tests.moc"
