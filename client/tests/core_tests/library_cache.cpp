#include "core_tests.hpp"
#include "core/library_cache.hpp"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QUuid>
#include <QtTest>
#include <chrono>
#include <filesystem>

void CoreTests::prunesDeletedLibraryCacheAndPreservesLiveFiles() {
    QTemporaryDir data;
    QVERIFY(data.isValid());
    const auto previous = qgetenv("XDG_DATA_HOME");
    const auto restore = qScopeGuard([&] {
        if (previous.isNull()) qunsetenv("XDG_DATA_HOME");
        else qputenv("XDG_DATA_HOME", previous);
    });
    qputenv("XDG_DATA_HOME", data.path().toUtf8());
    const auto id = [] { return QUuid::createUuid().toString(QUuid::WithoutBraces); };
    pacsmith::Project project;
    project.id = id();
    pacsmith::PackageRelease release;
    release.id = id();
    release.sourceArtifactId = id();
    release.iconArtifactId = id();
    project.releases.append(release);
    const auto removed = id();
    const auto old = id();
    const auto recent = id();
    const auto deletedRelease = id();
    const QDir cache(pacsmith::library_cache::root());
    const QStringList paths{
        release.sourceArtifactId + "-source", release.iconArtifactId + "-icon",
        removed + "-package", old + "-source", recent + "-source", "unmanaged-file",
        project.id + '/' + release.id + "/package", project.id + '/' + deletedRelease + "/package"
    };
    for (const auto &path : paths) {
        const auto absolute = cache.filePath(path);
        QVERIFY(QDir().mkpath(QFileInfo(absolute).absolutePath()));
        QFile file(absolute);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write("cached data"), 11);
        QVERIFY(file.flush());
        QVERIFY(file.setFileTime(QDateTime::currentDateTimeUtc().addDays(-2), QFileDevice::FileModificationTime));
    }
    for (const auto &path : {recent + "-source", removed + "-package"}) {
        QFile file(cache.filePath(path));
        QVERIFY(file.open(QIODevice::ReadOnly));
        QVERIFY(file.setFileTime(QDateTime::currentDateTimeUtc(), QFileDevice::FileModificationTime));
    }
    for (const auto &releaseId : {release.id, deletedRelease}) {
        std::error_code error;
        std::filesystem::last_write_time(cache.filePath(project.id + '/' + releaseId).toStdString(),
            std::filesystem::file_time_type::clock::now() - std::chrono::hours(48), error);
        QVERIFY(!error);
    }
    pacsmith::library_cache::prune({project}, {removed, release.sourceArtifactId});
    for (const auto &path : {release.sourceArtifactId + "-source", release.iconArtifactId + "-icon",
                             recent + "-source", project.id + '/' + release.id + "/package"}) {
        QVERIFY2(QFileInfo::exists(cache.filePath(path)), qPrintable(path));
    }
    QVERIFY(QFileInfo::exists(cache.filePath("unmanaged-file")));
    QVERIFY(!QFileInfo::exists(cache.filePath(removed + "-package")));
    QVERIFY(!QFileInfo::exists(cache.filePath(old + "-source")));
    QVERIFY(!QFileInfo::exists(cache.filePath(project.id + '/' + deletedRelease)));
    pacsmith::library_cache::removeRelease(project.id, release.id);
    QVERIFY(!QFileInfo::exists(cache.filePath(project.id + '/' + release.id)));
    pacsmith::library_cache::removeRelease("..", "..");
    QVERIFY(QFileInfo::exists(cache.filePath("unmanaged-file")));
}
