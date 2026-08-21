#pragma once

#include "core/process_services.hpp"

#include <QByteArray>
#include <QLocalServer>
#include <QPointer>
#include <QProcessEnvironment>
#include <QStringList>
#include <QTimer>

#include <filesystem>
#include <optional>

class QLocalSocket;

namespace pacsmith {

struct InstallSessionEvent {
    QString type;
    QString token;
    QString text;
    int exitCode{-1};
    QProcess::ExitStatus exitStatus{QProcess::NormalExit};
    bool canceled{false};
};

class InstallSessionProtocol final {
public:
    [[nodiscard]] static QByteArray encode(const InstallSessionEvent &event);
    [[nodiscard]] static std::optional<InstallSessionEvent> decode(QByteArrayView line,
                                                                   QString *error = nullptr);
};

struct TerminalCommand {
    QString program;
    QStringList arguments;
    QString displayName;
};

class TerminalLauncher final {
public:
    [[nodiscard]] static std::optional<TerminalCommand> commandFor(
        const QString &terminalProgram, const QStringList &terminalArguments,
        const QString &helperProgram, const QStringList &helperArguments,
        QString *error = nullptr);
    [[nodiscard]] static std::optional<TerminalCommand> resolve(
        const QString &helperProgram, const QStringList &helperArguments,
        const QProcessEnvironment &environment = QProcessEnvironment::systemEnvironment(),
        QString *error = nullptr);
};

class TerminalInstallService final : public QObject {
    Q_OBJECT
public:
    explicit TerminalInstallService(QObject *parent = nullptr);

    [[nodiscard]] bool isRunning() const noexcept;
    void start(const std::filesystem::path &packagePath, const QString &pacsmithExecutable);
    void startUninstall(const QString &packageName, const QString &pacsmithExecutable);

signals:
    void progressChanged(const QString &message);
    void outputAvailable(const QString &text);
    void finished(const pacsmith::ProcessResult &result);
    void failedToStart(const QString &message);

private:
    void acceptConnection();
    void readMessages();
    void handleMessage(const InstallSessionEvent &event);
    void fail(const QString &message);
    void resetSession();
    void startSession(const QStringList &operationArguments, const QString &pacsmithExecutable,
                      const QString &description);

    QLocalServer server_;
    QPointer<QLocalSocket> socket_;
    QTimer connectionTimer_;
    QByteArray receiveBuffer_;
    QString token_;
    bool active_{false};
    bool authenticated_{false};
    ProcessResult result_;
};

} // namespace pacsmith
