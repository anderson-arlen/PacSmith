#include "core/process_services.hpp"

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QTimer>

#include <unistd.h>

namespace pacsmith {
namespace {

QStringList packageFiles(const std::filesystem::path &directory) {
    const QDir dir(QString::fromUtf8(directory.string().c_str()));
    QStringList result;
    const auto files = dir.entryInfoList(QStringList{QStringLiteral("*.pkg.tar.*")}, QDir::Files, QDir::Time);
    for (const auto &file : files) {
        if (!file.fileName().endsWith(QStringLiteral(".sig"))) result.append(file.absoluteFilePath());
    }
    return result;
}

bool validArchPackageName(const QString &name) {
    static const QRegularExpression pattern(QStringLiteral("^[a-z0-9][a-z0-9@._+\\-]*$"));
    return pattern.match(name).hasMatch();
}

} // namespace

bool ProcessResult::succeeded() const noexcept {
    return exitStatus == QProcess::NormalExit && exitCode == 0;
}

BuildService::BuildService(QObject *parent) : QObject(parent) {
    connect(&process_, &QProcess::readyReadStandardOutput, this, [this]() {
        const auto text = QString::fromLocal8Bit(process_.readAllStandardOutput());
        result_.output.append(text);
        emit outputAvailable(text);
    });
    connect(&process_, &QProcess::readyReadStandardError, this, [this]() {
        const auto text = QString::fromLocal8Bit(process_.readAllStandardError());
        result_.errorOutput.append(text);
        emit outputAvailable(text);
    });
    connect(&process_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this](const int exitCode, const QProcess::ExitStatus exitStatus) {
                const auto remainingOutput = QString::fromLocal8Bit(process_.readAllStandardOutput());
                const auto remainingErrors = QString::fromLocal8Bit(process_.readAllStandardError());
                result_.output.append(remainingOutput);
                result_.errorOutput.append(remainingErrors);
                if (!remainingOutput.isEmpty()) emit outputAvailable(remainingOutput);
                if (!remainingErrors.isEmpty()) emit outputAvailable(remainingErrors);
                result_.exitCode = exitCode;
                result_.exitStatus = exitStatus;
                result_.finishedAt = QDateTime::currentDateTimeUtc();
                result_.producedPackages = packageFiles(directory_);
                emit finished(result_);
            });
    connect(&process_, &QProcess::errorOccurred, this, [this](const QProcess::ProcessError processError) {
        if (processError == QProcess::FailedToStart) emit failedToStart(process_.errorString());
    });
}

bool BuildService::isRunning() const { return process_.state() != QProcess::NotRunning; }

QStringList BuildService::makepkgArguments() {
    // Rebuilding a changed recipe at the same pkgver/pkgrel is a normal PacSmith
    // workflow. --force only permits replacing that existing output artifact; it
    // does not bypass source hashes or package() failures. --nodeps is required
    // because PacSmith only repackages prebuilt vendor files: PKGBUILD depends=
    // are runtime requirements for pacman -U, not libraries needed to run bsdtar.
    return {QStringLiteral("--force"), QStringLiteral("--nodeps")};
}

void BuildService::start(const std::filesystem::path &projectDirectory) {
    if (isRunning()) {
        emit failedToStart(QStringLiteral("A build is already running"));
        return;
    }
    if (geteuid() == 0) {
        emit failedToStart(QStringLiteral("PacSmith refuses to run makepkg as root"));
        return;
    }
    const QFileInfo directory(QString::fromUtf8(projectDirectory.string().c_str()));
    if (!directory.exists() || !directory.isDir()) {
        emit failedToStart(QStringLiteral("Project directory does not exist: %1").arg(directory.filePath()));
        return;
    }
    result_ = {};
    result_.startedAt = QDateTime::currentDateTimeUtc();
    directory_ = projectDirectory;
    process_.setWorkingDirectory(directory.absoluteFilePath());
    process_.setProgram(QStringLiteral("/usr/bin/makepkg"));
    process_.setArguments(makepkgArguments());
    process_.start();
}

void BuildService::cancel() {
    if (!isRunning() || result_.canceled) return;
    result_.canceled = true;
    const auto message = QStringLiteral("\n[PacSmith] Cancel requested; stopping makepkg…\n");
    result_.output.append(message);
    emit outputAvailable(message);
    process_.terminate();
    QTimer::singleShot(3000, &process_, [this] {
        if (isRunning() && result_.canceled) process_.kill();
    });
}

