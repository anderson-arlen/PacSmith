#include "core/lifecycle_validator.hpp"

#include <QProcess>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <QTemporaryFile>

namespace pacsmith {

QString LifecycleValidation::message() const {
    return passed ? QStringLiteral("Syntax and PacSmith lifecycle policy validation passed.")
                  : problems.join(QLatin1Char('\n'));
}

LifecycleValidation LifecycleValidator::validate(const QString &contents) {
    LifecycleValidation result;
    if (contents.trimmed().isEmpty()) {
        result.problems.append(QStringLiteral("Lifecycle script is empty"));
        return result;
    }
    if (contents.toUtf8().size() > 128 * 1024) {
        result.problems.append(QStringLiteral("Lifecycle script exceeds 128 KiB"));
    }
    static const QList<QPair<QRegularExpression, QString>> forbidden{
        {QRegularExpression(QStringLiteral(R"((?:^|[^A-Za-z0-9_])(curl|wget|aria2c|ftp)(?:[^A-Za-z0-9_]|$))"),
                            QRegularExpression::CaseInsensitiveOption),
         QStringLiteral("Network download commands are not allowed")},
        {QRegularExpression(QStringLiteral(R"((?:^|[^A-Za-z0-9_])(apt|apt-get|dpkg|pacman|makepkg)(?:[^A-Za-z0-9_]|$))"),
                            QRegularExpression::CaseInsensitiveOption),
         QStringLiteral("Package-manager recursion is not allowed")},
        {QRegularExpression(QStringLiteral(R"((?:^|[^A-Za-z0-9_])(sudo|pkexec|su)(?:[^A-Za-z0-9_]|$))"),
                            QRegularExpression::CaseInsensitiveOption),
         QStringLiteral("Privilege-elevation commands are not allowed")},
        {QRegularExpression(QStringLiteral(R"((?:^|[;&|[:space:]])(eval|source|\.)[[:space:]])")),
         QStringLiteral("Dynamic shell evaluation is not allowed")},
        {QRegularExpression(QStringLiteral(R"(https?://)"), QRegularExpression::CaseInsensitiveOption),
         QStringLiteral("Network URLs are not allowed in lifecycle scripts")},
        {QRegularExpression(QStringLiteral("`|\\$\\(")),
         QStringLiteral("Command substitution is not allowed in lifecycle scripts")}};
    for (const auto &[expression, message] : forbidden) {
        if (expression.match(contents).hasMatch()) result.problems.append(message);
    }

    static const QRegularExpression functionExpression(
        QStringLiteral(R"((?m)^\s*([A-Za-z_][A-Za-z0-9_]*)\s*\(\s*\)\s*\{)"));
    const QSet<QString> allowed{QStringLiteral("pre_install"), QStringLiteral("post_install"),
                                QStringLiteral("pre_upgrade"), QStringLiteral("post_upgrade"),
                                QStringLiteral("pre_remove"), QStringLiteral("post_remove")};
    bool foundFunction = false;
    auto functions = functionExpression.globalMatch(contents);
    while (functions.hasNext()) {
        foundFunction = true;
        const auto name = functions.next().captured(1);
        if (!allowed.contains(name)) {
            result.problems.append(QStringLiteral("Unsupported lifecycle function: %1").arg(name));
        }
    }
    if (!foundFunction) result.problems.append(QStringLiteral("No Arch lifecycle function was found"));

    const auto bash = QStandardPaths::findExecutable(QStringLiteral("bash"));
    if (bash.isEmpty()) {
        result.problems.append(QStringLiteral("bash is required for syntax-only validation"));
    } else {
        QTemporaryFile file;
        const auto encoded = contents.toUtf8();
        if (!file.open() || file.write(encoded) != encoded.size() || !file.flush()) {
            result.problems.append(QStringLiteral("Could not prepare lifecycle script for syntax validation"));
        } else {
            QProcess process;
            process.setProgram(bash);
            process.setArguments({QStringLiteral("-n"), file.fileName()});
            process.start();
            if (!process.waitForStarted(5000) || !process.waitForFinished(10000) || process.exitCode() != 0) {
                const auto error = QString::fromUtf8(process.readAllStandardError()).trimmed();
                result.problems.append(error.isEmpty() ? QStringLiteral("bash syntax validation failed") : error);
            }
        }
    }
    result.problems.removeDuplicates();
    result.passed = result.problems.isEmpty();
    return result;
}

} // namespace pacsmith
