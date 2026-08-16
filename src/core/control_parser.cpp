#include "core/control_parser.hpp"

#include <QStringList>

namespace pacsmith {

QList<QMap<QString, QString>> ControlParser::parseParagraphs(const QByteArrayView data) {
    const auto text = QString::fromUtf8(data.data(), data.size());
    const auto lines = text.split(QLatin1Char('\n'), Qt::KeepEmptyParts);
    QList<QMap<QString, QString>> paragraphs;
    QMap<QString, QString> fields;
    QString currentField;

    auto finishParagraph = [&]() {
        if (!fields.isEmpty()) {
            paragraphs.append(fields);
            fields.clear();
            currentField.clear();
        }
    };

    for (QString line : lines) {
        if (line.endsWith(QLatin1Char('\r'))) {
            line.chop(1);
        }
        if (line.isEmpty()) {
            finishParagraph();
            continue;
        }
        if ((line.startsWith(QLatin1Char(' ')) || line.startsWith(QLatin1Char('\t'))) &&
            !currentField.isEmpty()) {
            QString continuation = line.mid(1);
            if (continuation == QStringLiteral(".")) {
                continuation.clear();
            }
            fields[currentField].append(QLatin1Char('\n'));
            fields[currentField].append(continuation);
            continue;
        }

        const auto separator = line.indexOf(QLatin1Char(':'));
        if (separator <= 0) {
            currentField.clear();
            continue;
        }
        currentField = line.left(separator).trimmed();
        fields[currentField] = line.mid(separator + 1).trimmed();
    }
    finishParagraph();
    return paragraphs;
}

DebianMetadata ControlParser::parsePackage(const QByteArrayView data) {
    DebianMetadata result;
    const auto paragraphs = parseParagraphs(data);
    if (paragraphs.isEmpty()) {
        return result;
    }
    result.rawFields = paragraphs.first();
    const auto field = [&result](const QString &name) { return result.rawFields.value(name); };
    result.package = field(QStringLiteral("Package"));
    result.version = field(QStringLiteral("Version"));
    result.architecture = field(QStringLiteral("Architecture"));
    result.maintainer = field(QStringLiteral("Maintainer"));
    result.description = field(QStringLiteral("Description"));
    result.homepage = field(QStringLiteral("Homepage"));
    result.depends = field(QStringLiteral("Depends"));
    result.preDepends = field(QStringLiteral("Pre-Depends"));
    result.recommends = field(QStringLiteral("Recommends"));
    result.suggests = field(QStringLiteral("Suggests"));
    result.conflicts = field(QStringLiteral("Conflicts"));
    result.provides = field(QStringLiteral("Provides"));
    return result;
}

} // namespace pacsmith
