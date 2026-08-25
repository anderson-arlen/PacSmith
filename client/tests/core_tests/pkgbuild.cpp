#include "core_tests.hpp"

#include "core/app_settings.hpp"
#include "core/background_updates.hpp"
#include "core/apt_repository.hpp"
#include "core/apt_update_service.hpp"
#include "core/apt_sources.hpp"
#include "core/control_parser.hpp"
#include "core/credential_store.hpp"
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
    QCOMPARE(reset->update.url,
             QStringLiteral("https://vendor.example/releases/latest.tar"));
    QCOMPARE(reset->buildStatus, pacsmith::BuildStatus::NeverBuilt);
    QVERIFY(reset->producedPackages.isEmpty());
    QCOMPARE(reset->builds.size(), 1);
    QVERIFY(!QFileInfo::exists(QString::fromUtf8(lifecycle.string().c_str())));
    QFile pkgbuild(QString::fromUtf8(store.pkgbuildPath(*reset).string().c_str()));
    QVERIFY(pkgbuild.open(QIODevice::ReadOnly));
    const auto regenerated = pkgbuild.readAll();
    QVERIFY(regenerated.contains("pkgname=\"${_PACSMITH_PKGNAME}\""));
    QVERIFY(!regenerated.contains("user-owned recipe"));
    QFile vars(QString::fromUtf8(store.identityVariablesPath(*reset).string().c_str()));
    QVERIFY(vars.open(QIODevice::ReadOnly));
    QVERIFY(vars.readAll().contains("_PACSMITH_PKGNAME='vendor-bin'"));
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
    project.packageMetadata.description = project.debian.description;
    project.packageMetadata.homepage = project.debian.homepage;
    project.packageMetadata.licenses = {QStringLiteral("MIT")};
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
    const auto vars = pacsmith::PkgbuildGenerator::identityVariables(project);
    QVERIFY(pkgbuild.contains(QStringLiteral("source \"${startdir:-.}/pacsmith.vars\"")));
    QVERIFY(pkgbuild.contains(QStringLiteral("pkgname=\"${_PACSMITH_PKGNAME}\"")));
    QVERIFY(vars.contains(QStringLiteral("_PACSMITH_PKGNAME='vendor-app-bin'")));
    QVERIFY(vars.contains(QStringLiteral("_PACSMITH_EPOCH='2'")));
    QVERIFY(vars.contains(QStringLiteral("_PACSMITH_PKGVER='1.2.3'")));
    QVERIFY(vars.contains(QStringLiteral("_PACSMITH_ARCH='x86_64'")));
    QVERIFY(vars.contains(QStringLiteral("_PACSMITH_SOURCE='vendor app_1.2_amd64.deb'")));
    QVERIFY(vars.contains(QStringLiteral("_PACSMITH_INSTALL='vendor-app-bin.install'")));
    QVERIFY(vars.contains(QStringLiteral("_PACSMITH_LICENSES=('MIT')")));
    QVERIFY(pkgbuild.contains(QStringLiteral("depends=('gtk3')")));
    QVERIFY(pkgbuild.contains(QStringLiteral("options=('!strip' '!debug')")));
    QVERIFY(pkgbuild.contains(QStringLiteral("source=(\"${_PACSMITH_SOURCE}\")")));
    QVERIFY(pkgbuild.contains(QStringLiteral("data.tar|data.tar.*")));
    QVERIFY(pkgbuild.contains(QStringLiteral("--no-same-owner")));
    QVERIFY(pkgbuild.contains(QStringLiteral("install=\"${_PACSMITH_INSTALL}\"")));
    QVERIFY(pkgbuild.contains(QStringLiteral("${pkgdir}/etc/apt/sources.list.d/vendor.list")));
    QVERIFY(pkgbuild.contains(QStringLiteral("provides=(\"${_PACSMITH_PROVIDES[@]}\")")));
    QVERIFY(pkgbuild.contains(QStringLiteral("if [[ -n ${_PACSMITH_COMPAT_PKGNAME} ]]; then")));
    QVERIFY(pkgbuild.contains(QStringLiteral("provides+=(\"${_PACSMITH_COMPAT_PKGNAME}=${pkgver}\")")));
    QVERIFY(!pkgbuild.contains(QStringLiteral("postinst")));

    project.packageMetadata.provides = {QStringLiteral("vendor-app"),
                                        QStringLiteral("extra-app")};
    project.packageMetadata.conflicts = {QStringLiteral("old-vendor-app")};
    project.packageMetadata.additionalDependencies = {QStringLiteral("libnotify")};
    const auto varsWithMeta = pacsmith::PkgbuildGenerator::identityVariables(project);
    QVERIFY(varsWithMeta.contains(QStringLiteral("_PACSMITH_PROVIDES=('vendor-app' 'extra-app')")));
    QVERIFY(varsWithMeta.contains(QStringLiteral("_PACSMITH_CONFLICTS=('old-vendor-app')")));
    QVERIFY(pacsmith::PkgbuildGenerator::generate(project).contains(
        QStringLiteral("depends=('gtk3' 'libnotify')")));

    project.lifecycleScript.validationPassed = false;
    const auto blockedLifecycle = pacsmith::PkgbuildGenerator::generate(project);
    QVERIFY(blockedLifecycle.contains(QStringLiteral("install=\"${_PACSMITH_INSTALL}\"")));
    QVERIFY(!pacsmith::PkgbuildGenerator::identityVariables(project).contains(
        QStringLiteral("_PACSMITH_INSTALL='vendor-app-bin.install'")));
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
    const auto archiveVars = pacsmith::PkgbuildGenerator::identityVariables(release);
    QVERIFY(archive.contains(QStringLiteral("options=('!strip' '!debug')")));
    QVERIFY(archive.contains(QStringLiteral("pacsmith.schema=1")));
    QVERIFY(archive.contains(QStringLiteral("pacsmith.source=${_PACSMITH_SOURCE_IDENTITY}")));
    QVERIFY(archiveVars.contains(QStringLiteral("_PACSMITH_SOURCE_IDENTITY='github%3Avendor%2Ftool'")));
    QVERIFY(archive.contains(QStringLiteral("$pkgdir/opt/${_PACSMITH_OPT}")));
    QCOMPARE(pacsmith::PkgbuildGenerator::installedPayloadPath(
                 release, QStringLiteral("vendor-tool-2.1/bin/tool")),
             QStringLiteral("/opt/vendor-tool/bin/tool"));
    QVERIFY(archive.contains(QStringLiteral("--strip-components 1")));
    QVERIFY(archive.contains(QStringLiteral("../../opt/${_PACSMITH_OPT}/bin/tool")));
    QVERIFY(archive.contains(QStringLiteral("$pkgdir/usr/bin/tool")));
    QVERIFY(archive.contains(QStringLiteral("source+=(\"${_PACSMITH_ICON}\")")));
    QVERIFY(archiveVars.contains(QStringLiteral("_PACSMITH_ICON='pacsmith-icon.svg'")));
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
    QVERIFY(appImage.contains(QStringLiteral("unsquashfs -no-progress -no-xattrs -f -o ${_PACSMITH_APPIMAGE_OFFSET}")));
    QVERIFY(appImage.contains(QStringLiteral("-type f -exec chmod u-s,g-s")));
    QVERIFY(appImage.contains(QStringLiteral("APPDIR='/opt/${_PACSMITH_OPT}'")));
    QVERIFY(appImage.contains(QStringLiteral("unset APPIMAGE")));
    QVERIFY(appImage.contains(QStringLiteral("exec \"/opt/${_PACSMITH_OPT}/AppRun\" \"\\$@\"")));
    QVERIFY(!appImage.contains(QStringLiteral("exec -a")));
    QVERIFY(!appImage.contains(QStringLiteral("APPIMAGE=extracted")));
    QVERIFY(!appImage.contains(QStringLiteral("$pkgdir/opt/${_PACSMITH_OPT}/AppRun")));
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
    QVERIFY(overlaid.contains(QStringLiteral("$pkgdir/opt/${_PACSMITH_OPT}/AppRun")));
    QVERIFY(overlaid.contains(QStringLiteral("exec \"$APPDIR/vendor-tool\" \"$@\"")));
    QVERIFY(overlaid.contains(QStringLiteral("unset APPIMAGE")));
    QVERIFY(overlaid.contains(QStringLiteral("exec \"/opt/${_PACSMITH_OPT}/AppRun\" \"\\$@\"")));

    release.sourceType = pacsmith::SourcePackageType::ElfBinary;
    release.originalSourceFilename = QStringLiteral("tool");
    release.installMapping.binaryDestination = QStringLiteral("/usr/bin/tool");
    release.installMapping.launchers.clear();
    const auto elf = pacsmith::PkgbuildGenerator::generate(release);
    QVERIFY(elf.contains(QStringLiteral(
        "install -Dm755 \"$srcdir/${_PACSMITH_SOURCE}\" \"$pkgdir/usr/bin/tool\"")));

    release.sourceType = pacsmith::SourcePackageType::ArchPackage;
    release.originalSourceFilename = QStringLiteral("vendor-tool-2.1-1-x86_64.pkg.tar.zst");
    release.archPkgrelOverride = QStringLiteral("1.1");
    const auto archPackage = pacsmith::PkgbuildGenerator::generate(release);
    QVERIFY(archPackage.contains(QStringLiteral("pkgrel=\"${_PACSMITH_PKGREL}\"")));
    QVERIFY(pacsmith::PkgbuildGenerator::identityVariables(release).contains(
        QStringLiteral("_PACSMITH_PKGREL='1.1'")));
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

