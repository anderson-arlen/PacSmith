#include "core/automatic_update_review.hpp"
#include "core/harness_launcher.hpp"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalServer>
#include <QLocalSocket>
#include <QTemporaryDir>
#include <QTest>
#include <QtConcurrent>

using namespace pacsmith;

class ReviewServer final : public QObject {
public:
    QLocalServer server;
    Project project;
    int claims{0};

    explicit ReviewServer(const QString &path) {
        project.id = QStringLiteral("project");
        project.archPackageName = QStringLiteral("pacsmith-review-test");
        project.autoBuildPolicy = AutoBuildPolicy::Ai;
        PackageRelease candidate;
        candidate.id = QStringLiteral("release");
        candidate.revision = 1;
        candidate.update.lastAutomaticStatus = QStringLiteral("ai-pending");
        project.releases.append(candidate);
        connect(&server, &QLocalServer::newConnection, this, [this] {
            while (auto *socket = server.nextPendingConnection()) {
                connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QLocalSocket::readyRead, socket, [this, socket, buffer = QByteArray{}]() mutable {
                    buffer += socket->readAll();
                    const auto headerEnd = buffer.indexOf("\r\n\r\n");
                    if (headerEnd < 0) return;
                    qsizetype size = 0;
                    for (const auto &header : buffer.left(headerEnd).split('\n')) {
                        if (header.toLower().startsWith("content-length:")) size = header.mid(15).trimmed().toInt();
                    }
                    if (buffer.size() < headerEnd + 4 + size) return;
                    int status = 200;
                    QJsonObject response;
                    if (buffer.startsWith("GET /api/v1/projects/project ")) {
                        response = project.toJson();
                        response.insert(QStringLiteral("releases"), QJsonArray{project.releases.first().toJson()});
                    } else if (buffer.startsWith("PATCH /api/v1/releases/release/configuration ")) {
                        const auto body = QJsonDocument::fromJson(buffer.mid(headerEnd + 4, size)).object();
                        auto &release = project.releases.first();
                        if (body.value(QStringLiteral("revision")).toInteger() != release.revision) {
                            status = 409;
                        } else {
                            auto document = release.toJson();
                            document.insert(QStringLiteral("update"), body.value(QStringLiteral("configuration"))
                                .toObject().value(QStringLiteral("update")));
                            document.insert(QStringLiteral("revision"), release.revision + 1);
                            release = PackageRelease::fromJson(document);
                            if (release.update.lastAutomaticStatus == QStringLiteral("ai-reviewing")) ++claims;
                            response = document;
                        }
                    } else {
                        status = 404;
                    }
                    const auto body = QJsonDocument(response).toJson(QJsonDocument::Compact);
                    socket->write("HTTP/1.1 " + QByteArray::number(status) + " OK\r\nContent-Type: application/json\r\nContent-Length: " +
                                  QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
                    socket->disconnectFromServer();
                });
            }
        });
        if (!server.listen(path)) qFatal("Could not listen on test socket");
    }
};

class AutomaticUpdateReviewTest final : public QObject {
    Q_OBJECT
private slots:
    void claimsOnceWithTheReleasePrompt() {
        QTemporaryDir directory;
        const auto socketPath = directory.filePath(QStringLiteral("library.sock"));
        ReviewServer server(socketPath);
        ConnectionConfig config;
        config.socketPath = socketPath;
        const LibraryClient client(config);
        const auto reviews = directory.filePath(QStringLiteral("reviews"));
        const QList<Project> summaries{server.project};
        AppSettings settings;
        HarnessProfile profile;
        profile.name = QStringLiteral("Test ACP agent");
        profile.executable = QStringLiteral("test-agent");
        settings.harness = profile;
        auto first = QtConcurrent::run([=] { return claimPendingUpdateReviews(client, summaries, settings, reviews); });
        auto second = QtConcurrent::run([=] { return claimPendingUpdateReviews(client, summaries, settings, reviews); });
        QTRY_VERIFY_WITH_TIMEOUT(first.isFinished() && second.isFinished(), 10000);
        QCOMPARE(server.claims, 1);
        QCOMPARE(server.project.releases.first().update.lastAutomaticStatus, QStringLiteral("ai-reviewing"));
        const auto requests = first.result() + second.result();
        QCOMPARE(requests.size(), 1);
        QCOMPARE(requests.first().projectId, QStringLiteral("project"));
        QCOMPARE(requests.first().releaseId, QStringLiteral("release"));
        QCOMPARE(requests.first().connection.socketPath, socketPath);
        QCOMPARE(requests.first().prompt,
                 HarnessLauncher::automaticUpdatePrompt(QStringLiteral("project"), QStringLiteral("release"), false));
        auto repeat = QtConcurrent::run([=] { return claimPendingUpdateReviews(client, summaries, settings, reviews); });
        QTRY_VERIFY_WITH_TIMEOUT(repeat.isFinished(), 10000);
        QCOMPARE(server.claims, 1);
        const QList<Project> recoveringSummaries{server.project};
        auto recovery = QtConcurrent::run([=] { return claimPendingUpdateReviews(client, recoveringSummaries, settings, reviews, true); });
        QTRY_VERIFY_WITH_TIMEOUT(recovery.isFinished(), 10000);
        QCOMPARE(recovery.result().size(), 1);
        QCOMPARE(server.claims, 2);
    }

    void missingProfilesWaitForAcpConfiguration() {
        QTemporaryDir directory;
        const auto socketPath = directory.filePath(QStringLiteral("library.sock"));
        ReviewServer server(socketPath);
        ConnectionConfig config;
        config.socketPath = socketPath;
        const LibraryClient client(config);
        const auto reviews = directory.filePath(QStringLiteral("reviews"));
        const QList<Project> summaries{server.project};
        AppSettings settings;
        HarnessProfile profile;
        profile.name = QStringLiteral("Missing executable");
        profile.executable = directory.filePath(QStringLiteral("does-not-exist"));
        auto incomplete = QtConcurrent::run([=] { return claimPendingUpdateReviews(client, summaries, settings, reviews); });
        QTRY_VERIFY_WITH_TIMEOUT(incomplete.isFinished(), 10000);
        QCOMPARE(server.claims, 0);
        QCOMPARE(server.project.releases.first().update.lastAutomaticStatus, QStringLiteral("ai-pending"));
        QVERIFY(server.project.releases.first().update.lastAutomaticMessage.contains(QStringLiteral("ACP")));
        settings.harness = profile;
        auto claimed = QtConcurrent::run([=] { return claimPendingUpdateReviews(client, summaries, settings, reviews); });
        QTRY_VERIFY_WITH_TIMEOUT(claimed.isFinished(), 10000);
        QCOMPARE(server.claims, 1);
        QCOMPARE(claimed.result().size(), 1);
        QCOMPARE(server.project.releases.first().update.lastAutomaticStatus, QStringLiteral("ai-reviewing"));

    }
};

QTEST_GUILESS_MAIN(AutomaticUpdateReviewTest)
#include "automatic_update_review_test.moc"
