#include "core_tests.hpp"

#include "core/app_settings.hpp"
#include "core/background_updates.hpp"
#include "core/apt_sources.hpp"
#include "core/control_parser.hpp"
#include "core/dependency_parser.hpp"
#include "core/deb_analyzer.hpp"
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

void CoreTests::serializesImportedSigningKeyContents() {
    pacsmith::RepositorySigningKey key;
    key.relativePath = QStringLiteral("files/keys/vendor-abcd.gpg");
    key.sha256 = QString(64, QLatin1Char('a'));
    key.fingerprints = {QStringLiteral("ABCDEF0123456789ABCDEF0123456789ABCDEF01")};
    key.trusted = true;
    key.contents = QByteArrayLiteral("normalized-keyring");
    const auto json = key.toJson();
    QCOMPARE(json.value(QStringLiteral("contents")).toString(),
             QString::fromLatin1(key.contents.toBase64()));
    const auto restored = pacsmith::RepositorySigningKey::fromJson(json);
    QCOMPARE(restored.contents, key.contents);
    QCOMPARE(restored.trusted, true);
    key.contents.clear();
    QVERIFY(!key.toJson().contains(QStringLiteral("contents")));
}
