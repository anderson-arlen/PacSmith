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

void CoreTests::mapsKnownDependencies() {
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

void CoreTests::comparesDebianVersions() {
    QVERIFY(pacsmith::DebianVersion::compare(QStringLiteral("1.0~rc1"), QStringLiteral("1.0")) < 0);
    QVERIFY(pacsmith::DebianVersion::compare(QStringLiteral("1:1.0"), QStringLiteral("2.0")) > 0);
    QVERIFY(pacsmith::DebianVersion::compare(QStringLiteral("1.0-2"), QStringLiteral("1.0-1")) > 0);
    QCOMPARE(pacsmith::DebianVersion::compare(QStringLiteral("1.0"), QStringLiteral("1.0-0")), 0);
    QVERIFY(pacsmith::DebianVersion::compare(QStringLiteral("1.0+git1"), QStringLiteral("1.0")) > 0);
    QVERIFY(pacsmith::DebianVersion::compare(QStringLiteral("1.10"), QStringLiteral("1.9")) > 0);
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
