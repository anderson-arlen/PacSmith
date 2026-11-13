#include "../server_ai_host.hpp"
#include "core/managed_package.hpp"
#include "core/project_store/internal.hpp"
#include "gui/main_window/main_window.hpp"
#include "gui/main_window/support.hpp"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QLabel>
#include <QPlainTextEdit>
#include <QProcess>
#include <QProcessEnvironment>
#include <QScopeGuard>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTest>
#include <algorithm>

namespace pacsmith::gui {
namespace {
QByteArray run(const QString &program, const QStringList &arguments) {
    QProcess process;
    process.start(program, arguments);
    if (!process.waitForFinished(30000) || process.exitCode() != 0)
        qFatal("%s failed: %s", qPrintable(program), process.readAllStandardError().constData());
    return process.readAllStandardOutput();
}

void write(const QString &path, const QByteArray &contents) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(contents) != contents.size())
        qFatal("Cannot write fixture %s", qPrintable(path));
}

QString appImage(const QString &directory, const QByteArray &color) {
    const auto appDir = directory + QStringLiteral("/AppDir");
    write(appDir + QStringLiteral("/AppRun"), "#!/bin/sh\nexec /usr/bin/true\n");
    QFile::setPermissions(appDir + QStringLiteral("/AppRun"), QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    write(appDir + QStringLiteral("/word-squadron.desktop"),
          "[Desktop Entry]\nType=Application\nName=Word Squadron regression\nExec=AppRun\nIcon=word-squadron\nCategories=Game;\n");
    write(appDir + QStringLiteral("/word-squadron.svg"),
          "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"32\" height=\"32\"><rect width=\"32\" height=\"32\" fill=\"" + color + "\"/></svg>");
    const auto squash = directory + QStringLiteral("/payload.squashfs");
    run(QStringLiteral("mksquashfs"), {appDir, squash, QStringLiteral("-noappend"), QStringLiteral("-processors"), QStringLiteral("1"), QStringLiteral("-quiet")});
    QFile payload(squash);
    if (!payload.open(QIODevice::ReadOnly)) qFatal("Cannot read squashfs fixture");
    QByteArray header(4096, '\0');
    header.replace(0, 4, QByteArrayLiteral("\x7f" "ELF"));
    header.replace(8, 3, QByteArrayLiteral("AI\x02"));
    const auto path = directory + QStringLiteral("/Word_Squadron-0.1.0+1-x86_64.AppImage");
    write(path, header + payload.readAll());
    return path;
}

QJsonObject mcp(const ConnectionConfig &connection, const QString &tool, const QJsonObject &arguments) {
    QProcess process;
    auto environment = QProcessEnvironment::systemEnvironment();
    const auto directory = QFileInfo(connection.socketPath).absolutePath();
    // User services must not hide a subprocess that uses the default daemon instead of the fixture.
    environment.insert(QStringLiteral("XDG_RUNTIME_DIR"), directory);
    environment.insert(QStringLiteral("DBUS_SESSION_BUS_ADDRESS"),
                       QStringLiteral("unix:path=") + directory + QStringLiteral("/missing-bus"));
    environment.insert(QStringLiteral("PACSMITH_MCP_MODE"), QStringLiteral("local"));
    environment.insert(QStringLiteral("PACSMITH_MCP_SOCKET"), connection.socketPath);
    process.setProcessEnvironment(environment);
    process.start(QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("pacsmith")), {QStringLiteral("mcp")});
    if (!process.waitForStarted()) qFatal("Cannot start MCP test client");
    const QJsonArray requests{
        QJsonObject{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")}, {QStringLiteral("id"), 1},
            {QStringLiteral("method"), QStringLiteral("initialize")},
            {QStringLiteral("params"), QJsonObject{{QStringLiteral("protocolVersion"), QStringLiteral("2025-11-25")}}}},
        QJsonObject{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
            {QStringLiteral("method"), QStringLiteral("notifications/initialized")}},
        QJsonObject{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")}, {QStringLiteral("id"), 2},
            {QStringLiteral("method"), QStringLiteral("tools/call")},
            {QStringLiteral("params"), QJsonObject{{QStringLiteral("name"), tool}, {QStringLiteral("arguments"), arguments}}}}};
    for (const auto &request : requests) process.write(QJsonDocument(request.toObject()).toJson(QJsonDocument::Compact) + '\n');
    process.closeWriteChannel();
    if (!process.waitForFinished(30000)) qFatal("MCP test client timed out");
    for (const auto &line : process.readAllStandardOutput().split('\n')) {
        const auto response = QJsonDocument::fromJson(line).object();
        if (response.value(QStringLiteral("id")).toInt() == 2) {
            if (response.contains(QStringLiteral("error"))) qFatal("MCP protocol error: %s", line.constData());
            return response.value(QStringLiteral("result")).toObject();
        }
    }
    qFatal("MCP returned no tool result: %s", process.readAllStandardError().constData());
}
}

