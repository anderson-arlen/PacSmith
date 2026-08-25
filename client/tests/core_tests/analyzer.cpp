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
    QVERIFY(pkgbuild.contains(QStringLiteral("pacsmith.artifact=${_PACSMITH_ARTIFACT}")));
    QVERIFY(pkgbuild.contains(QStringLiteral("bsdtar -xpf \"$srcdir/${_PACSMITH_SOURCE}\"")));
    QVERIFY(pacsmith::PkgbuildGenerator::identityVariables(release).contains(
        QStringLiteral("_PACSMITH_ARTIFACT='rpm'")));
    QVERIFY(pacsmith::PkgbuildGenerator::identityVariables(release).contains(
        QStringLiteral("_PACSMITH_SOURCE='vendor.rpm'")));
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

void CoreTests::rewritesDesktopIconFields() {
    pacsmith::IconConfiguration icon;
    icon.iconName = QStringLiteral("brave-browser-bin");
    icon.format = QStringLiteral("png");
    icon.sha256 = QString(64, QLatin1Char('a'));
    QCOMPARE(icon.installedPath(),
             QStringLiteral("/usr/share/icons/hicolor/256x256/apps/brave-browser-bin.png"));
    icon.format = QStringLiteral("svg");
    QCOMPARE(icon.installedPath(),
             QStringLiteral("/usr/share/icons/hicolor/scalable/apps/brave-browser-bin.svg"));
    QVERIFY(icon.isConfigured());
    icon.sha256.clear();
    QVERIFY(!icon.isConfigured());
    icon.sha256 = QString(64, QLatin1Char('a'));
    icon.missing = true;
    QVERIFY(!icon.isConfigured());

    const auto rewritten = pacsmith::withDesktopEntryField(
        QStringLiteral("[Desktop Entry]\nName=Brave\nExec=brave %U\nIcon=brave-browser\n"),
        QStringLiteral("Icon"),
        QStringLiteral("brave-browser-bin"));
    QCOMPARE(pacsmith::desktopEntryField(rewritten, QStringLiteral("Icon")),
             QStringLiteral("brave-browser-bin"));
    QCOMPARE(pacsmith::desktopEntryField(rewritten, QStringLiteral("Name")),
             QStringLiteral("Brave"));

    QList<pacsmith::DesktopEntryConfiguration> entries;
    pacsmith::DesktopEntryConfiguration enabled;
    enabled.enabled = true;
    enabled.contents = QStringLiteral("[Desktop Entry]\nName=Brave\nIcon=brave-browser\n");
    pacsmith::DesktopEntryConfiguration disabled;
    disabled.enabled = false;
    disabled.contents = QStringLiteral("[Desktop Entry]\nName=Brave Beta\nIcon=brave-browser-beta\n");
    entries.append(enabled);
    entries.append(disabled);
    QCOMPARE(pacsmith::applyDesktopIconName(entries, QStringLiteral("brave-browser-bin")), 1);
    QCOMPARE(pacsmith::desktopEntryField(entries.at(0).contents, QStringLiteral("Icon")),
             QStringLiteral("brave-browser-bin"));
    QVERIFY(entries.at(0).userModified);
    QCOMPARE(pacsmith::desktopEntryField(entries.at(1).contents, QStringLiteral("Icon")),
             QStringLiteral("brave-browser-beta"));
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
    const auto storedAppRun = std::find_if(
        analyzed->payload.cbegin(), analyzed->payload.cend(), [](const auto &entry) {
            return entry.path == QStringLiteral("AppRun");
        });
    QVERIFY(storedAppRun != analyzed->payload.cend());
    QVERIFY(storedAppRun->executable);
    QCOMPARE(storedAppRun->contentSha256, inspectedAppRun->contentSha256);
    QCOMPARE(storedAppRun->textPreview, QStringLiteral("#!/bin/sh\nexit 0\n"));
}

void CoreTests::acceptsExecutableSymlinkAppRun() {
    const auto mksquashfs = QStandardPaths::findExecutable(QStringLiteral("mksquashfs"));
    const auto unsquashfs = QStandardPaths::findExecutable(QStringLiteral("unsquashfs"));
    if (mksquashfs.isEmpty() || unsquashfs.isEmpty()) {
        QSKIP("squashfs-tools is required for the AppImage analyzer regression test");
    }

    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto appDir = temporary.filePath(QStringLiteral("AppDir"));
    const auto target = appDir + QStringLiteral("/Letos/letos");
    QVERIFY(QDir().mkpath(QFileInfo(target).path()));
    QFile executable(target);
    QVERIFY(executable.open(QIODevice::WriteOnly));
    QCOMPARE(executable.write(QByteArrayLiteral("#!/bin/sh\nexit 0\n")), 17);
    executable.close();
    QVERIFY(QFile::setPermissions(
        target, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                    QFileDevice::ExeOwner | QFileDevice::ReadGroup |
                    QFileDevice::ExeGroup | QFileDevice::ReadOther |
                    QFileDevice::ExeOther));
    std::error_code linkError;
    std::filesystem::create_symlink(
        std::filesystem::path("Letos/letos"),
        std::filesystem::path((appDir + QStringLiteral("/AppRun")).toUtf8().constData()),
        linkError);
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
    const auto appImagePath = temporary.filePath(QStringLiteral("Letos-4.0.3-x86_64.AppImage"));
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
    QVERIFY(analyzed->installMapping.appRun.present);
    QVERIFY(!analyzed->installMapping.appRun.script);
    QCOMPARE(analyzed->installMapping.appRun.reviewReason,
             QStringLiteral("Symlink AppRun; not a text script"));
    const auto appRun = std::find_if(
        analyzed->payload.cbegin(), analyzed->payload.cend(), [](const auto &entry) {
            return entry.path == QStringLiteral("AppRun");
        });
    QVERIFY(appRun != analyzed->payload.cend());
    QCOMPARE(appRun->type, QStringLiteral("symlink"));
    QCOMPARE(appRun->symlinkTarget, QStringLiteral("Letos/letos"));
    QVERIFY(appRun->executable);
    QCOMPARE(analyzed->installMapping.launchers.size(), 1);
    QCOMPARE(analyzed->installMapping.launchers.first().sourcePath,
             QStringLiteral("AppRun"));
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
