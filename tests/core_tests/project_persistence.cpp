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
    settings.credentialSources.insert(QStringLiteral("github"), pacsmith::CredentialSource::Age);
    settings.githubTokenConfigured = true;
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
    QCOMPARE(restored.credentialSources.value(QStringLiteral("github")),
             pacsmith::CredentialSource::Age);
    QVERIFY(restored.githubTokenConfigured);
    QVERIFY(pacsmith::githubTokenUsesAge(restored));
    QVERIFY(store.ageSecretsPath().startsWith(temporary.path()));
    QCOMPARE(pacsmith::aiProviderFromName(QStringLiteral("codex")),
             pacsmith::AiProviderKind::None);
    QCOMPARE(pacsmith::aiProviderFromName(QStringLiteral("chatgpt")),
             pacsmith::AiProviderKind::ChatGpt);

    QTemporaryDir legacyDir;
    QVERIFY(legacyDir.isValid());
    QFile legacy(QDir(legacyDir.path()).filePath(QStringLiteral("settings.json")));
    QVERIFY(legacy.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QVERIFY(legacy.write(QByteArrayLiteral(
                R"({"formatVersion":4,"credentialSources":{"github":"age"}})")) > 0);
    legacy.close();
    pacsmith::AppSettingsStore legacyStore(legacyDir.path());
    const auto migrated = legacyStore.load();
    QVERIFY(migrated.githubTokenConfigured);
    QVERIFY(pacsmith::githubTokenUsesAge(migrated));
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
    settings.updates.startAtLogin = true;
    settings.updates.startMinimized = true;
    settings.updates.keepInTray = true;
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
    QVERIFY(restored.updates.startAtLogin);
    QVERIFY(restored.updates.startMinimized);
    QVERIFY(restored.updates.keepInTray);
    QVERIFY(restored.debAssociationPrompted);
    QVERIFY(restored.selfTrackingPrompted);
}

void CoreTests::writesLoginAutostartDesktopEntry() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto previous = qgetenv("XDG_CONFIG_HOME");
    QVERIFY(qputenv("XDG_CONFIG_HOME", QFile::encodeName(temporary.path())));
    pacsmith::BackgroundUpdateSettings settings;
    settings.startAtLogin = true;
    settings.startMinimized = true;
    QString error;
    const auto executable = QStringLiteral("/tmp/pacsmith-gui");
    QVERIFY2(pacsmith::BackgroundUpdateManager::apply(settings, executable, &error),
             qPrintable(error));
    QFile file(pacsmith::BackgroundUpdateManager::autostartPath());
    QVERIFY(file.exists());
    QVERIFY(file.open(QIODevice::ReadOnly));
    const auto contents = QString::fromUtf8(file.readAll());
    QVERIFY(contents.contains(QStringLiteral("Exec=/tmp/pacsmith-gui --tray")));
    settings.startAtLogin = false;
    QVERIFY2(pacsmith::BackgroundUpdateManager::apply(settings, executable, &error),
             qPrintable(error));
    QVERIFY(!QFile::exists(pacsmith::BackgroundUpdateManager::autostartPath()));
    if (previous.isEmpty()) qunsetenv("XDG_CONFIG_HOME");
    else QVERIFY(qputenv("XDG_CONFIG_HOME", previous));
}

void CoreTests::roundTripsBackgroundUpdateCheckActivity() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto previous = qgetenv("XDG_STATE_HOME");
    QVERIFY(qputenv("XDG_STATE_HOME", QFile::encodeName(temporary.path())));
    pacsmith::BackgroundUpdateState state;
    state.checking = true;
    state.checkingProjectId = QStringLiteral("cursor");
    state.checkingProjectName = QStringLiteral("Cursor");
    state.preparingProjectId = QStringLiteral("cursor");
    state.preparingProjectName = QStringLiteral("Cursor");
    state.preparationPhase = QStringLiteral("Downloading");
    state.preparationBytesReceived = 12 * 1024 * 1024;
    state.preparationBytesTotal = 40 * 1024 * 1024;
    state.message = QStringLiteral("Checking Cursor for updates");
    QString error;
    QVERIFY2(pacsmith::BackgroundUpdateStateStore::save(state, &error), qPrintable(error));
    const auto restored = pacsmith::BackgroundUpdateStateStore::load(&error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY(restored.checking);
    QCOMPARE(restored.checkingProjectId, QStringLiteral("cursor"));
    QCOMPARE(restored.checkingProjectName, QStringLiteral("Cursor"));
    QCOMPARE(restored.preparingProjectId, QStringLiteral("cursor"));
    QCOMPARE(restored.preparingProjectName, QStringLiteral("Cursor"));
    QCOMPARE(restored.preparationPhase, QStringLiteral("Downloading"));
    QCOMPARE(restored.preparationBytesReceived, static_cast<qint64>(12 * 1024 * 1024));
    QCOMPARE(restored.preparationBytesTotal, static_cast<qint64>(40 * 1024 * 1024));
    QCOMPARE(restored.message, QStringLiteral("Checking Cursor for updates"));
    if (previous.isEmpty()) qunsetenv("XDG_STATE_HOME");
    else QVERIFY(qputenv("XDG_STATE_HOME", previous));
}

