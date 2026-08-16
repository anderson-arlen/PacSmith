#include "core/dependency_parser.hpp"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

namespace pacsmith {
namespace {

QStringList splitTopLevel(const QString &input, const QChar separator) {
    QStringList result;
    qsizetype start = 0;
    int parentheses = 0;
    int brackets = 0;
    for (qsizetype index = 0; index < input.size(); ++index) {
        const auto character = input.at(index);
        if (character == QLatin1Char('(')) ++parentheses;
        else if (character == QLatin1Char(')') && parentheses > 0) --parentheses;
        else if (character == QLatin1Char('[')) ++brackets;
        else if (character == QLatin1Char(']') && brackets > 0) --brackets;
        else if (character == separator && parentheses == 0 && brackets == 0) {
            result.append(input.mid(start, index - start).trimmed());
            start = index + 1;
        }
    }
    result.append(input.mid(start).trimmed());
    result.removeAll(QString{});
    return result;
}

DependencyAlternative parseAlternative(QString input) {
    // Architecture restrictions and build profiles do not change the package-name mapping.
    input.remove(QRegularExpression(QStringLiteral(R"(\s*\[[^\]]*\]\s*)")));
    input.remove(QRegularExpression(QStringLiteral(R"(\s*<[^>]*>\s*)")));
    static const QRegularExpression expression(QStringLiteral(
        R"(^\s*([a-zA-Z0-9][a-zA-Z0-9+.-]*)(?::[a-zA-Z0-9-]+)?\s*(?:\((<<|<=|=|>=|>>)\s*([^\)]+)\))?\s*$)"));
    const auto match = expression.match(input);
    if (!match.hasMatch()) {
        return {input.trimmed(), {}, {}};
    }
    return {match.captured(1), match.captured(2), match.captured(3).trimmed()};
}

} // namespace

QList<DependencyMapping> DependencyParser::parse(const QString &declarations) {
    QList<DependencyMapping> result;
    for (const auto &group : splitTopLevel(declarations, QLatin1Char(','))) {
        DependencyMapping mapping;
        mapping.rawExpression = group;
        for (const auto &alternative : splitTopLevel(group, QLatin1Char('|'))) {
            mapping.alternatives.append(parseAlternative(alternative));
        }
        result.append(mapping);
    }
    return result;
}

bool DependencyParser::applyVerifiedMappings(QList<DependencyMapping> &dependencies,
                                             const QMap<QString, QString> &mappings) {
    bool changed = false;
    for (auto &dependency : dependencies) {
        const bool meaningfulUserOverride = dependency.userOverride &&
                                            (!dependency.archPackage.isEmpty() ||
                                             dependency.status != MappingStatus::Unresolved);
        if (meaningfulUserOverride || dependency.alternatives.isEmpty()) {
            continue;
        }
        for (const auto &alternative : dependency.alternatives) {
            const auto mapped = mappings.constFind(alternative.packageName);
            if (mapped != mappings.cend()) {
                dependency.archPackage = mapped.value();
                dependency.status = MappingStatus::Resolved;
                dependency.mappingSource = QStringLiteral("verified built-in mapping");
                dependency.confidence = 1.0;
                dependency.userOverride = false;
                dependency.ignored = false;
                dependency.bundled = false;
                dependency.provided = false;
                changed = true;
                break;
            }
        }
    }
    return changed;
}

QMap<QString, QString> DependencyParser::loadVerifiedMappings() {
    QFile file(QStringLiteral(":/pacsmith/dependency-mappings.json"));
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    const auto document = QJsonDocument::fromJson(file.readAll());
    const auto object = document.object();
    QMap<QString, QString> result;
    for (auto iterator = object.constBegin(); iterator != object.constEnd(); ++iterator) {
        result.insert(iterator.key(), iterator.value().toString());
    }
    return result;
}

} // namespace pacsmith