class ReleaseIdentityTest final : public QObject {
    Q_OBJECT
private slots:
    void equalVersionArtifactsKeepTheirIdentity() {
        QTemporaryDir directory;
        const auto dataHome = qgetenv("XDG_DATA_HOME");
        qputenv("XDG_DATA_HOME", directory.path().toUtf8());
        const auto restore = qScopeGuard([&] {
            if (dataHome.isNull()) qunsetenv("XDG_DATA_HOME"); else qputenv("XDG_DATA_HOME", dataHome);
        });
        tests::ServerAiHost host;
        LibraryClient client(host.connection);
        ImportOptions options;
        options.packageName = QStringLiteral("word-squadron-regression-bin");
        options.version = QStringLiteral("0.1.0+1");
        QString error;
        const auto first = client.importSource(appImage(directory.filePath(QStringLiteral("older")), "#e02020"), options, &error);
        QVERIFY2(first, qPrintable(error));
        const auto olderId = first->releaseId;
        const auto compatible = mcp(host.connection, QStringLiteral("set_package_metadata"),
            {{QStringLiteral("project_name"), first->project.archPackageName},
             {QStringLiteral("release_name"), QStringLiteral("0.1.0+1")},
             {QStringLiteral("description"), QStringLiteral("Compatible name selector")}});
        QVERIFY(!compatible.value(QStringLiteral("isError")).toBool());
        options.existingProjectId = first->project.id;
        QTest::qWait(10);
        const auto second = client.importSource(appImage(directory.filePath(QStringLiteral("newer")), "#2020e0"), options, &error);
        QVERIFY2(second, qPrintable(error));
        const auto newerId = second->releaseId;
        auto project = second->project;
        const auto *older = project.release(olderId);
        const auto *newer = project.release(newerId);
        QVERIFY(older && newer);
        QCOMPARE(older->debian.version, newer->debian.version);
        QCOMPARE(older->originalSourceFilename, newer->originalSourceFilename);
        QVERIFY(older->sourceSha256 != newer->sourceSha256);
        QVERIFY(older->installMapping.icon.sha256 != newer->installMapping.icon.sha256);
        QVERIFY(older->createdAt < newer->createdAt);
        QCOMPARE(project.newestRelease()->id, newerId);
        auto reordered = project;
        std::reverse(reordered.releases.begin(), reordered.releases.end());
        QCOMPARE(reordered.newestRelease()->id, newerId);
        reordered.release(newerId)->debian.version = QStringLiteral("0.0.9");
        QCOMPARE(reordered.newestRelease()->id, olderId);

        const QJsonObject labels{{QStringLiteral("project_name"), project.archPackageName},
                                 {QStringLiteral("release_name"), QStringLiteral("0.1.0+1")}};
        for (const auto &selector : {QStringLiteral("0.1.0+1"), newer->originalSourceFilename}) {
            auto arguments = labels;
            arguments.insert(QStringLiteral("release_name"), selector);
            arguments.insert(QStringLiteral("description"), QStringLiteral("ambiguous"));
            const auto response = mcp(host.connection, QStringLiteral("set_package_metadata"), arguments);
            QVERIFY(response.value(QStringLiteral("isError")).toBool());
            const auto text = QJsonDocument(response).toJson();
            QVERIFY(text.contains(olderId.toUtf8()));
            QVERIFY(text.contains(newerId.toUtf8()));
            QVERIFY(text.contains(newer->sourceSha256.toUtf8()));
        }
        for (const auto &id : {olderId, newerId}) {
            auto arguments = labels;
            arguments.insert(QStringLiteral("release_id"), id);
            arguments.insert(QStringLiteral("description"), id);
            const auto response = mcp(host.connection, QStringLiteral("set_package_metadata"), arguments);
            QVERIFY2(!response.value(QStringLiteral("isError")).toBool(), QJsonDocument(response).toJson().constData());
        }
        QVERIFY(mcp(host.connection, QStringLiteral("set_package_metadata"),
            {{QStringLiteral("release_id"), newerId}, {QStringLiteral("description"), QStringLiteral("missing labels")}})
                .value(QStringLiteral("isError")).toBool());
        auto mismatch = labels;
        mismatch.insert(QStringLiteral("release_id"), newerId);
        mismatch.insert(QStringLiteral("release_name"), QStringLiteral("wrong version"));
        mismatch.insert(QStringLiteral("description"), QStringLiteral("mismatched label"));
        QVERIFY(mcp(host.connection, QStringLiteral("set_package_metadata"), mismatch).value(QStringLiteral("isError")).toBool());
        project = *client.load(project.id, &error);
        QCOMPARE(project.release(olderId)->packageMetadata.description, olderId);
        QCOMPARE(project.release(newerId)->packageMetadata.description, newerId);
        for (const auto &release : project.releases)
            QVERIFY2(!client.cacheArtifact(release.iconArtifactId, QStringLiteral("icon"), &error).isEmpty(), qPrintable(error));
        QCOMPARE(projectIcon(client, project).pixmap(32, 32).toImage().pixelColor(16, 16), QColor(QStringLiteral("#2020e0")));

        AppSettingsStore store(directory.filePath(QStringLiteral("settings")));
        MainWindow window(store);
        window.applyLoadedProject(project, true);
        QCOMPARE(window.currentReleaseId_, newerId);
        window.currentReleaseId_ = olderId;
        window.populateOverview();
        QCOMPARE(window.releaseTable_->item(0, 0)->data(Qt::UserRole).toString(), newerId);
        QString previousPackageVersion;
        for (const auto &id : {olderId, olderId, newerId, olderId}) {
            window.currentReleaseId_ = id;
            window.populatePkgbuild();
            window.populateOverview();
            QCOMPARE(window.currentReleaseId_, id);
            window.startBuild(false);
            QVERIFY2(!window.buildJobId_.isEmpty(), qPrintable(error));
            QCOMPARE(window.buildReleaseId_, id);
            const auto job = client.getJob(window.buildJobId_, &error);
            QVERIFY2(job, qPrintable(error));
            QCOMPARE(job->releaseId, id);
            QTRY_VERIFY_WITH_TIMEOUT(window.buildJobId_.isEmpty(), 60000);
            project = *client.load(project.id, &error);
            const auto *built = project.release(id);
            QVERIFY(built && !built->builds.isEmpty());
            QVERIFY2(built->builds.last().status == BuildStatus::Succeeded, qPrintable(built->lastBuildLog));
            QVERIFY(built->buildStatus != BuildStatus::Building);
            QVERIFY(project.release(id == olderId ? newerId : olderId)->buildStatus != BuildStatus::Building);
            const auto artifactId = built->preferredBuiltArtifactId();
            QVERIFY(!artifactId.isEmpty());
            const auto path = acquireRetainedPackagePath(client, *built, &error);
            QVERIFY2(!path.isEmpty(), qPrintable(error));
            const auto pkginfo = run(QStringLiteral("bsdtar"), {QStringLiteral("-xOf"), path, QStringLiteral(".PKGINFO")});
            QVERIFY(pkginfo.contains("pacsmith.release=" + id.toUtf8()));
            const auto iconPath = built->installMapping.icon.installedPath().mid(1);
            const auto icon = run(QStringLiteral("bsdtar"), {QStringLiteral("-xOf"), path, iconPath});
            QVERIFY2(icon.contains(id == newerId ? "#2020e0" : "#e02020"),
                qPrintable(QStringLiteral("Release %1, icon %2: %3").arg(id, iconPath, QString::fromUtf8(icon.left(800)))));
            const auto payloadIcon = run(QStringLiteral("bsdtar"), {QStringLiteral("-xOf"), path,
                QStringLiteral("opt/%1/word-squadron.svg").arg(built->installMapping.optDirectory)});
            QVERIFY(payloadIcon.contains(id == newerId ? "#2020e0" : "#e02020"));
            const auto files = run(QStringLiteral("bsdtar"), {QStringLiteral("-tf"), path}).split('\n');
            QByteArray desktop;
            for (const auto &file : files)
                if (file.startsWith("usr/share/applications/") && file.endsWith(".desktop"))
                    desktop = run(QStringLiteral("bsdtar"), {QStringLiteral("-xOf"), path, QString::fromUtf8(file)});
            QVERIFY(!desktop.isEmpty());
            const auto iconName = QFileInfo(iconPath).completeBaseName().toUtf8();
            QVERIFY2(desktop.contains("Icon=" + iconName + '\n'), desktop.constData());

            ManagedPackageInfo installed;
            installed.packageName = project.archPackageName;
            installed.packageVersion = built->builds.last().artifacts.first().packageVersion;
            if (!previousPackageVersion.isEmpty())
                QVERIFY(run(QStringLiteral("vercmp"), {installed.packageVersion, previousPackageVersion}).trimmed().toInt() > 0);
            previousPackageVersion = installed.packageVersion;
            installed.xdata = {{QStringLiteral("pacsmith.project"), project.id}, {QStringLiteral("pacsmith.release"), id}};
            QVERIFY(client.reconcileInstalled(project, {{project.archPackageName, installed}}, &error));
            QCOMPARE(project.installedReleaseId, id);
            const auto cached = client.cacheArtifact(built->iconArtifactId, QStringLiteral("icon"), &error);
            QVERIFY2(!cached.isEmpty(), qPrintable(error));
            const auto displayed = projectIcon(client, project).pixmap(32, 32).toImage().pixelColor(16, 16);
            QCOMPARE(displayed, QColor(id == newerId ? QStringLiteral("#2020e0") : QStringLiteral("#e02020")));
            auto legacy = project;
            auto *unbuilt = legacy.release(id == olderId ? newerId : olderId);
            if (unbuilt->builds.isEmpty()) {
                unbuilt->archPkgrel = built->archPkgrel;
                std::reverse(legacy.releases.begin(), legacy.releases.end());
                QCOMPARE(project_store_internal::installedReleaseIdForVersion(legacy, installed.packageVersion), id);
            }
            window.project_ = project;
        }
        const auto *oldBuild = project.release(olderId);
        const auto *newBuild = project.release(newerId);
        QVERIFY(oldBuild->archPkgrel > newBuild->archPkgrel);
        QCOMPARE(project.newestRelease()->id, newerId);
        QCOMPARE(project.release(newerId)->builds.size(), 1);
        QCOMPARE(project.release(olderId)->builds.size(), 3);
        auto ambiguousInstalled = project;
        const auto duplicateVersion = newBuild->builds.last().artifacts.first().packageVersion;
        ambiguousInstalled.release(olderId)->builds.last().artifacts.first().packageVersion = duplicateVersion;
        QVERIFY(project_store_internal::installedReleaseIdForVersion(ambiguousInstalled, duplicateVersion).isEmpty());
        QVERIFY(project_store_internal::releaseMatchesInstalledVersion(ambiguousInstalled, duplicateVersion));
        window.pendingPackageOperation_ = QStringLiteral("install");
        window.pendingPackageOperationProject_ = project;
        window.pendingPackageOperationReleaseId_ = newerId;
        window.currentReleaseId_ = olderId;
        window.showCommandProgress(QStringLiteral("Install fixture"), QStringLiteral("Testing failure association"), false);
        window.installService_.failedToStart(QStringLiteral("fixture did not invoke pacman"));
        QTRY_VERIFY_WITH_TIMEOUT([&] {
            const auto latest = client.load(project.id, &error);
            return latest && !latest->history.isEmpty() && latest->history.last().event == QStringLiteral("install");
        }(), 10000);
        const auto recorded = client.load(project.id, &error);
        QVERIFY(recorded->history.last().detail.contains(newerId));
        QVERIFY(!recorded->history.last().detail.contains(olderId));
    }
};
}

QTEST_MAIN(pacsmith::gui::ReleaseIdentityTest)
#include "release_identity_test.moc"
