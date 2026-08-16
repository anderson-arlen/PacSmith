#include "core/rpm_analyzer.hpp"

#include "core/dependency_parser.hpp"
#include "core/path_safety.hpp"
#include "core/script_evidence.hpp"

#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <limits>

namespace pacsmith {
namespace {

constexpr qsizetype rpmLeadSize = 96;
constexpr quint32 maximumHeaderEntries = 131072;
constexpr quint32 maximumHeaderStore = 64U * 1024U * 1024U;

enum class RpmDataType : quint32 {
    Int8 = 2,
    Int16 = 3,
    Int32 = 4,
    Int64 = 5,
    String = 6,
    Binary = 7,
    StringArray = 8,
    I18nString = 9
};

struct HeaderEntry {
    quint32 tag{0};
    RpmDataType type{RpmDataType::Binary};
    quint32 offset{0};
    quint32 count{0};
};

struct HeaderSection {
    QList<HeaderEntry> entries;
    QByteArray store;
    qint64 endOffset{0};

    [[nodiscard]] const HeaderEntry *entry(const quint32 tag) const {
        const auto found = std::find_if(entries.cbegin(), entries.cend(),
                                        [tag](const auto &candidate) {
                                            return candidate.tag == tag;
                                        });
        return found == entries.cend() ? nullptr : &*found;
    }
};

bool readBe32(const QByteArrayView bytes, const qsizetype offset, quint32 &result) {
    if (offset < 0 || offset + 4 > bytes.size()) return false;
    result = (static_cast<quint32>(static_cast<unsigned char>(bytes.at(offset))) << 24U) |
             (static_cast<quint32>(static_cast<unsigned char>(bytes.at(offset + 1))) << 16U) |
             (static_cast<quint32>(static_cast<unsigned char>(bytes.at(offset + 2))) << 8U) |
             static_cast<quint32>(static_cast<unsigned char>(bytes.at(offset + 3)));
    return true;
}

std::optional<HeaderSection> readHeader(QFile &file, const qint64 offset,
                                        const QString &description, QString *error) {
    if (!file.seek(offset)) {
        if (error != nullptr) *error = QStringLiteral("Could not seek to the RPM %1 header").arg(description);
        return std::nullopt;
    }
    const auto prefix = file.read(16);
    if (prefix.size() != 16 || static_cast<unsigned char>(prefix.at(0)) != 0x8eU ||
        static_cast<unsigned char>(prefix.at(1)) != 0xadU ||
        static_cast<unsigned char>(prefix.at(2)) != 0xe8U || prefix.at(3) != '\x01') {
        if (error != nullptr) *error = QStringLiteral("Invalid RPM %1 header magic").arg(description);
        return std::nullopt;
    }
    quint32 entryCount = 0;
    quint32 storeSize = 0;
    if (!readBe32(prefix, 8, entryCount) || !readBe32(prefix, 12, storeSize) ||
        entryCount > maximumHeaderEntries || storeSize > maximumHeaderStore) {
        if (error != nullptr) *error = QStringLiteral("RPM %1 header exceeds the safety limit").arg(description);
        return std::nullopt;
    }
    const auto indexBytes = static_cast<quint64>(entryCount) * 16U;
    const auto bodyBytes = indexBytes + storeSize;
    if (bodyBytes > static_cast<quint64>(std::numeric_limits<qsizetype>::max()) ||
        offset > file.size() - 16 ||
        bodyBytes > static_cast<quint64>(file.size() - offset - 16)) {
        if (error != nullptr) *error = QStringLiteral("Truncated RPM %1 header").arg(description);
        return std::nullopt;
    }
    const auto body = file.read(static_cast<qint64>(bodyBytes));
    if (body.size() != static_cast<qsizetype>(bodyBytes)) {
        if (error != nullptr) *error = QStringLiteral("Could not read the complete RPM %1 header").arg(description);
        return std::nullopt;
    }

    HeaderSection result;
    result.entries.reserve(static_cast<qsizetype>(entryCount));
    for (quint32 index = 0; index < entryCount; ++index) {
        const auto position = static_cast<qsizetype>(index) * 16;
        quint32 tag = 0;
        quint32 type = 0;
        quint32 valueOffset = 0;
        quint32 count = 0;
        if (!readBe32(body, position, tag) || !readBe32(body, position + 4, type) ||
            !readBe32(body, position + 8, valueOffset) || !readBe32(body, position + 12, count) ||
            valueOffset > storeSize || count > maximumHeaderEntries) {
            if (error != nullptr) *error = QStringLiteral("Invalid RPM %1 header index").arg(description);
            return std::nullopt;
        }
        result.entries.append({tag, static_cast<RpmDataType>(type), valueOffset, count});
    }
    result.store = body.sliced(static_cast<qsizetype>(indexBytes), static_cast<qsizetype>(storeSize));
    result.endOffset = offset + 16 + static_cast<qint64>(bodyBytes);
    return result;
}

QStringList strings(const HeaderSection &header, const quint32 tag) {
    const auto *entry = header.entry(tag);
    if (entry == nullptr || (entry->type != RpmDataType::String &&
                             entry->type != RpmDataType::StringArray &&
                             entry->type != RpmDataType::I18nString) ||
        entry->offset >= static_cast<quint32>(header.store.size())) {
        return {};
    }
    QStringList result;
    auto position = static_cast<qsizetype>(entry->offset);
    const auto count = entry->type == RpmDataType::String ? 1U : entry->count;
    for (quint32 index = 0; index < count && position < header.store.size(); ++index) {
        const auto end = header.store.indexOf('\0', position);
        if (end < 0) return {};
        result.append(QString::fromUtf8(header.store.constData() + position, end - position));
        position = end + 1;
    }
    return result;
}

QString string(const HeaderSection &header, const quint32 tag) {
    const auto values = strings(header, tag);
    return values.isEmpty() ? QString{} : values.first();
}

QList<quint64> integers(const HeaderSection &header, const quint32 tag) {
    const auto *entry = header.entry(tag);
    if (entry == nullptr || entry->offset > static_cast<quint32>(header.store.size())) return {};
    qsizetype width = 0;
    switch (entry->type) {
    case RpmDataType::Int8: width = 1; break;
    case RpmDataType::Int16: width = 2; break;
    case RpmDataType::Int32: width = 4; break;
    case RpmDataType::Int64: width = 8; break;
    default: return {};
    }
    const auto required = static_cast<quint64>(entry->count) * static_cast<quint64>(width);
    if (required > static_cast<quint64>(header.store.size()) - entry->offset) return {};
    QList<quint64> result;
    result.reserve(static_cast<qsizetype>(entry->count));
    auto position = static_cast<qsizetype>(entry->offset);
    for (quint32 index = 0; index < entry->count; ++index) {
        quint64 value = 0;
        for (qsizetype byte = 0; byte < width; ++byte) {
            value = (value << 8U) |
                    static_cast<quint64>(static_cast<unsigned char>(header.store.at(position + byte)));
        }
        result.append(value);
        position += width;
    }
    return result;
}

QString relation(const quint64 flags) {
    constexpr quint64 less = 1U << 1U;
    constexpr quint64 greater = 1U << 2U;
    constexpr quint64 equal = 1U << 3U;
    if ((flags & greater) != 0U && (flags & equal) != 0U) return QStringLiteral(">=");
    if ((flags & less) != 0U && (flags & equal) != 0U) return QStringLiteral("<=");
    if ((flags & greater) != 0U) return QStringLiteral(">");
    if ((flags & less) != 0U) return QStringLiteral("<");
    if ((flags & equal) != 0U) return QStringLiteral("=");
    return {};
}

QList<DependencyMapping> dependencies(const HeaderSection &header) {
    constexpr quint32 requireNameTag = 1049;
    constexpr quint32 requireFlagsTag = 1048;
    constexpr quint32 requireVersionTag = 1050;
    const auto names = strings(header, requireNameTag);
    const auto versions = strings(header, requireVersionTag);
    const auto flags = integers(header, requireFlagsTag);
    constexpr quint64 scriptRequirementMask = (1U << 8U) | (1U << 9U) | (1U << 10U) |
                                               (1U << 11U) | (1U << 12U) | (1U << 13U);
    QList<DependencyMapping> result;
    QSet<QString> seen;
    for (qsizetype index = 0; index < names.size(); ++index) {
        const auto name = names.at(index).trimmed();
        const auto requirementFlags = index < flags.size() ? flags.at(index) : 0U;
        if (name.isEmpty() || name.startsWith(QStringLiteral("rpmlib(")) ||
            name.startsWith(QLatin1Char('/')) || (requirementFlags & scriptRequirementMask) != 0U) {
            continue;
        }
        const auto version = index < versions.size() ? versions.at(index).trimmed() : QString{};
        const auto comparison = relation(requirementFlags);
        const auto raw = comparison.isEmpty() || version.isEmpty()
            ? name : QStringLiteral("%1 (%2 %3)").arg(name, comparison, version);
        if (seen.contains(raw)) continue;
        seen.insert(raw);
        DependencyMapping mapping;
        mapping.rawExpression = raw;
        auto alternativeText = name;
        if (alternativeText.startsWith(QLatin1Char('(')) &&
            alternativeText.endsWith(QLatin1Char(')'))) {
            alternativeText = alternativeText.sliced(1, alternativeText.size() - 2);
        }
        const auto richAlternatives = alternativeText.split(
            QRegularExpression(QStringLiteral("\\s+or\\s+")), Qt::SkipEmptyParts);
        for (const auto &alternative : richAlternatives) {
            const auto candidate = alternative.trimmed();
            static const QRegularExpression packageName(
                QStringLiteral("^[A-Za-z0-9@._+\\-]+$"));
            if (packageName.match(candidate).hasMatch()) {
                mapping.alternatives.append({candidate, comparison, version});
            }
        }
        if (mapping.alternatives.isEmpty()) {
            mapping.alternatives.append({name, comparison, version});
        }
        result.append(mapping);
    }
    static_cast<void>(DependencyParser::applyVerifiedMappings(
        result, DependencyParser::loadVerifiedMappings()));
    return result;
}

void appendScript(QList<MaintainerScript> &scripts, const HeaderSection &header,
                  const quint32 tag, const QString &name) {
    const auto contents = string(header, tag);
    if (!contents.trimmed().isEmpty()) scripts.append({name, contents, {}});
}

void appendScripts(QList<MaintainerScript> &scripts, const HeaderSection &header,
                   const quint32 tag, const QString &prefix) {
    const auto values = strings(header, tag);
    for (qsizetype index = 0; index < values.size(); ++index) {
        if (!values.at(index).trimmed().isEmpty()) {
            scripts.append({QStringLiteral("%1-%2").arg(prefix).arg(index + 1),
                            values.at(index), {}});
        }
    }
}

QString joinVersionedNames(const HeaderSection &header, const quint32 nameTag,
                           const quint32 versionTag, const quint32 flagsTag) {
    const auto names = strings(header, nameTag);
    const auto versions = strings(header, versionTag);
    const auto flags = integers(header, flagsTag);
    QStringList result;
    for (qsizetype index = 0; index < names.size(); ++index) {
        const auto name = names.at(index).trimmed();
        if (name.isEmpty()) continue;
        const auto version = index < versions.size() ? versions.at(index).trimmed() : QString{};
        const auto comparison = relation(index < flags.size() ? flags.at(index) : 0U);
        result.append(comparison.isEmpty() || version.isEmpty()
                          ? name
                          : QStringLiteral("%1 (%2 %3)").arg(name, comparison, version));
    }
    return result.join(QStringLiteral(", "));
}

QMap<QString, QString> fileCapabilities(const HeaderSection &header) {
    constexpr quint32 directoryIndexesTag = 1116;
    constexpr quint32 baseNamesTag = 1117;
    constexpr quint32 directoryNamesTag = 1118;
    constexpr quint32 capabilitiesTag = 5010;
    const auto directoryIndexes = integers(header, directoryIndexesTag);
    const auto baseNames = strings(header, baseNamesTag);
    const auto directoryNames = strings(header, directoryNamesTag);
    const auto capabilities = strings(header, capabilitiesTag);
    QMap<QString, QString> result;
    const auto count = std::min({directoryIndexes.size(), baseNames.size(), capabilities.size()});
    for (qsizetype index = 0; index < count; ++index) {
        const auto capability = capabilities.at(index).trimmed();
        const auto directoryIndex = directoryIndexes.at(index);
        if (capability.isEmpty() || directoryIndex >= static_cast<quint64>(directoryNames.size())) {
            continue;
        }
        auto path = directoryNames.at(static_cast<qsizetype>(directoryIndex)) + baseNames.at(index);
        if (path.startsWith(QLatin1Char('/'))) path.remove(0, 1);
        const auto safe = PathSafety::normalizedArchivePath(path);
        if (safe && !safe->isEmpty()) result.insert(*safe, capability);
    }
    return result;
}

QString joinStrings(const HeaderSection &header, const quint32 tag) {
    return strings(header, tag).join(QStringLiteral(", "));
}

} // namespace

std::optional<RpmAnalysis> RpmAnalyzer::analyzeHeader(const std::filesystem::path &path,
                                                      QString *error) {
    QFile file(QString::fromUtf8(path.string().c_str()));
    if (!file.open(QIODevice::ReadOnly)) {
        if (error != nullptr) *error = file.errorString();
        return std::nullopt;
    }
    const auto lead = file.read(rpmLeadSize);
    if (lead.size() != rpmLeadSize || static_cast<unsigned char>(lead.at(0)) != 0xedU ||
        static_cast<unsigned char>(lead.at(1)) != 0xabU ||
        static_cast<unsigned char>(lead.at(2)) != 0xeeU ||
        static_cast<unsigned char>(lead.at(3)) != 0xdbU) {
        if (error != nullptr) *error = QStringLiteral("Source does not have an RPM package lead");
        return std::nullopt;
    }
    const auto signature = readHeader(file, rpmLeadSize, QStringLiteral("signature"), error);
    if (!signature) return std::nullopt;
    const auto mainOffset = (signature->endOffset + 7) & ~qint64{7};
    const auto header = readHeader(file, mainOffset, QStringLiteral("metadata"), error);
    if (!header) return std::nullopt;

    constexpr quint32 nameTag = 1000;
    constexpr quint32 versionTag = 1001;
    constexpr quint32 releaseTag = 1002;
    constexpr quint32 epochTag = 1003;
    constexpr quint32 summaryTag = 1004;
    constexpr quint32 descriptionTag = 1005;
    constexpr quint32 buildHostTag = 1007;
    constexpr quint32 installedSizeTag = 1009;
    constexpr quint32 distributionTag = 1010;
    constexpr quint32 vendorTag = 1011;
    constexpr quint32 licenseTag = 1014;
    constexpr quint32 packagerTag = 1015;
    constexpr quint32 groupTag = 1016;
    constexpr quint32 urlTag = 1020;
    constexpr quint32 osTag = 1021;
    constexpr quint32 architectureTag = 1022;
    constexpr quint32 preinTag = 1023;
    constexpr quint32 postinTag = 1024;
    constexpr quint32 preunTag = 1025;
    constexpr quint32 postunTag = 1026;
    constexpr quint32 provideNameTag = 1047;
    constexpr quint32 sourceRpmTag = 1044;
    constexpr quint32 conflictNameTag = 1054;
    constexpr quint32 verifyScriptTag = 1079;
    constexpr quint32 triggerScriptsTag = 1065;
    constexpr quint32 pretransTag = 1151;
    constexpr quint32 posttransTag = 1152;
    constexpr quint32 preuntransTag = 5103;
    constexpr quint32 postuntransTag = 5104;
    constexpr quint32 obsoleteNameTag = 1090;
    constexpr quint32 preinProgramTag = 1085;
    constexpr quint32 postinProgramTag = 1086;
    constexpr quint32 preunProgramTag = 1087;
    constexpr quint32 postunProgramTag = 1088;
    constexpr quint32 verifyProgramTag = 1091;
    constexpr quint32 triggerProgramTag = 1092;
    constexpr quint32 pretransProgramTag = 1153;
    constexpr quint32 posttransProgramTag = 1154;
    constexpr quint32 recommendNameTag = 5046;
    constexpr quint32 recommendVersionTag = 5047;
    constexpr quint32 recommendFlagsTag = 5048;
    constexpr quint32 suggestNameTag = 5049;
    constexpr quint32 suggestVersionTag = 5050;
    constexpr quint32 suggestFlagsTag = 5051;
    constexpr quint32 supplementNameTag = 5052;
    constexpr quint32 supplementVersionTag = 5053;
    constexpr quint32 supplementFlagsTag = 5054;
    constexpr quint32 enhanceNameTag = 5055;
    constexpr quint32 enhanceVersionTag = 5056;
    constexpr quint32 enhanceFlagsTag = 5057;
    constexpr quint32 fileTriggerScriptsTag = 5066;
    constexpr quint32 fileTriggerProgramsTag = 5067;
    constexpr quint32 transFileTriggerScriptsTag = 5076;
    constexpr quint32 transFileTriggerProgramsTag = 5077;
    constexpr quint32 preuntransProgramTag = 5105;
    constexpr quint32 postuntransProgramTag = 5106;
    constexpr quint32 sysusersTag = 5109;
    constexpr quint32 payloadFormatTag = 1124;
    constexpr quint32 payloadCompressorTag = 1125;

    RpmAnalysis result;
    result.metadata.package = string(*header, nameTag);
    auto version = string(*header, versionTag);
    const auto release = string(*header, releaseTag);
    if (!release.isEmpty()) version += QLatin1Char('-') + release;
    const auto epochs = integers(*header, epochTag);
    if (!epochs.isEmpty() && epochs.first() > 0U) {
        version.prepend(QString::number(epochs.first()) + QLatin1Char(':'));
    }
    result.metadata.version = version;
    result.metadata.architecture = string(*header, architectureTag);
    result.metadata.maintainer = string(*header, packagerTag);
    if (result.metadata.maintainer.isEmpty()) result.metadata.maintainer = string(*header, vendorTag);
    result.metadata.description = string(*header, summaryTag);
    const auto longDescription = string(*header, descriptionTag);
    if (!longDescription.isEmpty() && longDescription != result.metadata.description) {
        result.metadata.description += QLatin1Char('\n') + longDescription;
    }
    result.metadata.homepage = string(*header, urlTag);
    result.dependencies = dependencies(*header);
    QStringList dependencyExpressions;
    for (const auto &dependency : result.dependencies) dependencyExpressions.append(dependency.rawExpression);
    result.metadata.depends = dependencyExpressions.join(QStringLiteral(", "));
    result.metadata.conflicts = joinStrings(*header, conflictNameTag);
    result.metadata.provides = joinStrings(*header, provideNameTag);
    result.metadata.recommends = joinVersionedNames(
        *header, recommendNameTag, recommendVersionTag, recommendFlagsTag);
    result.metadata.suggests = joinVersionedNames(
        *header, suggestNameTag, suggestVersionTag, suggestFlagsTag);
    result.metadata.rawFields.insert(QStringLiteral("RPM-Name"), result.metadata.package);
    result.metadata.rawFields.insert(QStringLiteral("RPM-Version"), string(*header, versionTag));
    result.metadata.rawFields.insert(QStringLiteral("RPM-Release"), release);
    result.metadata.rawFields.insert(QStringLiteral("Architecture"), result.metadata.architecture);
    result.metadata.rawFields.insert(QStringLiteral("Vendor"), string(*header, vendorTag));
    result.metadata.rawFields.insert(QStringLiteral("Packager"), string(*header, packagerTag));
    result.metadata.rawFields.insert(QStringLiteral("License"), string(*header, licenseTag));
    result.metadata.rawFields.insert(QStringLiteral("Group"), string(*header, groupTag));
    result.metadata.rawFields.insert(QStringLiteral("Distribution"), string(*header, distributionTag));
    result.metadata.rawFields.insert(QStringLiteral("Build-Host"), string(*header, buildHostTag));
    result.metadata.rawFields.insert(QStringLiteral("Operating-System"), string(*header, osTag));
    result.metadata.rawFields.insert(QStringLiteral("Source-RPM"), string(*header, sourceRpmTag));
    const auto installedSizes = integers(*header, installedSizeTag);
    result.metadata.rawFields.insert(
        QStringLiteral("Installed-Size"),
        installedSizes.isEmpty() ? QString{} : QString::number(installedSizes.first()));
    result.metadata.rawFields.insert(QStringLiteral("URL"), result.metadata.homepage);
    result.metadata.rawFields.insert(QStringLiteral("Summary"), string(*header, summaryTag));
    result.metadata.rawFields.insert(QStringLiteral("Description"), longDescription);
    result.metadata.rawFields.insert(QStringLiteral("Requires"), result.metadata.depends);
    result.metadata.rawFields.insert(QStringLiteral("Provides"), result.metadata.provides);
    result.metadata.rawFields.insert(QStringLiteral("Conflicts"), result.metadata.conflicts);
    result.metadata.rawFields.insert(QStringLiteral("Recommends"), result.metadata.recommends);
    result.metadata.rawFields.insert(QStringLiteral("Suggests"), result.metadata.suggests);
    result.metadata.rawFields.insert(
        QStringLiteral("Supplements"),
        joinVersionedNames(*header, supplementNameTag, supplementVersionTag,
                           supplementFlagsTag));
    result.metadata.rawFields.insert(
        QStringLiteral("Enhances"),
        joinVersionedNames(*header, enhanceNameTag, enhanceVersionTag, enhanceFlagsTag));
    result.metadata.rawFields.insert(QStringLiteral("Obsoletes"),
                                     joinStrings(*header, obsoleteNameTag));
    result.metadata.rawFields.insert(QStringLiteral("Pre-Install-Interpreter"),
                                     joinStrings(*header, preinProgramTag));
    result.metadata.rawFields.insert(QStringLiteral("Post-Install-Interpreter"),
                                     joinStrings(*header, postinProgramTag));
    result.metadata.rawFields.insert(QStringLiteral("Pre-Uninstall-Interpreter"),
                                     joinStrings(*header, preunProgramTag));
    result.metadata.rawFields.insert(QStringLiteral("Post-Uninstall-Interpreter"),
                                     joinStrings(*header, postunProgramTag));
    result.metadata.rawFields.insert(QStringLiteral("Verify-Script-Interpreter"),
                                     joinStrings(*header, verifyProgramTag));
    result.metadata.rawFields.insert(QStringLiteral("Trigger-Script-Interpreters"),
                                     joinStrings(*header, triggerProgramTag));
    result.metadata.rawFields.insert(QStringLiteral("Pre-Transaction-Interpreter"),
                                     joinStrings(*header, pretransProgramTag));
    result.metadata.rawFields.insert(QStringLiteral("Post-Transaction-Interpreter"),
                                     joinStrings(*header, posttransProgramTag));
    result.metadata.rawFields.insert(QStringLiteral("File-Trigger-Interpreters"),
                                     joinStrings(*header, fileTriggerProgramsTag));
    result.metadata.rawFields.insert(QStringLiteral("Transaction-File-Trigger-Interpreters"),
                                     joinStrings(*header, transFileTriggerProgramsTag));
    result.metadata.rawFields.insert(QStringLiteral("Pre-Uninstall-Transaction-Interpreter"),
                                     joinStrings(*header, preuntransProgramTag));
    result.metadata.rawFields.insert(QStringLiteral("Post-Uninstall-Transaction-Interpreter"),
                                     joinStrings(*header, postuntransProgramTag));
    result.metadata.rawFields.insert(QStringLiteral("RPM-Payload-Format"), string(*header, payloadFormatTag));
    result.metadata.rawFields.insert(QStringLiteral("RPM-Payload-Compressor"), string(*header, payloadCompressorTag));
    result.fileCapabilities = fileCapabilities(*header);

    appendScript(result.maintainerScripts, *header, preinTag, QStringLiteral("prein"));
    appendScript(result.maintainerScripts, *header, postinTag, QStringLiteral("postin"));
    appendScript(result.maintainerScripts, *header, preunTag, QStringLiteral("preun"));
    appendScript(result.maintainerScripts, *header, postunTag, QStringLiteral("postun"));
    appendScript(result.maintainerScripts, *header, pretransTag, QStringLiteral("pretrans"));
    appendScript(result.maintainerScripts, *header, posttransTag, QStringLiteral("posttrans"));
    appendScript(result.maintainerScripts, *header, preuntransTag, QStringLiteral("preuntrans"));
    appendScript(result.maintainerScripts, *header, postuntransTag, QStringLiteral("postuntrans"));
    appendScript(result.maintainerScripts, *header, verifyScriptTag, QStringLiteral("verify"));
    const auto triggers = strings(*header, triggerScriptsTag);
    for (qsizetype index = 0; index < triggers.size(); ++index) {
        if (!triggers.at(index).trimmed().isEmpty()) {
            result.maintainerScripts.append(
                {QStringLiteral("trigger-%1").arg(index + 1), triggers.at(index), {}});
        }
    }
    appendScripts(result.maintainerScripts, *header, fileTriggerScriptsTag,
                  QStringLiteral("file-trigger"));
    appendScripts(result.maintainerScripts, *header, transFileTriggerScriptsTag,
                  QStringLiteral("transaction-file-trigger"));
    appendScripts(result.maintainerScripts, *header, sysusersTag,
                  QStringLiteral("sysusers"));
    result.scriptFindings = ScriptEvidenceAnalyzer::analyze(result.maintainerScripts).findings;
    if (result.metadata.package.isEmpty() || result.metadata.version.isEmpty() ||
        result.metadata.architecture.isEmpty()) {
        if (error != nullptr) {
            *error = QStringLiteral("RPM metadata is missing the package name, version, or architecture");
        }
        return std::nullopt;
    }
    return result;
}

} // namespace pacsmith
