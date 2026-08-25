#pragma once

#include "core/model.hpp"

#include <QDateTime>
#include <QObject>
#include <QProcess>
#include <QStringList>

#include <filesystem>
#include <optional>

namespace pacsmith {

struct ProcessResult {
    int exitCode{-1};
    QProcess::ExitStatus exitStatus{QProcess::NormalExit};
    QString output;
    QString errorOutput;
    QStringList producedPackages;
    QDateTime startedAt;
    QDateTime finishedAt;
    bool canceled{false};

    [[nodiscard]] bool succeeded() const noexcept;
};

enum class InstallPrivilegeMode {
    TtySudo,
    Polkit,
};

[[nodiscard]] std::optional<InstallPrivilegeMode> parseInstallPrivilegeOptions(
    const QStringList &options, QString *error = nullptr);

class BuildService final : public QObject {
    Q_OBJECT
public:
    explicit BuildService(QObject *parent = nullptr);
    [[nodiscard]] bool isRunning() const;
    [[nodiscard]] static QStringList makepkgArguments();
    void start(const std::filesystem::path &projectDirectory);
    void cancel();

signals:
    void outputAvailable(const QString &text);
    void finished(const pacsmith::ProcessResult &result);
    void failedToStart(const QString &message);

private:
    QProcess process_;
    std::filesystem::path directory_;
    ProcessResult result_;
};

class InstallService final : public QObject {
    Q_OBJECT
public:
    explicit InstallService(QObject *parent = nullptr);
    [[nodiscard]] bool isRunning() const;
    [[nodiscard]] static QStringList installArguments(const QString &absolutePackagePath,
                                                       bool nonInteractive);
    [[nodiscard]] static QStringList uninstallArguments(const QString &packageName,
                                                         bool nonInteractive);
    [[nodiscard]] static QString privilegeProgram(InstallPrivilegeMode mode);
    void start(const std::filesystem::path &packagePath,
               InstallPrivilegeMode mode = InstallPrivilegeMode::TtySudo);
    void startUninstall(const QString &packageName,
                        InstallPrivilegeMode mode = InstallPrivilegeMode::TtySudo);

signals:
    void progressChanged(const QString &message);
    void outputAvailable(const QString &text);
    void finished(const pacsmith::ProcessResult &result);
    void failedToStart(const QString &message);

private:
    QProcess process_;
    ProcessResult result_;
};

} // namespace pacsmith

Q_DECLARE_METATYPE(pacsmith::ProcessResult)
