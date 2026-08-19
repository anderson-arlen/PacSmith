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

