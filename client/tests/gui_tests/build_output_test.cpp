#include "gui/build_output_dialog.hpp"
#include <QDialogButtonBox>
#include <QJsonDocument>
#include <QLabel>
#include <QLocalServer>
#include <QLocalSocket>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTest>
#include <QUrlQuery>
#include <utility>

using namespace pacsmith;
using namespace pacsmith::gui;

class BuildServer final : public QObject {
public:
    QLocalServer server;
    QByteArray log{"Output emitted before opening the viewer\n"};
    QDateTime started{QDateTime::currentDateTimeUtc().addSecs(-125)};
    QDateTime finished;
    QString status{QStringLiteral("running")};
    int cancellations{0};
    bool failNextLog{false};

    BuildServer() {
        connect(&server, &QLocalServer::newConnection, this, [this] {
            while (auto *socket = server.nextPendingConnection()) {
                connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QLocalSocket::readyRead, socket, [this, socket, buffer = QByteArray{}]() mutable {
                    buffer += socket->readAll();
                    if (!buffer.contains("\r\n\r\n")) return;
                    const auto path = buffer.split(' ').value(1);
                    QJsonObject response;
                    int code = 200;
                    if (path.startsWith("/api/v1/jobs/build/log?")) {
                        if (std::exchange(failNextLog, false)) {
                            code = 503;
                            response.insert(QStringLiteral("error"), QStringLiteral("Temporary log failure"));
                        } else {
                            const auto after = QUrlQuery(QUrl(QString::fromUtf8(path))).queryItemValue(QStringLiteral("after")).toLongLong();
                            response = {{QStringLiteral("chunk"), QString::fromUtf8(log.mid(after))},
                                        {QStringLiteral("offset"), log.size()}};
                        }
                    } else if (path == "/api/v1/jobs/build/cancel") {
                        ++cancellations;
                        status = QStringLiteral("interrupted");
                        finished = started.addSecs(132);
                    } else if (path == "/api/v1/jobs/build") {
                        response = {{QStringLiteral("id"), QStringLiteral("build")},
                                    {QStringLiteral("kind"), QStringLiteral("build")},
                                    {QStringLiteral("status"), status},
                                    {QStringLiteral("started_at"), started.toString(Qt::ISODateWithMs).replace(QStringLiteral("Z"), QStringLiteral("123456Z"))},
                                    {QStringLiteral("finished_at"), finished.toString(Qt::ISODateWithMs)}};
                    } else code = 404;
                    const auto body = QJsonDocument(response).toJson(QJsonDocument::Compact);
                    socket->write("HTTP/1.1 " + QByteArray::number(code) + " OK\r\nContent-Type: application/json\r\nContent-Length: " +
                                  QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
                    socket->disconnectFromServer();
                });
            }
        });
    }
};

class BuildOutputTest final : public QObject {
    Q_OBJECT
private slots:
    void reopensWithHistoryAndBuildTimeWithoutCanceling() {
        QTemporaryDir directory;
        BuildServer server;
        ConnectionConfig connection;
        connection.socketPath = directory.filePath(QStringLiteral("build.sock"));
        QVERIFY2(server.server.listen(connection.socketPath), qPrintable(server.server.errorString()));
        const auto elapsed = [](const BuildOutputDialog &dialog) {
            for (auto *label : dialog.findChildren<QLabel *>()) {
                if (label->text().startsWith(QStringLiteral("Elapsed:"))) return label->text();
            }
            return QString{};
        };
        for (int closeMethod = 0; closeMethod < 3; ++closeMethod) {
            BuildOutputDialog dialog(connection, QStringLiteral("build"));
            dialog.show();
            QVERIFY(!dialog.isModal());
            auto *output = dialog.findChild<QPlainTextEdit *>();
            QVERIFY(output->isVisible());
            QTRY_COMPARE(output->toPlainText(), QString::fromUtf8(server.log));
            QVERIFY(elapsed(dialog).startsWith(QStringLiteral("Elapsed: 2m")));
            QCOMPARE(dialog.findChild<QLabel *>(QStringLiteral("buildStartedAt"))->text(),
                     QStringLiteral("Started: %1").arg(server.started.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss t"))));
            server.log += QStringLiteral("New output ✓ %1\n").arg(closeMethod).toUtf8();
            QTRY_COMPARE(output->toPlainText(), QString::fromUtf8(server.log));
            if (closeMethod == 0) dialog.close();
            else if (closeMethod == 1) QTest::keyClick(&dialog, Qt::Key_Escape);
            else dialog.findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Close)->click();
            QVERIFY(!dialog.isVisible());
            QCOMPARE(server.cancellations, 0);
            QCOMPARE(server.status, QStringLiteral("running"));
        }
        BuildOutputDialog dialog(connection, QStringLiteral("build"));
        dialog.show();
        auto *output = dialog.findChild<QPlainTextEdit *>();
        QTRY_COMPARE(output->toPlainText(), QString::fromUtf8(server.log));
        server.failNextLog = true;
        server.log += "Final output before cancellation\n";
        auto *cancel = dialog.findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Cancel);
        QCOMPARE(cancel->text(), QStringLiteral("Cancel build"));
        cancel->click();
        QTRY_COMPARE(server.cancellations, 1);
        QTRY_VERIFY(dialog.isFinished());
        QCOMPARE(output->toPlainText(), QString::fromUtf8(server.log));
        QCOMPARE(elapsed(dialog), QStringLiteral("Elapsed: 2m 12s"));
        if (qEnvironmentVariableIsSet("PACSMITH_TEST_BUILD_SCREENSHOT")) dialog.grab().save(qEnvironmentVariable("PACSMITH_TEST_BUILD_SCREENSHOT"));
    }
};

QTEST_MAIN(BuildOutputTest)
#include "build_output_test.moc"