void CoreTests::writesPacsmithIdentityVariablesAcrossUpdates() {
    const auto executable = QStandardPaths::findExecutable(QStringLiteral("true"));
    QVERIFY(!executable.isEmpty());
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto firstPath = temporary.filePath(QStringLiteral("vendorctl-1.0.0-linux-x86_64"));
    const auto secondPath = temporary.filePath(QStringLiteral("vendorctl-2.0.0-linux-x86_64"));
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
    firstOptions.acquisition.kind = pacsmith::AcquisitionKind::GitHubRelease;
    firstOptions.acquisition.canonicalIdentity = QStringLiteral("github:vendor/vendorctl");
    firstOptions.acquisition.originalUrl = QStringLiteral("https://example.invalid/v1");
    firstOptions.acquisition.githubOwner = QStringLiteral("vendor");
    firstOptions.acquisition.githubRepository = QStringLiteral("vendorctl");
    firstOptions.acquisition.githubReleaseId = 10;
    firstOptions.acquisition.githubAssetId = 11;
    QString error;
    auto first = store.importSource(
        std::filesystem::path(firstPath.toUtf8().constData()), firstOptions, &error);
    QVERIFY2(first.has_value(), qPrintable(error));
    auto *tracker = first->project.release(first->releaseId);
    QVERIFY(tracker != nullptr);
    QFile firstVars(QString::fromUtf8(store.identityVariablesPath(*tracker).string().c_str()));
    QVERIFY(firstVars.open(QIODevice::ReadOnly));
    QVERIFY(firstVars.readAll().contains("_PACSMITH_SOURCE='vendorctl-1.0.0-linux-x86_64'"));
    const auto custom = QStringLiteral(
        "source \"${startdir:-.}/pacsmith.vars\"\n"
        "pkgname=\"${_PACSMITH_PKGNAME}\"\n"
        "pkgver=\"${_PACSMITH_PKGVER}\"\n"
        "pkgrel=\"${_PACSMITH_PKGREL}\"\n"
        "arch=(\"${_PACSMITH_ARCH}\")\n"
        "source=(\"${_PACSMITH_SOURCE}\")\n"
        "sha256sums=(\"${_PACSMITH_SHA256}\")\n"
        "package() {\n"
        "  install -Dm755 \"$srcdir/${_PACSMITH_SOURCE}\" \"$pkgdir/usr/bin/vendorctl\"\n"
        "}\n");
    QVERIFY2(store.saveCustomPkgbuild(first->project, *tracker, custom, &error), qPrintable(error));

    pacsmith::ImportOptions secondOptions = firstOptions;
    secondOptions.version = QStringLiteral("2.0.0");
    secondOptions.acquisition.originalUrl = QStringLiteral("https://example.invalid/v2");
    secondOptions.acquisition.githubReleaseId = 20;
    secondOptions.acquisition.githubAssetId = 21;
    auto imported = store.importSource(
        std::filesystem::path(secondPath.toUtf8().constData()), secondOptions, &error);
    QVERIFY2(imported.has_value(), qPrintable(error));
    const auto *updated = imported->project.release(imported->releaseId);
    QVERIFY(updated != nullptr);
    QVERIFY(updated->pkgbuildManuallyModified);
    QCOMPARE(updated->customPkgbuild, custom);
    QCOMPARE(store.readPkgbuild(*updated, &error).value_or(QString{}), custom);
    const auto *historical = imported->project.release(first->releaseId);
    QVERIFY(historical != nullptr);
    QCOMPARE(store.readPkgbuild(*historical, &error).value_or(QString{}), custom);
    QFile nextVars(QString::fromUtf8(store.identityVariablesPath(*updated).string().c_str()));
    QVERIFY(nextVars.open(QIODevice::ReadOnly));
    const auto nextIdentity = QString::fromUtf8(nextVars.readAll());
    QVERIFY(nextIdentity.contains("_PACSMITH_SOURCE='vendorctl-2.0.0-linux-x86_64'"));
    QVERIFY(!nextIdentity.contains("vendorctl-1.0.0-linux-x86_64"));

    const auto plan = pacsmith::PkgbuildInstallPlan::parse(custom, *updated);
    QVERIFY2(plan.warnings.isEmpty(), qPrintable(plan.warnings.join(QLatin1Char('\n'))));
    QVERIFY(std::any_of(plan.entries.cbegin(), plan.entries.cend(), [](const auto &entry) {
        return entry.path == QStringLiteral("/usr/bin/vendorctl") && !entry.excluded;
    }));
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