void CoreTests::recountsAvailableUpdatesFromInstalledState() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto previous = qgetenv("XDG_STATE_HOME");
    QVERIFY(qputenv("XDG_STATE_HOME", QFile::encodeName(temporary.path())));

    pacsmith::Project current;
    current.id = QStringLiteral("current");
    pacsmith::PackageRelease currentRelease;
    currentRelease.id = QStringLiteral("2.0");
    currentRelease.debian.version = QStringLiteral("2.0");
    currentRelease.state = pacsmith::ReleaseState::Ready;
    current.releases.append(currentRelease);
    current.installedReleaseId = QStringLiteral("2.0");
    current.installedVersion = QStringLiteral("2.0-1");

    pacsmith::Project outdated;
    outdated.id = QStringLiteral("outdated");
    for (const auto &version : {QStringLiteral("1.0"), QStringLiteral("2.0")}) {
        pacsmith::PackageRelease release;
        release.id = version;
        release.debian.version = version;
        release.state = pacsmith::ReleaseState::Ready;
        outdated.releases.append(release);
    }
    outdated.installedReleaseId = QStringLiteral("1.0");
    outdated.installedVersion = QStringLiteral("1.0-1");

    pacsmith::BackgroundUpdateState stale;
    stale.availableUpdates = 4;
    stale.projectsWithUpdates = {QStringLiteral("stale-a"), QStringLiteral("stale-b")};
    stale.message = QStringLiteral("4 update(s) available");
    QString error;
    QVERIFY2(pacsmith::BackgroundUpdateStateStore::save(stale, &error), qPrintable(error));

    QCOMPARE(pacsmith::availableUpdateCount({current, outdated}), 1);
    QVERIFY2(pacsmith::BackgroundUpdateStateStore::syncAvailableUpdates({current, outdated}, &error),
             qPrintable(error));
    const auto restored = pacsmith::BackgroundUpdateStateStore::load(&error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(restored.availableUpdates, 1);
    QCOMPARE(restored.projectsWithUpdates, QStringList{QStringLiteral("outdated")});
    QCOMPARE(restored.message, QStringLiteral("1 update(s) available"));

    QVERIFY2(pacsmith::BackgroundUpdateStateStore::syncAvailableUpdates({current}, &error),
             qPrintable(error));
    const auto afterInstall = pacsmith::BackgroundUpdateStateStore::load(&error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(afterInstall.availableUpdates, 0);
    QVERIFY(afterInstall.projectsWithUpdates.isEmpty());
    QCOMPARE(afterInstall.message, QStringLiteral("All eligible project trackers are current"));

    if (previous.isEmpty()) qunsetenv("XDG_STATE_HOME");
    else QVERIFY(qputenv("XDG_STATE_HOME", previous));
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

void CoreTests::reportsOverdueBackgroundUpdateChecks() {
    pacsmith::BackgroundUpdateSettings settings;
    settings.enabled = true;
    settings.daily = true;
    settings.localTime = QTime(2, 0);
    const auto now = QDateTime(QDate(2026, 8, 18), QTime(16, 0), QTimeZone::systemTimeZone());

    QVERIFY(pacsmith::BackgroundUpdateManager::isOverdue(settings, {}, now));

    const auto lastDue = pacsmith::BackgroundUpdateManager::lastScheduledOccurrence(settings, now);
    QCOMPARE(lastDue.date(), QDate(2026, 8, 18));
    QCOMPARE(lastDue.time(), QTime(2, 0));
    QVERIFY(!pacsmith::BackgroundUpdateManager::isOverdue(settings, lastDue, now));
    QVERIFY(!pacsmith::BackgroundUpdateManager::isOverdue(settings, lastDue.addSecs(60), now));
    QVERIFY(pacsmith::BackgroundUpdateManager::isOverdue(settings, lastDue.addSecs(-60), now));

    const auto beforeSchedule = QDateTime(QDate(2026, 8, 18), QTime(1, 0), QTimeZone::systemTimeZone());
    QCOMPARE(pacsmith::BackgroundUpdateManager::lastScheduledOccurrence(settings, beforeSchedule).date(),
             QDate(2026, 8, 17));

    settings.enabled = false;
    QVERIFY(!pacsmith::BackgroundUpdateManager::isOverdue(settings, {}, now));

    settings.enabled = true;
    settings.daily = false;
    settings.weekDay = 1;
    const auto weekly = pacsmith::BackgroundUpdateManager::lastScheduledOccurrence(settings, now);
    QCOMPARE(weekly.date(), QDate(2026, 8, 17));
    QCOMPARE(weekly.time(), QTime(2, 0));
    QCOMPARE(pacsmith::BackgroundUpdateManager::nextScheduledOccurrence(settings, now).date(),
             QDate(2026, 8, 24));
    settings.daily = true;
    QCOMPARE(pacsmith::BackgroundUpdateManager::nextScheduledOccurrence(settings, now).date(),
             QDate(2026, 8, 19));
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
    QVERIFY(!project.hasAvailableUpdate());
    pacsmith::PackageRelease detected;
    detected.id = QStringLiteral("3.0");
    detected.debian.version = QStringLiteral("3.0");
    detected.state = pacsmith::ReleaseState::Discovered;
    project.releases.append(detected);
    QVERIFY(project.hasAvailableUpdate());

    project.installedReleaseId.clear();
    project.externallyInstalled = true;
    QVERIFY(!project.hasAvailableUpdate());
}

void CoreTests::deletingUpdateReleaseClearsAvailableStatus() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    pacsmith::ProjectStore store(
        std::filesystem::path(temporary.path().toUtf8().constData()) / "projects");
    pacsmith::Project project;
    project.id = QStringLiteral("signal");
    project.archPackageName = QStringLiteral("signal-desktop-bin");
    pacsmith::PackageRelease installed;
    installed.id = QStringLiteral("8.23.0-aaaaaaaaaaaa");
    installed.projectId = project.id;
    installed.archPackageName = project.archPackageName;
    installed.debian.version = QStringLiteral("8.23.0");
    installed.state = pacsmith::ReleaseState::Built;
    installed.buildStatus = pacsmith::BuildStatus::Succeeded;
    installed.update.strategy = pacsmith::UpdateStrategy::AptRepository;
    installed.update.detectedVersion = QStringLiteral("8.24.0");
    installed.update.detectedFilename = QStringLiteral("signal-desktop_8.24.0_amd64.deb");
    installed.update.detectedUrl = QStringLiteral("https://updates.signal.org/desktop/apt/pool/s/signal-desktop/signal-desktop_8.24.0_amd64.deb");
    installed.update.githubEtag = QStringLiteral("etag-824");
    pacsmith::PackageRelease update;
    update.id = QStringLiteral("8.24.0-bbbbbbbbbbbb");
    update.projectId = project.id;
    update.archPackageName = project.archPackageName;
    update.debian.version = QStringLiteral("8.24.0");
    update.state = pacsmith::ReleaseState::Ready;
    project.releases.append(installed);
    project.releases.append(update);
    project.installedReleaseId = installed.id;
    project.installedVersion = QStringLiteral("8.23.0-1");
    QString error;
    QVERIFY2(store.save(project, &error), qPrintable(error));
    QVERIFY(project.hasAvailableUpdate());

    QVERIFY2(store.deleteRelease(project, update.id, &error), qPrintable(error));
    QCOMPARE(project.releases.size(), 1);
    QVERIFY(!project.hasAvailableUpdate());
    const auto *tracker = project.release(installed.id);
    QVERIFY(tracker != nullptr);
    QCOMPARE(tracker->update.detectedVersion, QString{});
    QCOMPARE(tracker->update.detectedFilename, QString{});
    QCOMPARE(tracker->update.detectedUrl, QString{});
    QCOMPARE(tracker->update.githubEtag, QString{});
}

void CoreTests::dropsUnbuiltIntermediateUpdates() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    pacsmith::ProjectStore store(
        std::filesystem::path(temporary.path().toUtf8().constData()) / "projects");
    pacsmith::Project project;
    project.id = QStringLiteral("stale-updates");
    project.displayName = QStringLiteral("Stale Updates");
    project.archPackageName = QStringLiteral("pacsmith-test-stale-updates-not-installed");
    const QStringList versions{QStringLiteral("1.0"), QStringLiteral("1.1"),
                               QStringLiteral("1.2"), QStringLiteral("1.3")};
    for (int index = 0; index < versions.size(); ++index) {
        pacsmith::PackageRelease release;
        release.id = versions.at(index) + QStringLiteral("-aaaaaaaaaaaa");
        release.projectId = project.id;
        release.archPackageName = project.archPackageName;
        release.debian.package = QStringLiteral("stale");
        release.debian.version = versions.at(index);
        release.sourceSha256 = QString(64, QChar(u'a' + static_cast<char16_t>(index)));
        release.state = pacsmith::ReleaseState::Ready;
        if (index == 0 || index == 1) {
            release.buildStatus = pacsmith::BuildStatus::Succeeded;
            release.state = pacsmith::ReleaseState::Built;
        }
        project.releases.append(release);
    }
    QString error;
    QVERIFY2(store.save(project, &error), qPrintable(error));

    auto kept = store.cleanup(project, {2, 3, false}, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(project.releases.size(), 4);

    kept = store.cleanup(project, {2, 3, true}, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(project.releases.size(), 3);
    QStringList remaining;
    for (const auto &release : project.releases) remaining.append(release.debian.version);
    remaining.sort();
    QCOMPARE(remaining, (QStringList{QStringLiteral("1.0"), QStringLiteral("1.1"),
                                     QStringLiteral("1.3")}));
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

void CoreTests::reusesInspectedReleaseForSameVendorVersion() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    pacsmith::ProjectStore store(
        std::filesystem::path(temporary.path().toUtf8().constData()) / "projects");
    pacsmith::Project project;
    project.id = QStringLiteral("reuse");
    project.displayName = QStringLiteral("Reuse");
    project.archPackageName = QStringLiteral("reuse-bin");
    pacsmith::PackageRelease installed;
    installed.id = QStringLiteral("1.0-aaaaaaaaaaaa");
    installed.projectId = project.id;
    installed.archPackageName = project.archPackageName;
    installed.sourceType = pacsmith::SourcePackageType::Debian;
    installed.sourceUrl = QStringLiteral("file:///tmp/reuse-1.0.deb");
    installed.sourceSha256 = QString(64, QLatin1Char('a'));
    installed.debian.package = QStringLiteral("reuse");
    installed.debian.version = QStringLiteral("1.0");
    installed.state = pacsmith::ReleaseState::Built;
    installed.buildStatus = pacsmith::BuildStatus::Succeeded;
    installed.update.strategy = pacsmith::UpdateStrategy::GitHubRelease;
    installed.update.githubOwner = QStringLiteral("vendor");
    installed.update.githubRepository = QStringLiteral("reuse");
    pacsmith::PackageRelease inspected;
    inspected.id = QStringLiteral("1.1-bbbbbbbbbbbb");
    inspected.projectId = project.id;
    inspected.archPackageName = project.archPackageName;
    inspected.sourceType = pacsmith::SourcePackageType::Debian;
    inspected.sourceUrl = QStringLiteral("file:///tmp/reuse-1.1.deb");
    inspected.sourceSha256 = QString(64, QLatin1Char('b'));
    inspected.debian.package = QStringLiteral("reuse");
    inspected.debian.version = QStringLiteral("1.1");
    inspected.state = pacsmith::ReleaseState::Ready;
    inspected.update.strategy = pacsmith::UpdateStrategy::GitHubRelease;
    inspected.update.githubOwner = QStringLiteral("vendor");
    inspected.update.githubRepository = QStringLiteral("reuse");
    project.releases.append(installed);
    project.releases.append(inspected);
    project.installedReleaseId = installed.id;
    project.installedVersion = QStringLiteral("1.0-1");
    QString error;
    QVERIFY2(store.save(project, &error), qPrintable(error));

    auto *matched = store.recordDiscoveredRelease(
        project, installed, QStringLiteral("1.1"), QStringLiteral("reuse-1.1.tar.gz"), {},
        QStringLiteral("https://github.com/vendor/reuse/releases/download/v1.1/reuse-1.1.tar.gz"),
        &error, 110, 111, QStringLiteral("v1.1"), {});
    QVERIFY2(matched != nullptr, qPrintable(error));
    QCOMPARE(matched->id, inspected.id);
    QVERIFY(matched->state != pacsmith::ReleaseState::Discovered);
    QVERIFY(matched->state != pacsmith::ReleaseState::Preparing);
    QCOMPARE(project.releases.size(), 2);

    auto *rebuild = store.recordDiscoveredRelease(
        project, installed, QStringLiteral("1.1"), QStringLiteral("reuse-1.1.tar.gz"),
        QString(64, QLatin1Char('c')),
        QStringLiteral("https://github.com/vendor/reuse/releases/download/v1.1/reuse-1.1.tar.gz"),
        &error, 112, 113, QStringLiteral("v1.1"), {});
    QVERIFY2(rebuild != nullptr, qPrintable(error));
    QVERIFY(rebuild->id != inspected.id);
    QCOMPARE(rebuild->state, pacsmith::ReleaseState::Discovered);
    QCOMPARE(project.releases.size(), 3);
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
        QStringLiteral("install=\"${_PACSMITH_INSTALL}\"")));
    QVERIFY(pacsmith::PkgbuildGenerator::identityVariables(*updated).contains(
        QStringLiteral("_PACSMITH_INSTALL='vendorctl-bin.install'")));
    QFile lifecycle(QString::fromUtf8(store.lifecyclePath(*updated).string().c_str()));
    QVERIFY(lifecycle.open(QIODevice::ReadOnly));
    QCOMPARE(QString::fromUtf8(lifecycle.readAll()), updated->lifecycleScript.contents);
}

