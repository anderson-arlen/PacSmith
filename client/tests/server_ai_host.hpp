#pragma once
#include "core/server_ai.hpp"
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QProcess>
#include <QTemporaryDir>
#include <QThread>

namespace pacsmith::tests {
class ServerAiHost final {
  public:
    explicit ServerAiHost(const std::optional<HarnessProfile> &profile = std::nullopt) {
        previousConfig_ = qgetenv("XDG_CONFIG_HOME");
        qputenv("XDG_CONFIG_HOME", directory_.filePath(QStringLiteral("client-config")).toUtf8());
        connection.socketPath = directory_.filePath(QStringLiteral("library.sock"));
        if (!connection.save())
            qFatal("Cannot save test connection");
        auto env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("DBUS_SESSION_BUS_ADDRESS"),
                   QStringLiteral("unix:path=") + directory_.filePath(QStringLiteral("missing-bus")));
        env.insert(QStringLiteral("CODEX_HOME"), directory_.filePath(QStringLiteral("auth")));
        process_.setProcessEnvironment(env);
        process_.start(QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("pacsmithd")),
                       {QStringLiteral("--data-home"), directory_.filePath(QStringLiteral("data")),
                        QStringLiteral("--config-home"), directory_.filePath(QStringLiteral("config")),
                        QStringLiteral("--state-home"), directory_.filePath(QStringLiteral("state")),
                        QStringLiteral("--runtime-dir"), directory_.filePath(QStringLiteral("runtime")),
                        QStringLiteral("--socket"), connection.socketPath});
        if (!process_.waitForStarted())
            qFatal("Cannot start test daemon");
        QElapsedTimer timer;
        timer.start();
        while (!ServerAi(connection).request(QStringLiteral("GET"), QStringLiteral("/settings"))) {
            if (timer.elapsed() > 10000 || process_.state() == QProcess::NotRunning)
                qFatal("Test daemon failed: %s", process_.readAllStandardError().constData());
            QThread::msleep(20);
        }
        if (profile && !ServerAi(connection).setHarness(profile))
            qFatal("Cannot configure test server agent");
    }
    ~ServerAiHost() {
        process_.terminate();
        if (!process_.waitForFinished(5000)) {
            process_.kill();
            process_.waitForFinished();
        }
        if (qgetenv("XDG_CONFIG_HOME") == directory_.filePath(QStringLiteral("client-config")).toUtf8()) {
            if (previousConfig_.isNull())
                qunsetenv("XDG_CONFIG_HOME");
            else
                qputenv("XDG_CONFIG_HOME", previousConfig_);
        }
    }
    ConnectionConfig connection;

  private:
    QTemporaryDir directory_;
    QProcess process_;
    QByteArray previousConfig_;
};
} // namespace pacsmith::tests
