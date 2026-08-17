#include "core/pkgbuild_install_plan.hpp"

#include <QHash>
#include <QMap>
#include <QRegularExpression>

#include <algorithm>

namespace pacsmith {
namespace {

QString unquote(QString value) {
    value = value.trimmed();
    if (value.size() >= 2 &&
        ((value.front() == QLatin1Char('\'') && value.back() == QLatin1Char('\'')) ||
         (value.front() == QLatin1Char('"') && value.back() == QLatin1Char('"')))) {
        return value.mid(1, value.size() - 2);
    }
    return value;
}

QString expandVars(QString text, const QHash<QString, QString> &vars) {
    static const QRegularExpression braced(QStringLiteral(R"(\$\{([A-Za-z_][A-Za-z0-9_]*)\})"));
    static const QRegularExpression bare(QStringLiteral(R"(\$([A-Za-z_][A-Za-z0-9_]*))"));
    for (int pass = 0; pass < 4; ++pass) {
        bool changed = false;
        auto replace = [&](const QRegularExpression &pattern) {
            QRegularExpressionMatchIterator iterator = pattern.globalMatch(text);
            QString result;
            qsizetype last = 0;
            while (iterator.hasNext()) {
                const auto match = iterator.next();
                result += text.mid(last, match.capturedStart() - last);
                result += vars.value(match.captured(1));
                last = match.capturedEnd();
                changed = true;
            }
            result += text.mid(last);
            if (changed) text = result;
        };
        replace(braced);
        replace(bare);
        if (!changed) break;
    }
    return text;
}

QString normalizeInstalledPath(QString path) {
    path = unquote(path.trimmed());
    path.replace(QLatin1Char('\\'), QLatin1Char('/'));
    while (path.contains(QStringLiteral("//"))) path.replace(QStringLiteral("//"), QStringLiteral("/"));
    if (path.isEmpty() || path == QStringLiteral(".")) return {};
    if (!path.startsWith(QLatin1Char('/'))) path.prepend(QLatin1Char('/'));
    while (path.size() > 1 && path.endsWith(QLatin1Char('/'))) path.chop(1);
    return path;
}

QStringList tokenize(const QString &line) {
    QStringList tokens;
    QString current;
    QChar quote;
    for (const auto character : line) {
        if (quote.isNull()) {
            if (character == QLatin1Char('#') && current.isEmpty()) break;
            if (character == QLatin1Char('\'') || character == QLatin1Char('"')) {
                quote = character;
                current += character;
            } else if (character.isSpace()) {
                if (!current.isEmpty()) {
                    tokens.append(current);
                    current.clear();
                }
            } else {
                current += character;
            }
        } else {
            current += character;
            if (character == quote) quote = {};
        }
    }
    if (!current.isEmpty()) tokens.append(current);
    return tokens;
}

QString extractFunctionBody(const QString &pkgbuild, const QString &name) {
    const auto needle = name + QStringLiteral("() {");
    const auto start = pkgbuild.indexOf(needle);
    if (start < 0) return {};
    qsizetype index = start + needle.size();
    int depth = 1;
    QChar quote;
    for (; index < pkgbuild.size(); ++index) {
        const auto character = pkgbuild.at(index);
        if (!quote.isNull()) {
            if (character == quote) quote = {};
            continue;
        }
        if (character == QLatin1Char('\'') || character == QLatin1Char('"')) {
            quote = character;
        } else if (character == QLatin1Char('{')) {
            ++depth;
        } else if (character == QLatin1Char('}')) {
            --depth;
            if (depth == 0) return pkgbuild.mid(start + needle.size(), index - (start + needle.size()));
        }
    }
    return {};
}

QString parseArrayFirst(const QString &value) {
    auto inner = value.trimmed();
    if (inner.startsWith(QLatin1Char('(')) && inner.endsWith(QLatin1Char(')'))) {
        inner = inner.mid(1, inner.size() - 2);
    }
    const auto tokens = tokenize(inner);
    return tokens.isEmpty() ? QString{} : unquote(tokens.first());
}

QHash<QString, QString> parseAssignments(const QString &pkgbuild, QString *installFile, QString *sourceName) {
    QHash<QString, QString> vars;
    vars.insert(QStringLiteral("pkgdir"), {});
    vars.insert(QStringLiteral("srcdir"), {});
    const auto lines = QStringView{pkgbuild}.split(QLatin1Char('\n'));
    static const QRegularExpression assignment(
        QStringLiteral(R"(^\s*(?:local\s+)?([A-Za-z_][A-Za-z0-9_]*)=(.*)$)"));
    for (const auto &lineView : lines) {
        const auto line = lineView.toString();
        if (line.contains(QStringLiteral("() {")) || line.contains(QStringLiteral("(){"))) break;
        const auto match = assignment.match(line);
        if (!match.hasMatch()) continue;
        const auto name = match.captured(1);
        const auto raw = match.captured(2).trimmed();
        if (name == QStringLiteral("source")) {
            if (sourceName != nullptr) *sourceName = parseArrayFirst(expandVars(raw, vars));
            continue;
        }
        const auto value = unquote(expandVars(raw, vars));
        vars.insert(name, value);
        if (name == QStringLiteral("install") && installFile != nullptr) *installFile = value;
    }
    return vars;
}

bool isFlag(const QString &token) {
    return token.startsWith(QLatin1Char('-'));
}

QStringList positionalArgs(const QStringList &tokens, const int start) {
    QStringList args;
    for (int index = start; index < tokens.size(); ++index) {
        const auto &token = tokens.at(index);
        if (token == QLatin1Char('-') || token == QStringLiteral("--")) continue;
        if (isFlag(token)) continue;
        args.append(token);
    }
    return args;
}

QString optionValue(const QStringList &tokens, const QString &flag, const QString &longEquals) {
    for (int index = 0; index < tokens.size(); ++index) {
        const auto &token = tokens.at(index);
        if (token == flag && index + 1 < tokens.size()) return tokens.at(index + 1);
        if (!longEquals.isEmpty() && token.startsWith(longEquals)) return token.mid(longEquals.size());
    }
    return {};
}

int stripComponents(const QStringList &tokens) {
    const auto value = optionValue(tokens, QStringLiteral("--strip-components"),
                                   QStringLiteral("--strip-components="));
    bool ok = false;
    const auto count = value.toInt(&ok);
    return ok ? std::max(0, count) : 0;
}

void upsert(QMap<QString, InstallPlanEntry> *entries, const QString &path, const QString &source,
            const QString &purpose, const bool excluded = false) {
    const auto normalized = normalizeInstalledPath(path);
    if (normalized.isEmpty() || normalized == QStringLiteral("/")) return;
    InstallPlanEntry entry;
    entry.path = normalized;
    entry.source = source;
    entry.purpose = purpose;
    entry.excluded = excluded;
    entries->insert(normalized, entry);
}

void addExtractedPayload(QMap<QString, InstallPlanEntry> *entries, const PackageRelease &release,
                         QString destPrefix, const int strip, const QString &source) {
    destPrefix = normalizeInstalledPath(destPrefix);
    if (destPrefix.isEmpty()) destPrefix = QStringLiteral("/");
    for (const auto &payload : release.payload) {
        if (payload.type == QStringLiteral("directory")) continue;
        auto relative = payload.path;
        if (relative.startsWith(QLatin1Char('/'))) relative.remove(0, 1);
        if (strip > 0) {
            const auto parts = relative.split(QLatin1Char('/'), Qt::SkipEmptyParts);
            if (parts.size() <= strip) continue;
            relative = QStringList(parts.mid(strip)).join(QLatin1Char('/'));
        }
        if (relative.isEmpty()) continue;
        const auto installed = destPrefix == QStringLiteral("/")
            ? QLatin1Char('/') + relative
            : destPrefix + QLatin1Char('/') + relative;
        QString purpose = QStringLiteral("Vendor payload");
        if (!payload.symlinkTarget.isEmpty()) {
            purpose = QStringLiteral("Symlink → %1").arg(payload.symlinkTarget);
        }
        upsert(entries, installed, source, purpose);
    }
}

void removePrefix(QMap<QString, InstallPlanEntry> *entries, const QString &path) {
    const auto normalized = normalizeInstalledPath(path);
    if (normalized.isEmpty()) return;
    const auto prefix = normalized + QLatin1Char('/');
    QList<QString> keys;
    for (auto iterator = entries->cbegin(); iterator != entries->cend(); ++iterator) {
        if (iterator.key() == normalized || iterator.key().startsWith(prefix)) keys.append(iterator.key());
    }
    for (const auto &key : keys) {
        auto entry = entries->take(key);
        entry.excluded = true;
        if (!entry.purpose.contains(QStringLiteral("Removed"), Qt::CaseInsensitive)) {
            entry.purpose = QStringLiteral("Removed in package()");
        }
        entry.source = QStringLiteral("package() rm");
        entries->insert(key, entry);
    }
}

bool looksLikeOpaqueInstall(const QStringList &tokens) {
    if (tokens.isEmpty()) return false;
    const auto command = tokens.first();
    const auto joined = tokens.join(QLatin1Char(' '));
    if (command == QStringLiteral("make") || command == QStringLiteral("gmake") ||
        command == QStringLiteral("ninja") || command == QStringLiteral("cmake") ||
        command == QStringLiteral("meson")) {
        return joined.contains(QStringLiteral("DESTDIR")) ||
               joined.contains(QStringLiteral("install"));
    }
    return false;
}

} // namespace

InstallPlan PkgbuildInstallPlan::parse(const QString &pkgbuild, const PackageRelease &release) {
    InstallPlan plan;
    QString installFile;
    QString sourceName = release.originalSourceFilename;
    auto vars = parseAssignments(pkgbuild, &installFile, &sourceName);
    if (vars.value(QStringLiteral("pkgname")).isEmpty()) {
        vars.insert(QStringLiteral("pkgname"), release.archPackageName);
    }
    if (sourceName.isEmpty()) sourceName = release.originalSourceFilename;

    const auto packageBody = extractFunctionBody(pkgbuild, QStringLiteral("package"));
    if (packageBody.isEmpty()) {
        plan.warnings.append(QStringLiteral("No package() function was found in the PKGBUILD."));
        return plan;
    }

    QMap<QString, InstallPlanEntry> entries;
    bool extracted = false;
    bool opaque = false;
    const auto lines = QStringView{packageBody}.split(QLatin1Char('\n'));
    static const QRegularExpression assignment(
        QStringLiteral(R"(^\s*(?:local\s+)?([A-Za-z_][A-Za-z0-9_]*)=(.*)$)"));
    for (const auto &lineView : lines) {
        auto line = lineView.toString().trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#'))) continue;
        const auto assign = assignment.match(line);
        if (assign.hasMatch() && !line.contains(QStringLiteral("()"))) {
            vars.insert(assign.captured(1), unquote(expandVars(assign.captured(2), vars)));
            continue;
        }
        auto tokens = tokenize(line);
        for (auto &token : tokens) token = unquote(expandVars(token, vars));
        if (tokens.isEmpty()) continue;
        const auto command = tokens.first();
        if (looksLikeOpaqueInstall(tokens)) {
            opaque = true;
            continue;
        }
        if (command == QStringLiteral("bsdtar") || command == QStringLiteral("tar") ||
            command.contains(QStringLiteral("bsdtar"))) {
            auto dest = optionValue(tokens, QStringLiteral("-C"), {});
            if (dest.isEmpty() && line.contains(QStringLiteral("|")) &&
                line.contains(QStringLiteral("-C"))) {
                dest = optionValue(tokenize(expandVars(line, vars)), QStringLiteral("-C"), {});
            }
            if (dest.isEmpty()) dest = QStringLiteral("/");
            addExtractedPayload(&entries, release, dest, stripComponents(tokens), sourceName);
            extracted = true;
            continue;
        }
        if (command == QStringLiteral("unsquashfs")) {
            auto dest = optionValue(tokens, QStringLiteral("-d"), {});
            if (dest.isEmpty()) dest = QStringLiteral("/");
            addExtractedPayload(&entries, release, dest, 0, sourceName);
            extracted = true;
            continue;
        }
        if (command == QStringLiteral("rm")) {
            for (const auto &arg : positionalArgs(tokens, 1)) removePrefix(&entries, arg);
            continue;
        }
        if (command == QStringLiteral("ln")) {
            const auto args = positionalArgs(tokens, 1);
            if (args.size() >= 2) {
                upsert(&entries, args.last(), QStringLiteral("Symlink → %1").arg(args.at(args.size() - 2)),
                       QStringLiteral("Created in package()"));
            }
            continue;
        }
        if (command == QStringLiteral("install")) {
            bool directoryOnly = false;
            for (const auto &token : tokens) {
            if (token == QStringLiteral("-d") || token == QStringLiteral("--directory")) {
                directoryOnly = true;
                break;
            }
            }
            if (directoryOnly) continue;
            const auto args = positionalArgs(tokens, 1);
            if (args.size() >= 2) {
                upsert(&entries, args.last(), args.at(args.size() - 2),
                       QStringLiteral("Installed by package()"));
            }
            continue;
        }
        if (command == QStringLiteral("cp") || command == QStringLiteral("mv")) {
            const auto args = positionalArgs(tokens, 1);
            if (args.size() >= 2) {
                upsert(&entries, args.last(), args.at(args.size() - 2),
                       QStringLiteral("Copied by package()"));
            }
            continue;
        }
        if (command == QStringLiteral("cat") || command == QStringLiteral("printf")) {
            const auto redirect = tokens.indexOf(QStringLiteral(">"));
            if (redirect >= 0 && redirect + 1 < tokens.size()) {
                upsert(&entries, tokens.at(redirect + 1), QStringLiteral("package()"),
                       command == QStringLiteral("cat")
                           ? QStringLiteral("Written by package()")
                           : QStringLiteral("Generated by package()"));
            }
            continue;
        }
    }

    static const QRegularExpression redirectDest(
        QStringLiteral(R"(>\s*\"\$\{?pkgdir\}?([^\"]+)\")"));
    auto redirectIterator = redirectDest.globalMatch(packageBody);
    while (redirectIterator.hasNext()) {
        const auto match = redirectIterator.next();
        upsert(&entries, match.captured(1), QStringLiteral("package()"),
               QStringLiteral("Written by package()"));
    }

    if (opaque) {
        plan.warnings.append(
            QStringLiteral("package() installs through a build-system command (make/cmake/meson/ninja); the exact file list is unknown."));
    }
    if (!extracted && !opaque && entries.isEmpty()) {
        plan.warnings.append(QStringLiteral("package() did not yield recognizable install paths."));
    }
    if (!installFile.isEmpty()) {
        upsert(&entries, installFile, QStringLiteral("PKGBUILD install="),
               QStringLiteral("Package .install script; pacman may run it as root"));
    }

    for (auto iterator = entries.cbegin(); iterator != entries.cend(); ++iterator) {
        plan.entries.append(iterator.value());
    }
    return plan;
}

} // namespace pacsmith
