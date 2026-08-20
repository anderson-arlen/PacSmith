#include "core_tests.hpp"

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
#include "core/project_store/project_store.hpp"
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
#include <QTimeZone>
#include <QtTest>

#include <algorithm>
#include <filesystem>

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
    QVERIFY(pkgbuild.contains(QStringLiteral("exec \"/opt/${_PACSMITH_OPT}/AppRun\" \"\\$@\"")));

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