InstallService::InstallService(QObject *parent) : QObject(parent) {
    connect(&process_, &QProcess::readyReadStandardOutput, this, [this]() {
        const auto text = QString::fromLocal8Bit(process_.readAllStandardOutput());
        result_.output.append(text);
        emit outputAvailable(text);
    });
    connect(&process_, &QProcess::readyReadStandardError, this, [this]() {
        const auto text = QString::fromLocal8Bit(process_.readAllStandardError());
        result_.errorOutput.append(text);
        emit outputAvailable(text);
    });
    connect(&process_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this](const int exitCode, const QProcess::ExitStatus exitStatus) {
                const auto remainingOutput = QString::fromLocal8Bit(process_.readAllStandardOutput());
                const auto remainingErrors = QString::fromLocal8Bit(process_.readAllStandardError());
                result_.output.append(remainingOutput);
                result_.errorOutput.append(remainingErrors);
                if (!remainingOutput.isEmpty()) emit outputAvailable(remainingOutput);
                if (!remainingErrors.isEmpty()) emit outputAvailable(remainingErrors);
                result_.exitCode = exitCode;
                result_.exitStatus = exitStatus;
                result_.finishedAt = QDateTime::currentDateTimeUtc();
                emit finished(result_);
            });
    connect(&process_, &QProcess::errorOccurred, this, [this](const QProcess::ProcessError processError) {
        if (processError == QProcess::FailedToStart) emit failedToStart(process_.errorString());
    });
}

bool InstallService::isRunning() const { return process_.state() != QProcess::NotRunning; }

QStringList InstallService::installArguments(const QString &absolutePackagePath,
                                              const bool nonInteractive) {
    QStringList arguments{QStringLiteral("/usr/bin/pacman")};
    if (nonInteractive) arguments.append(QStringLiteral("--noconfirm"));
    arguments.append({QStringLiteral("-U"), QStringLiteral("--"), absolutePackagePath});
    return arguments;
}

QStringList InstallService::uninstallArguments(const QString &packageName,
                                                const bool nonInteractive) {
    QStringList arguments{QStringLiteral("/usr/bin/pacman")};
    if (nonInteractive) arguments.append(QStringLiteral("--noconfirm"));
    arguments.append({QStringLiteral("-R"), QStringLiteral("--"), packageName});
    return arguments;
}

void InstallService::start(const std::filesystem::path &packagePath,
                           const bool nonInteractive) {
    if (isRunning()) {
        emit failedToStart(QStringLiteral("An installation is already running"));
        return;
    }
    const QFileInfo package(QString::fromUtf8(packagePath.string().c_str()));
    if (!package.isAbsolute() || !package.exists() || !package.isFile() ||
        !package.fileName().contains(QStringLiteral(".pkg.tar.")) ||
        package.fileName().endsWith(QStringLiteral(".sig"))) {
        emit failedToStart(QStringLiteral("Not a valid absolute Arch package path: %1").arg(package.filePath()));
        return;
    }
    result_ = {};
    result_.startedAt = QDateTime::currentDateTimeUtc();
    process_.setInputChannelMode(nonInteractive ? QProcess::ManagedInputChannel
                                                : QProcess::ForwardedInputChannel);
    process_.setProgram(QStringLiteral("/usr/bin/pkexec"));
    process_.setArguments(installArguments(package.absoluteFilePath(), nonInteractive));
    process_.start();
    if (nonInteractive) process_.closeWriteChannel();
    emit progressChanged(nonInteractive
        ? QStringLiteral("Waiting for polkit authorization and non-interactive pacman installation…")
        : QStringLiteral("Waiting for pacman installation…"));
}

void InstallService::startUninstall(const QString &packageName,
                                    const bool nonInteractive) {
    if (isRunning()) {
        emit failedToStart(QStringLiteral("A package operation is already running"));
        return;
    }
    if (!validArchPackageName(packageName)) {
        emit failedToStart(QStringLiteral("Invalid Arch package name: %1").arg(packageName));
        return;
    }
    result_ = {};
    result_.startedAt = QDateTime::currentDateTimeUtc();
    process_.setInputChannelMode(nonInteractive ? QProcess::ManagedInputChannel
                                                : QProcess::ForwardedInputChannel);
    process_.setProgram(QStringLiteral("/usr/bin/pkexec"));
    process_.setArguments(uninstallArguments(packageName, nonInteractive));
    process_.start();
    if (nonInteractive) process_.closeWriteChannel();
    emit progressChanged(nonInteractive
        ? QStringLiteral("Waiting for polkit authorization and non-interactive pacman removal…")
        : QStringLiteral("Waiting for pacman removal…"));
}

} // namespace pacsmith
