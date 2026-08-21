#include "core/apt_sources.hpp"

#include "core/control_parser.hpp"

#include <QRegularExpression>
#include <QSet>
#include <QUrl>

namespace pacsmith {
namespace {

QStringList words(const QString &value) {
    return value.split(QRegularExpression(QStringLiteral(R"(\s+)")), Qt::SkipEmptyParts);
}

bool usableRepositoryUri(const QString &value) {
    const QUrl url(value);
    return url.isValid() && !url.host().isEmpty() &&
           (url.scheme() == QStringLiteral("https") || url.scheme() == QStringLiteral("http"));
}

void appendUnique(QList<AptRepositoryCandidate> &result, QSet<QString> &seen,
                  AptRepositoryCandidate candidate) {
    if (!usableRepositoryUri(candidate.uri) || candidate.suite.isEmpty()) return;
    while (candidate.uri.endsWith(QLatin1Char('/'))) candidate.uri.chop(1);
    const auto key = candidate.uri + QChar::Null + candidate.suite + QChar::Null +
                     candidate.components.join(QLatin1Char(' ')) + QChar::Null +
                     candidate.architectures.join(QLatin1Char(','));
    if (!seen.contains(key)) {
        seen.insert(key);
        result.append(std::move(candidate));
    }
}

QList<AptRepositoryCandidate> parseDeb822(const QByteArrayView data, const QString &sourcePath) {
    QList<AptRepositoryCandidate> result;
    QSet<QString> seen;
    for (const auto &fields : ControlParser::parseParagraphs(data)) {
        if (!words(fields.value(QStringLiteral("Types"))).contains(QStringLiteral("deb"))) continue;
        const auto uris = words(fields.value(QStringLiteral("URIs")));
        const auto suites = words(fields.value(QStringLiteral("Suites")));
        const auto components = words(fields.value(QStringLiteral("Components")));
        auto architectures = words(fields.value(QStringLiteral("Architectures")));
        for (const auto &removed : words(fields.value(QStringLiteral("Architectures-Remove")))) {
            architectures.removeAll(removed);
        }
        for (const auto &uri : uris) {
            for (const auto &suite : suites) {
                appendUnique(result, seen,
                             {uri, suite, components, architectures,
                              fields.value(QStringLiteral("Signed-By")), sourcePath});
            }
        }
    }
    return result;
}

QList<AptRepositoryCandidate> parseOneLine(const QString &text, const QString &sourcePath) {
    QList<AptRepositoryCandidate> result;
    QSet<QString> seen;
    for (QString line : text.split(QLatin1Char('\n'))) {
        if (line.endsWith(QLatin1Char('\r'))) line.chop(1);
        const auto comment = line.indexOf(QLatin1Char('#'));
        if (comment >= 0) line.truncate(comment);
        line = line.trimmed();
        if (!line.startsWith(QStringLiteral("deb ")) && !line.startsWith(QStringLiteral("deb\t"))) continue;
        line = line.mid(3).trimmed();

        QString options;
        if (line.startsWith(QLatin1Char('['))) {
            const auto closing = line.indexOf(QLatin1Char(']'));
            if (closing < 0) continue;
            options = line.mid(1, closing - 1);
            line = line.mid(closing + 1).trimmed();
        }
        const auto fields = words(line);
        if (fields.size() < 2) continue;

        QStringList architectures;
        QString signedBy;
        for (const auto &option : words(options)) {
            if (option.startsWith(QStringLiteral("arch="))) {
                architectures = option.mid(5).split(QLatin1Char(','), Qt::SkipEmptyParts);
            } else if (option.startsWith(QStringLiteral("signed-by="))) {
                signedBy = option.mid(10);
            }
        }
        appendUnique(result, seen,
                     {fields.at(0), fields.at(1), fields.mid(2), architectures,
                      signedBy, sourcePath});
    }
    return result;
}

} // namespace

QList<AptRepositoryCandidate> AptSourcesParser::parse(const QByteArrayView data,
                                                       const QString &sourcePath) {
    const auto text = QString::fromUtf8(data.data(), data.size());
    const bool looksDeb822 = sourcePath.endsWith(QStringLiteral(".sources")) ||
                             QRegularExpression(QStringLiteral(R"((?:^|\n)Types\s*:)"),
                                                QRegularExpression::CaseInsensitiveOption)
                                 .match(text).hasMatch();
    return looksDeb822 ? parseDeb822(data, sourcePath) : parseOneLine(text, sourcePath);
}

} // namespace pacsmith