void CoreTests::attachesPreparedGitHubDebToExistingProject() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto writeFixture = [&](const std::filesystem::path &path, const QByteArray &contents) {
        std::filesystem::create_directories(path.parent_path());
        QFile file(QString::fromUtf8(path.string().c_str()));
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(contents), contents.size());
    };
    const auto makeDeb = [&](const QString &version, const QByteArray &payload) -> QString {
        const auto root = std::filesystem::path(
            (temporary.path() + QStringLiteral("/deb-") + version).toUtf8().constData());
        writeFixture(root / "control/control",
                     QByteArray("Package: affine\nVersion: ") + version.toUtf8() +
                         "\nArchitecture: amd64\nDescription: AFFiNE fixture\n");
        writeFixture(root / "data/usr/share/applications/affine.desktop",
                     QByteArrayLiteral("[Desktop Entry]\nType=Application\nName=AFFiNE\nExec=affine\n"));
        writeFixture(root / "data/opt/AFFiNE/affine", payload);
        writeFixture(root / "debian-binary", QByteArrayLiteral("2.0\n"));
        const auto makeTar = [&](const QString &output, const std::filesystem::path &directory) -> bool {
            QProcess tar;
            tar.setProgram(QStringLiteral("/usr/bin/bsdtar"));
            tar.setArguments({QStringLiteral("-cf"), output, QStringLiteral("-C"),
                              QString::fromUtf8(directory.string().c_str()), QStringLiteral(".")});
            tar.start();
            return tar.waitForFinished(10000) && tar.exitCode() == 0;
        };
        const auto work = QString::fromUtf8(root.string().c_str());
        if (!makeTar(work + QStringLiteral("/control.tar"), root / "control")) return {};
        if (!makeTar(work + QStringLiteral("/data.tar"), root / "data")) return {};
        const auto deb = temporary.filePath(QStringLiteral("affine_%1_amd64.deb").arg(version));
        QProcess ar;
        ar.setWorkingDirectory(work);
        ar.setProgram(QStringLiteral("/usr/bin/ar"));
        ar.setArguments({QStringLiteral("r"), deb, QStringLiteral("control.tar"),
                         QStringLiteral("data.tar"), QStringLiteral("debian-binary")});
        ar.start();
        if (!ar.waitForFinished(10000) || ar.exitCode() != 0) return {};
        return deb;
    };
    const auto firstDeb = makeDeb(QStringLiteral("0.27.3"), QByteArrayLiteral("old-payload\n"));
    const auto secondDeb = makeDeb(QStringLiteral("0.27.4"), QByteArrayLiteral("new-payload\n"));
    QVERIFY(!firstDeb.isEmpty());
    QVERIFY(!secondDeb.isEmpty());

    pacsmith::ProjectStore store(
        std::filesystem::path(temporary.path().toUtf8().constData()) / "projects");
    pacsmith::ImportOptions firstOptions;
    firstOptions.acquisition.kind = pacsmith::AcquisitionKind::LocalFile;
    firstOptions.acquisition.canonicalIdentity = QStringLiteral("deb:affine:amd64");
    firstOptions.acquisition.originalUrl = firstDeb;
    QString error;
    auto first = store.importSource(
        std::filesystem::path(firstDeb.toUtf8().constData()), firstOptions, &error);
    QVERIFY2(first.has_value(), qPrintable(error));
    QCOMPARE(store.list().size(), 1);
    auto *tracker = first->project.release(first->releaseId);
    QVERIFY(tracker != nullptr);
    tracker->update.strategy = pacsmith::UpdateStrategy::GitHubRelease;
    tracker->update.githubOwner = QStringLiteral("toeverything");
    tracker->update.githubRepository = QStringLiteral("AFFiNE");
    tracker->update.githubAssetRegex = QStringLiteral("affine-.*-stable-linux-x64\\.deb");
    const auto trackerSnapshot = *tracker;
    QVERIFY2(store.save(first->project, &error), qPrintable(error));

    const auto *discovered = store.recordDiscoveredRelease(
        first->project, trackerSnapshot, QStringLiteral("0.27.4"),
        QStringLiteral("affine-0.27.4-stable-linux-x64.deb"), {},
        QStringLiteral("https://github.com/toeverything/AFFiNE/releases/download/v0.27.4/"
                       "affine-0.27.4-stable-linux-x64.deb"),
        &error, 274, 275, QStringLiteral("v0.27.4"), {});
    QVERIFY2(discovered != nullptr, qPrintable(error));
    QCOMPARE(discovered->acquisition.canonicalIdentity,
             QStringLiteral("github:toeverything/affine"));

    pacsmith::ImportOptions prepareOptions;
    prepareOptions.version = QStringLiteral("0.27.4");
    prepareOptions.acquisition = discovered->acquisition;
    prepareOptions.githubAssetRegex = trackerSnapshot.update.githubAssetRegex;
    prepareOptions.existingProjectId = first->project.id;
    auto prepared = store.importSource(
        std::filesystem::path(secondDeb.toUtf8().constData()), prepareOptions, &error);
    QVERIFY2(prepared.has_value(), qPrintable(error));
    QVERIFY(!prepared->projectCreated);
    QCOMPARE(prepared->project.id, first->project.id);
    QCOMPARE(store.list().size(), 1);
    QCOMPARE(prepared->project.sourceIdentity, QStringLiteral("github:toeverything/affine"));
    QCOMPARE(prepared->project.releases.size(), 2);
    const auto *preparedRelease = prepared->project.release(prepared->releaseId);
    QVERIFY(preparedRelease != nullptr);
    QCOMPARE(preparedRelease->update.strategy, pacsmith::UpdateStrategy::GitHubRelease);
    QCOMPARE(preparedRelease->update.githubOwner, QStringLiteral("toeverything"));
    QCOMPARE(preparedRelease->update.githubRepository, QStringLiteral("AFFiNE"));
    QCOMPARE(preparedRelease->update.githubAssetRegex,
             QStringLiteral("affine-.*-stable-linux-x64\\.deb"));

    pacsmith::ProjectStore siblingStore(
        std::filesystem::path(temporary.path().toUtf8().constData()) / "projects");
    pacsmith::ImportOptions unmatched = prepareOptions;
    unmatched.existingProjectId.clear();
    auto attached = siblingStore.importSource(
        std::filesystem::path(secondDeb.toUtf8().constData()), unmatched, &error);
    QVERIFY2(attached.has_value(), qPrintable(error));
    QVERIFY(!attached->projectCreated);
    QCOMPARE(attached->project.id, first->project.id);
    QCOMPARE(siblingStore.list().size(), 1);
}

