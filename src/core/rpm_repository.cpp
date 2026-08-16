#include "core/rpm_repository.hpp"

#include "core/path_safety.hpp"

#include <QRegularExpression>
#include <QUrl>
#include <QXmlStreamReader>

#include <cctype>

namespace pacsmith {
namespace {

bool safeRepositoryFile(const QString &path) {
    const QUrl url(path);
    const auto normalized = PathSafety::normalizedArchivePath(path);
    return normalized && !normalized->isEmpty() && *normalized == path && url.isRelative() &&
           !path.contains(QLatin1Char(':')) && !path.contains(QLatin1Char('\\')) &&
           !path.startsWith(QStringLiteral("//"));
}

struct Evr {
    quint64 epoch{0};
    QByteArray version;
    QByteArray release;
};

Evr splitEvr(QString value) {
    Evr result;
    const auto colon = value.indexOf(QLatin1Char(':'));
    if (colon >= 0) {
        bool valid = false;
        result.epoch = value.left(colon).toULongLong(&valid);
        if (!valid) result.epoch = 0;
        value = value.mid(colon + 1);
    }
    const auto dash = value.lastIndexOf(QLatin1Char('-'));
    if (dash >= 0) {
        result.version = value.left(dash).toUtf8();
        result.release = value.mid(dash + 1).toUtf8();
    } else {
        result.version = value.toUtf8();
    }
    return result;
}

int compareSegmented(const QByteArray &left, const QByteArray &right) {
    qsizetype li = 0;
    qsizetype ri = 0;
    while (li < left.size() || ri < right.size()) {
        while (li < left.size() && !std::isalnum(static_cast<unsigned char>(left.at(li))) &&
               left.at(li) != '~' && left.at(li) != '^') ++li;
        while (ri < right.size() && !std::isalnum(static_cast<unsigned char>(right.at(ri))) &&
               right.at(ri) != '~' && right.at(ri) != '^') ++ri;

        const bool leftTilde = li < left.size() && left.at(li) == '~';
        const bool rightTilde = ri < right.size() && right.at(ri) == '~';
        if (leftTilde || rightTilde) {
            if (leftTilde != rightTilde) return leftTilde ? -1 : 1;
            ++li;
            ++ri;
            continue;
        }
        const bool leftCaret = li < left.size() && left.at(li) == '^';
        const bool rightCaret = ri < right.size() && right.at(ri) == '^';
        if (leftCaret || rightCaret) {
            if (leftCaret != rightCaret) {
                if (leftCaret && ri >= right.size()) return 1;
                if (rightCaret && li >= left.size()) return -1;
                return leftCaret ? -1 : 1;
            }
            ++li;
            ++ri;
            continue;
        }
        if (li >= left.size() || ri >= right.size()) {
            if (li >= left.size() && ri >= right.size()) return 0;
            return li >= left.size() ? -1 : 1;
        }

        const bool leftNumeric = std::isdigit(static_cast<unsigned char>(left.at(li))) != 0;
        const bool rightNumeric = std::isdigit(static_cast<unsigned char>(right.at(ri))) != 0;
        if (leftNumeric != rightNumeric) return leftNumeric ? 1 : -1;
        auto le = li;
        auto re = ri;
        if (leftNumeric) {
            while (li < left.size() && left.at(li) == '0') ++li;
            while (ri < right.size() && right.at(ri) == '0') ++ri;
            le = li;
            re = ri;
            while (le < left.size() && std::isdigit(static_cast<unsigned char>(left.at(le))) != 0) ++le;
            while (re < right.size() && std::isdigit(static_cast<unsigned char>(right.at(re))) != 0) ++re;
            const auto leftLength = le - li;
            const auto rightLength = re - ri;
            if (leftLength != rightLength) return leftLength < rightLength ? -1 : 1;
        } else {
            while (le < left.size() && std::isalpha(static_cast<unsigned char>(left.at(le))) != 0) ++le;
            while (re < right.size() && std::isalpha(static_cast<unsigned char>(right.at(re))) != 0) ++re;
        }
        const auto comparison = QByteArrayView(left).sliced(li, le - li)
                                    .compare(QByteArrayView(right).sliced(ri, re - ri));
        if (comparison != 0) return comparison < 0 ? -1 : 1;
        li = le;
        ri = re;
    }
    return 0;
}

bool validChecksum(const QString &type, const QString &value) {
    const auto normalized = type.toLower();
    const auto length = normalized == QStringLiteral("sha256") ? 64
                      : normalized == QStringLiteral("sha512") ? 128 : 0;
    if (length == 0 || value.size() != length) return false;
    static const QRegularExpression hexadecimal(QStringLiteral("^[0-9a-fA-F]+$"));
    return hexadecimal.match(value).hasMatch();
}

} // namespace

QString RpmPackageRecord::evr() const {
    auto value = version;
    if (!release.isEmpty()) value += QLatin1Char('-') + release;
    if (!epoch.isEmpty() && epoch != QStringLiteral("0")) value.prepend(epoch + QLatin1Char(':'));
    return value;
}

int RpmVersion::compare(const QString &left, const QString &right) {
    const auto lhs = splitEvr(left);
    const auto rhs = splitEvr(right);
    if (lhs.epoch != rhs.epoch) return lhs.epoch < rhs.epoch ? -1 : 1;
    const auto version = compareSegmented(lhs.version, rhs.version);
    return version != 0 ? version : compareSegmented(lhs.release, rhs.release);
}

std::optional<RpmRepomdPrimary> RpmRepositoryMetadata::parseRepomd(
    const QByteArrayView data, QString *error) {
    QXmlStreamReader xml(data);
    bool inPrimary = false;
    RpmRepomdPrimary result;
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isStartElement() && xml.name() == QStringLiteral("data")) {
            inPrimary = xml.attributes().value(QStringLiteral("type")) == QStringLiteral("primary");
        } else if (inPrimary && xml.isStartElement() && xml.name() == QStringLiteral("checksum")) {
            result.checksumType = xml.attributes().value(QStringLiteral("type")).toString().toLower();
            result.checksum = xml.readElementText().trimmed().toLower();
        } else if (inPrimary && xml.isStartElement() && xml.name() == QStringLiteral("location")) {
            result.path = xml.attributes().value(QStringLiteral("href")).toString();
        } else if (xml.isEndElement() && xml.name() == QStringLiteral("data")) {
            if (inPrimary) break;
            inPrimary = false;
        }
    }
    if (xml.hasError()) {
        if (error != nullptr) *error = QStringLiteral("Invalid repomd.xml: %1").arg(xml.errorString());
        return std::nullopt;
    }
    if (!safeRepositoryFile(result.path) || !validChecksum(result.checksumType, result.checksum)) {
        if (error != nullptr) {
            *error = QStringLiteral("repomd.xml has no safe primary metadata entry with a SHA256 or SHA512 checksum");
        }
        return std::nullopt;
    }
    return result;
}

