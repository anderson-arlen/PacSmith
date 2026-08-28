#include "core/package_version.hpp"

#include <QByteArray>
#include <QByteArrayView>

#include <cctype>

namespace pacsmith {
namespace {

struct DebianVersionParts {
    quint64 epoch{0};
    QByteArray upstream;
    QByteArray revision{"0"};
};

DebianVersionParts splitDebianVersion(const QString &version) {
    DebianVersionParts result;
    QString remainder = version;
    const auto colon = remainder.indexOf(QLatin1Char(':'));
    if (colon >= 0) {
        bool valid = false;
        result.epoch = remainder.left(colon).toULongLong(&valid);
        if (!valid) result.epoch = 0;
        remainder = remainder.mid(colon + 1);
    }
    const auto dash = remainder.lastIndexOf(QLatin1Char('-'));
    if (dash >= 0) {
        result.upstream = remainder.left(dash).toUtf8();
        result.revision = remainder.mid(dash + 1).toUtf8();
    } else {
        result.upstream = remainder.toUtf8();
    }
    return result;
}

int debianCharacterOrder(const unsigned char value) {
    if (value == static_cast<unsigned char>('~')) return -1;
    if (value == 0U || std::isdigit(value) != 0) return 0;
    if (std::isalpha(value) != 0) return static_cast<int>(value);
    return static_cast<int>(value) + 256;
}

int compareDebianPart(const QByteArray &left, const QByteArray &right) {
    qsizetype leftIndex = 0;
    qsizetype rightIndex = 0;
    while (leftIndex < left.size() || rightIndex < right.size()) {
        while ((leftIndex < left.size() && std::isdigit(static_cast<unsigned char>(left.at(leftIndex))) == 0) ||
               (rightIndex < right.size() && std::isdigit(static_cast<unsigned char>(right.at(rightIndex))) == 0)) {
            const auto leftCharacter = leftIndex < left.size()
                                           ? static_cast<unsigned char>(left.at(leftIndex))
                                           : static_cast<unsigned char>(0);
            const auto rightCharacter = rightIndex < right.size()
                                            ? static_cast<unsigned char>(right.at(rightIndex))
                                            : static_cast<unsigned char>(0);
            const auto difference = debianCharacterOrder(leftCharacter) -
                                    debianCharacterOrder(rightCharacter);
            if (difference != 0) return difference < 0 ? -1 : 1;
            if (leftIndex < left.size()) ++leftIndex;
            if (rightIndex < right.size()) ++rightIndex;
        }

        while (leftIndex < left.size() && left.at(leftIndex) == '0') ++leftIndex;
        while (rightIndex < right.size() && right.at(rightIndex) == '0') ++rightIndex;
        const auto leftDigits = leftIndex;
        const auto rightDigits = rightIndex;
        while (leftIndex < left.size() &&
               std::isdigit(static_cast<unsigned char>(left.at(leftIndex))) != 0) ++leftIndex;
        while (rightIndex < right.size() &&
               std::isdigit(static_cast<unsigned char>(right.at(rightIndex))) != 0) ++rightIndex;
        const auto leftLength = leftIndex - leftDigits;
        const auto rightLength = rightIndex - rightDigits;
        if (leftLength != rightLength) return leftLength < rightLength ? -1 : 1;
        const auto comparison = QByteArrayView(left).sliced(leftDigits, leftLength)
                                    .compare(QByteArrayView(right).sliced(rightDigits, rightLength));
        if (comparison != 0) return comparison < 0 ? -1 : 1;
    }
    return 0;
}

struct RpmVersionParts {
    quint64 epoch{0};
    QByteArray version;
    QByteArray release;
};

RpmVersionParts splitRpmVersion(QString value) {
    RpmVersionParts result;
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

int compareRpmPart(const QByteArray &left, const QByteArray &right) {
    qsizetype leftIndex = 0;
    qsizetype rightIndex = 0;
    while (leftIndex < left.size() || rightIndex < right.size()) {
        while (leftIndex < left.size() &&
               std::isalnum(static_cast<unsigned char>(left.at(leftIndex))) == 0 &&
               left.at(leftIndex) != '~' && left.at(leftIndex) != '^') ++leftIndex;
        while (rightIndex < right.size() &&
               std::isalnum(static_cast<unsigned char>(right.at(rightIndex))) == 0 &&
               right.at(rightIndex) != '~' && right.at(rightIndex) != '^') ++rightIndex;

        const bool leftTilde = leftIndex < left.size() && left.at(leftIndex) == '~';
        const bool rightTilde = rightIndex < right.size() && right.at(rightIndex) == '~';
        if (leftTilde || rightTilde) {
            if (leftTilde != rightTilde) return leftTilde ? -1 : 1;
            ++leftIndex;
            ++rightIndex;
            continue;
        }
        const bool leftCaret = leftIndex < left.size() && left.at(leftIndex) == '^';
        const bool rightCaret = rightIndex < right.size() && right.at(rightIndex) == '^';
        if (leftCaret || rightCaret) {
            if (leftCaret != rightCaret) {
                if (leftCaret && rightIndex >= right.size()) return 1;
                if (rightCaret && leftIndex >= left.size()) return -1;
                return leftCaret ? -1 : 1;
            }
            ++leftIndex;
            ++rightIndex;
            continue;
        }
        if (leftIndex >= left.size() || rightIndex >= right.size()) {
            if (leftIndex >= left.size() && rightIndex >= right.size()) return 0;
            return leftIndex >= left.size() ? -1 : 1;
        }

        const bool leftNumeric =
            std::isdigit(static_cast<unsigned char>(left.at(leftIndex))) != 0;
        const bool rightNumeric =
            std::isdigit(static_cast<unsigned char>(right.at(rightIndex))) != 0;
        if (leftNumeric != rightNumeric) return leftNumeric ? 1 : -1;
        auto leftEnd = leftIndex;
        auto rightEnd = rightIndex;
        if (leftNumeric) {
            while (leftIndex < left.size() && left.at(leftIndex) == '0') ++leftIndex;
            while (rightIndex < right.size() && right.at(rightIndex) == '0') ++rightIndex;
            leftEnd = leftIndex;
            rightEnd = rightIndex;
            while (leftEnd < left.size() &&
                   std::isdigit(static_cast<unsigned char>(left.at(leftEnd))) != 0) ++leftEnd;
            while (rightEnd < right.size() &&
                   std::isdigit(static_cast<unsigned char>(right.at(rightEnd))) != 0) ++rightEnd;
            const auto leftLength = leftEnd - leftIndex;
            const auto rightLength = rightEnd - rightIndex;
            if (leftLength != rightLength) return leftLength < rightLength ? -1 : 1;
        } else {
            while (leftEnd < left.size() &&
                   std::isalpha(static_cast<unsigned char>(left.at(leftEnd))) != 0) ++leftEnd;
            while (rightEnd < right.size() &&
                   std::isalpha(static_cast<unsigned char>(right.at(rightEnd))) != 0) ++rightEnd;
        }
        const auto comparison = QByteArrayView(left).sliced(leftIndex, leftEnd - leftIndex)
                                    .compare(QByteArrayView(right).sliced(rightIndex,
                                                                         rightEnd - rightIndex));
        if (comparison != 0) return comparison < 0 ? -1 : 1;
        leftIndex = leftEnd;
        rightIndex = rightEnd;
    }
    return 0;
}

} // namespace

int DebianVersion::compare(const QString &left, const QString &right) {
    const auto leftParts = splitDebianVersion(left);
    const auto rightParts = splitDebianVersion(right);
    if (leftParts.epoch != rightParts.epoch) return leftParts.epoch < rightParts.epoch ? -1 : 1;
    const auto upstream = compareDebianPart(leftParts.upstream, rightParts.upstream);
    return upstream != 0 ? upstream : compareDebianPart(leftParts.revision, rightParts.revision);
}

int RpmVersion::compare(const QString &left, const QString &right) {
    const auto leftParts = splitRpmVersion(left);
    const auto rightParts = splitRpmVersion(right);
    if (leftParts.epoch != rightParts.epoch) return leftParts.epoch < rightParts.epoch ? -1 : 1;
    const auto version = compareRpmPart(leftParts.version, rightParts.version);
    return version != 0 ? version : compareRpmPart(leftParts.release, rightParts.release);
}

} // namespace pacsmith