void CoreTests::preservesAptTrackerAcrossDebUpdates() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto writeFixture = [&](const std::filesystem::path &path, const QByteArray &contents) {
        std::filesystem::create_directories(path.parent_path());
        QFile file(QString::fromUtf8(path.string().c_str()));
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(contents), contents.size());
    };
    const auto makeDeb = [&](const QString &version, const QByteArray &payload) -> QString {
        const auto root = std::filesystem::path(
            (temporary.path() + QStringLiteral("/signal-") + version).toUtf8().constData());
        writeFixture(root / "control/control",
                     QByteArray("Package: signal-desktop\nVersion: ") + version.toUtf8() +
                         "\nArchitecture: amd64\nDescription: Signal fixture\n");
        writeFixture(root / "data/usr/share/applications/signal-desktop.desktop",
                     QByteArrayLiteral("[Desktop Entry]\nType=Application\nName=Signal\nExec=signal-desktop\n"));
        writeFixture(root / "data/opt/Signal/signal-desktop", payload);
        writeFixture(root / "debian-binary", QByteArrayLiteral("2.0\n"));
        const auto makeTar = [&](const QString &output, const std::filesystem::path &directory) -> bool {
            QProcess tar;
            tar.setProgram(QStringLiteral("/usr/bin/bsdtar"));
            tar.setArguments({QStringLiteral("-cf"), output, QStringLiteral("-C"),
                              QString::fromUtf8(directory.string().c_str()), QStringLiteral(".")});
            tar.start();
            return tar.waitForFinished(10000) && tar.exitCode() == 0;
        };
        const auto work = QString::fromUtf8(root.string().c_str());
        if (!makeTar(work + QStringLiteral("/control.tar"), root / "control")) return {};
        if (!makeTar(work + QStringLiteral("/data.tar"), root / "data")) return {};
        const auto deb = temporary.filePath(QStringLiteral("signal-desktop_%1_amd64.deb").arg(version));
        QProcess ar;
        ar.setWorkingDirectory(work);
        ar.setProgram(QStringLiteral("/usr/bin/ar"));
        ar.setArguments({QStringLiteral("r"), deb, QStringLiteral("control.tar"),
                         QStringLiteral("data.tar"), QStringLiteral("debian-binary")});
        ar.start();
        if (!ar.waitForFinished(10000) || ar.exitCode() != 0) return {};
        return deb;
    };
    const auto firstDeb = makeDeb(QStringLiteral("8.23.0"), QByteArrayLiteral("old-signal\n"));
    const auto secondDeb = makeDeb(QStringLiteral("8.24.0"), QByteArrayLiteral("new-signal\n"));
    QVERIFY(!firstDeb.isEmpty());
    QVERIFY(!secondDeb.isEmpty());

    pacsmith::ProjectStore store(
        std::filesystem::path(temporary.path().toUtf8().constData()) / "projects");
    pacsmith::ImportOptions firstOptions;
    firstOptions.acquisition.kind = pacsmith::AcquisitionKind::LocalFile;
    firstOptions.acquisition.canonicalIdentity = QStringLiteral("deb:signal-desktop:amd64");
    firstOptions.acquisition.originalUrl = firstDeb;
    QString error;
    auto first = store.importSource(
        std::filesystem::path(firstDeb.toUtf8().constData()), firstOptions, &error);
    QVERIFY2(first.has_value(), qPrintable(error));
    auto *tracker = first->project.release(first->releaseId);
    QVERIFY(tracker != nullptr);
    tracker->update.strategy = pacsmith::UpdateStrategy::AptRepository;
    tracker->update.url = QStringLiteral("https://updates.signal.org/desktop/apt");
    tracker->update.aptSuite = QStringLiteral("xenial");
    tracker->update.aptComponent = QStringLiteral("main");
    tracker->update.aptArchitecture = QStringLiteral("amd64");
    tracker->update.aptPackageName = QStringLiteral("signal-desktop");
    tracker->update.aptSigningKeyring = QStringLiteral("files/keys/vendor-signal.gpg");
    tracker->update.trustedSigningFingerprint = QStringLiteral("DBA36B818115965B43BB1BA87521D88C4F37503A");
    const auto keyPath = store.releasePath(*tracker) /
                         std::filesystem::path("files/keys/vendor-signal.gpg");
    writeFixture(keyPath, QByteArrayLiteral("signal-apt-key"));
    const auto trackerSnapshot = *tracker;
    QVERIFY2(store.save(first->project, &error), qPrintable(error));

    const auto secondHash = pacsmith::sha256File(
        std::filesystem::path(secondDeb.toUtf8().constData()), &error);
    QVERIFY2(!secondHash.isEmpty(), qPrintable(error));
    const auto *discovered = store.recordDiscoveredRelease(
        first->project, trackerSnapshot, QStringLiteral("8.24.0"),
        QStringLiteral("signal-desktop_8.24.0_amd64.deb"), secondHash,
        QStringLiteral("https://updates.signal.org/desktop/apt/pool/s/signal-desktop/"
                       "signal-desktop_8.24.0_amd64.deb"),
        &error);
    QVERIFY2(discovered != nullptr, qPrintable(error));
    QCOMPARE(discovered->update.strategy, pacsmith::UpdateStrategy::AptRepository);

    pacsmith::ImportOptions prepareOptions;
    prepareOptions.version = QStringLiteral("8.24.0");
    prepareOptions.acquisition = discovered->acquisition;
    prepareOptions.existingProjectId = first->project.id;
    auto prepared = store.importSource(
        std::filesystem::path(secondDeb.toUtf8().constData()), prepareOptions, &error);
    QVERIFY2(prepared.has_value(), qPrintable(error));
    const auto *updated = prepared->project.release(prepared->releaseId);
    QVERIFY(updated != nullptr);
    QCOMPARE(updated->update.strategy, pacsmith::UpdateStrategy::AptRepository);
    QCOMPARE(updated->update.url, QStringLiteral("https://updates.signal.org/desktop/apt"));
    QCOMPARE(updated->update.aptSuite, QStringLiteral("xenial"));
    QCOMPARE(updated->update.aptPackageName, QStringLiteral("signal-desktop"));
    QCOMPARE(updated->update.aptSigningKeyring, QStringLiteral("files/keys/vendor-signal.gpg"));
    QVERIFY(std::filesystem::is_regular_file(
        store.releasePath(*updated) / std::filesystem::path("files/keys/vendor-signal.gpg")));
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
    QString queryError;
    const auto installed = pacsmith::ProjectStore::queryInstalledVersion(QStringLiteral("pacman"),
                                                                         &queryError);
    QVERIFY2(installed.has_value() && !installed->isEmpty(), qPrintable(queryError));
    pacsmith::Project project;
    project.id = QStringLiteral("installed-project");
    project.displayName = QStringLiteral("Installed Project");
    project.archPackageName = QStringLiteral("pacman");
    pacsmith::PackageRelease release;
    release.id = QStringLiteral("installed");
    release.archPackageName = project.archPackageName;
    pacsmith::BuildRecord build;
    pacsmith::PackageArtifact artifact;
    artifact.packageName = project.archPackageName;
    artifact.packageVersion = *installed;
    build.artifacts.append(artifact);
    release.builds.append(build);
    project.releases.append(release);
    QString error;
    QVERIFY2(store.save(project, &error), qPrintable(error));
    QVERIFY(!store.deleteProject(project, &error));
    QVERIFY(error.contains(QStringLiteral("is installed")));
    QVERIFY(std::filesystem::exists(store.projectPath(project.id)));
}

void CoreTests::allowsDeletingProjectThatDoesNotOwnInstalledPackage() {
    if (!QFileInfo::exists(QStringLiteral("/usr/bin/pacman"))) QSKIP("pacman is required for this Arch-specific test");
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    pacsmith::ProjectStore store(std::filesystem::path(temporary.path().toUtf8().constData()) / "projects");
    pacsmith::Project project;
    project.id = QStringLiteral("sibling-project");
    project.displayName = QStringLiteral("Sibling Project");
    project.archPackageName = QStringLiteral("pacman");
    QString error;
    QVERIFY2(store.save(project, &error), qPrintable(error));
    QVERIFY2(store.deleteProject(project, &error), qPrintable(error));
    QVERIFY(!std::filesystem::exists(store.projectPath(project.id)));
}