std::optional<RpmPackageRecord> RpmRepositoryMetadata::latestPackage(
    const QByteArrayView primaryXml, const QString &packageName,
    const QString &architecture, QString *error) {
    QXmlStreamReader xml(primaryXml);
    std::optional<RpmPackageRecord> best;
    while (!xml.atEnd()) {
        xml.readNext();
        if (!xml.isStartElement() || xml.name() != QStringLiteral("package")) continue;
        RpmPackageRecord candidate;
        while (!(xml.isEndElement() && xml.name() == QStringLiteral("package")) && !xml.atEnd()) {
            xml.readNext();
            if (!xml.isStartElement()) continue;
            if (xml.name() == QStringLiteral("name")) candidate.name = xml.readElementText().trimmed();
            else if (xml.name() == QStringLiteral("arch")) candidate.architecture = xml.readElementText().trimmed();
            else if (xml.name() == QStringLiteral("version")) {
                candidate.epoch = xml.attributes().value(QStringLiteral("epoch")).toString();
                candidate.version = xml.attributes().value(QStringLiteral("ver")).toString();
                candidate.release = xml.attributes().value(QStringLiteral("rel")).toString();
            } else if (xml.name() == QStringLiteral("checksum")) {
                candidate.checksumType = xml.attributes().value(QStringLiteral("type")).toString().toLower();
                candidate.checksum = xml.readElementText().trimmed().toLower();
            } else if (xml.name() == QStringLiteral("location")) {
                candidate.filename = xml.attributes().value(QStringLiteral("href")).toString();
            }
        }
        if (candidate.name != packageName ||
            (candidate.architecture != architecture && candidate.architecture != QStringLiteral("noarch")) ||
            candidate.version.isEmpty() || !safeRepositoryFile(candidate.filename) ||
            candidate.checksumType != QStringLiteral("sha256") ||
            !validChecksum(candidate.checksumType, candidate.checksum)) {
            continue;
        }
        if (!best || RpmVersion::compare(candidate.evr(), best->evr()) > 0) best = candidate;
    }
    if (xml.hasError()) {
        if (error != nullptr) *error = QStringLiteral("Invalid primary RPM metadata: %1").arg(xml.errorString());
        return std::nullopt;
    }
    if (!best && error != nullptr) {
        *error = QStringLiteral("Package %1 for architecture %2 was not found in primary RPM metadata")
                     .arg(packageName, architecture);
    }
    return best;
}

} // namespace pacsmith
