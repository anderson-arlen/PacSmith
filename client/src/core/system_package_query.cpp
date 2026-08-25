#include "core/system_package_query.hpp"

#include <QProcess>
#include <QRegularExpression>

namespace pacsmith {

QStringList SystemPackageQuery::repositoryPackageNames(QString *error) {
    QProcess process;
    process.setProgram(QStringLiteral("/usr/bin/pacman"));
    process.setArguments({QStringLiteral("-Slq")});
    process.start();
    if (!process.waitForStarted(3000) || !process.waitForFinished(15000)) {
        if (error != nullptr) *error = QStringLiteral("The configured pacman repository catalog could not be read");
        return {};
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        if (error != nullptr) {
            *error = QString::fromUtf8(process.readAllStandardError()).trimmed();
            if (error->isEmpty()) *error = QStringLiteral("pacman -Slq exited with code %1").arg(process.exitCode());
        }
        return {};
    }
    auto packages = QString::fromUtf8(process.readAllStandardOutput()).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (auto &package : packages) package = package.trimmed();
    packages.removeAll(QString{});
    packages.removeDuplicates();
    packages.sort(Qt::CaseInsensitive);
    return packages;
}

bool SystemPackageQuery::repositoryPackageAvailable(const QString &packageName) {
    static const QRegularExpression safeName(QStringLiteral("^[a-z0-9@._+\\-]+$"));
    if (!safeName.match(packageName).hasMatch()) return false;
    QProcess process;
    process.setProgram(QStringLiteral("/usr/bin/pacman"));
    process.setArguments({QStringLiteral("-Sp"), QStringLiteral("--print-format"),
                          QStringLiteral("%n"), QStringLiteral("--noconfirm"),
                          QStringLiteral("--"), packageName});
    process.start();
    return process.waitForStarted(3000) && process.waitForFinished(10000) &&
           process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
}

} // namespace pacsmith
