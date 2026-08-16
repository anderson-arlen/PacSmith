#include "core/script_evidence.hpp"

#include "core/apt_sources.hpp"
#include "core/path_safety.hpp"

#include <QCryptographicHash>
#include <QMap>
#include <QRegularExpression>
#include <QSet>
#include <QUrl>

#include <algorithm>

namespace pacsmith {
namespace {

QString fingerprint(const QString &name, const QString &evidence) {
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(name.toUtf8());
    hash.addData(QByteArrayView{"\0", 1});
    hash.addData(evidence.toUtf8());
    return QString::fromLatin1(hash.result().toHex());
}

FieldProvenance deterministicProvenance(const QString &sourceFingerprint, const QString &rationale) {
    return {ValueOrigin::Deterministic, {}, {}, sourceFingerprint, rationale,
            QDateTime::currentDateTimeUtc(), false};
}

void addFinding(QList<ScriptFinding> &findings, const MaintainerScript &script,
                const QString &kind, const QString &summary, const QString &evidence,
                const ScriptDisposition disposition, const QString &rationale) {
    // A single script commonly carries several independent responsibilities.
    // Include the responsibility kind in the identity so constrained AI output
    // can address each finding exactly once without two schema entries sharing
    // the same enum value.
    const auto evidenceHash = fingerprint(
        QStringLiteral("%1\n%2").arg(script.name, kind), evidence);
    const auto duplicate = std::any_of(findings.cbegin(), findings.cend(), [&](const auto &finding) {
        return finding.scriptName == script.name && finding.kind == kind;
    });
    if (duplicate) return;
    findings.append({script.name, kind, summary, evidence.left(4096), evidenceHash, disposition,
                     deterministicProvenance(evidenceHash, rationale)});
}

QMap<QString, QString> literalAssignments(const QString &script) {
    QMap<QString, QString> result;
    static const QRegularExpression assignment(
        QStringLiteral(R"((?m)^\s*(?:export\s+|readonly\s+)?([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(?:'([^']*)'|\"([^\"`$]*)\"|([^\s#;]+)))"));
    auto match = assignment.globalMatch(script);
    while (match.hasNext()) {
        const auto current = match.next();
        QString value;
        if (!current.captured(2).isNull()) value = current.captured(2);
        else if (!current.captured(3).isNull()) value = current.captured(3);
        else value = current.captured(4);
        if (value.size() <= 4 * 1024 * 1024) result.insert(current.captured(1), value);
    }
    return result;
}

QString sourceLabel(const MaintainerScript &script) {
    return script.name.startsWith(QStringLiteral("payload/"))
               ? script.name.mid(QStringLiteral("payload/").size())
               : QStringLiteral("control/%1").arg(script.name);
}

QString expandLiteralVariables(QString value, const QMap<QString, QString> &assignments) {
    static const QRegularExpression variable(
        QStringLiteral(R"(\$\{([A-Za-z_][A-Za-z0-9_]*)\}|\$([A-Za-z_][A-Za-z0-9_]*))"));
    for (int pass = 0; pass < 8; ++pass) {
        bool changed = false;
        qsizetype offset = 0;
        while (true) {
            const auto match = variable.match(value, offset);
            if (!match.hasMatch()) break;
            const auto name = match.captured(1).isEmpty() ? match.captured(2) : match.captured(1);
            const auto replacement = assignments.value(name);
            if (replacement.isEmpty()) {
                offset = match.capturedEnd();
                continue;
            }
            value.replace(match.capturedStart(), match.capturedLength(), replacement);
            offset = match.capturedStart() + replacement.size();
            changed = true;
        }
        if (!changed) break;
    }
    return value;
}

void appendRpmCandidate(QList<RpmRepositoryCandidate> &destination,
                        RpmRepositoryCandidate candidate) {
    QUrl url(candidate.baseUrl, QUrl::StrictMode);
    if (!url.isValid() || url.host().isEmpty() || !url.userInfo().isEmpty() ||
        (url.scheme() != QStringLiteral("https") && url.scheme() != QStringLiteral("http"))) {
        return;
    }
    auto path = url.path();
    while (path.endsWith(QLatin1Char('/'))) path.chop(1);
    url.setPath(path);
    candidate.baseUrl = url.toString();
    candidate.keyUrls.removeDuplicates();
    const auto duplicate = std::any_of(destination.cbegin(), destination.cend(),
                                       [&](const auto &existing) {
                                           return existing.baseUrl == candidate.baseUrl &&
                                                  existing.architecture == candidate.architecture;
                                       });
    if (!duplicate) destination.append(std::move(candidate));
}

void appendRpmCandidates(QList<RpmRepositoryCandidate> &destination,
                         const MaintainerScript &script,
                         const QMap<QString, QString> &assignments) {
    QStringList keyUrls;
    for (const auto &candidate : PathSafety::urlsFromText(script.contents)) {
        const auto lower = candidate.toLower();
        if (lower.contains(QStringLiteral("gpg")) || lower.endsWith(QStringLiteral(".asc")) ||
            lower.endsWith(QStringLiteral(".key"))) {
            keyUrls.append(candidate);
        }
    }
    keyUrls.removeDuplicates();

    static const QRegularExpression baseUrl(
        QStringLiteral(R"((?im)^\s*baseurl\s*=\s*([^\s#;]+))"));
    auto matches = baseUrl.globalMatch(script.contents);
    while (matches.hasNext()) {
        auto value = matches.next().captured(1).trimmed();
        if ((value.startsWith(QLatin1Char('\'')) && value.endsWith(QLatin1Char('\''))) ||
            (value.startsWith(QLatin1Char('"')) && value.endsWith(QLatin1Char('"')))) {
            value = value.mid(1, value.size() - 2);
        }
        value = expandLiteralVariables(value, assignments);
        if (value.contains(QLatin1Char('$'))) continue;
        appendRpmCandidate(destination,
                           {value, assignments.value(QStringLiteral("DEFAULT_ARCH")), keyUrls,
                            sourceLabel(script)});
    }

    // Several vendor bootstrap scripts construct baseurl from two literal
    // variables instead of containing a complete repo stanza (Slack is one).
    const auto repository = assignments.value(QStringLiteral("REPOCONFIG"));
    const auto architecture = assignments.value(QStringLiteral("DEFAULT_ARCH"));
    if (!repository.isEmpty() && script.contents.contains(QStringLiteral("$REPOCONFIG"))) {
        auto value = repository;
        if (!architecture.isEmpty() &&
            (script.contents.contains(QStringLiteral("$REPOCONFIG/$DEFAULT_ARCH")) ||
             script.contents.contains(QStringLiteral("${REPOCONFIG}/${DEFAULT_ARCH}")))) {
            value += QLatin1Char('/') + architecture;
        }
        appendRpmCandidate(destination, {value, architecture, keyUrls, sourceLabel(script)});
    }
}

QStringList heredocBodies(const QString &script) {
    QStringList result;
    const auto lines = script.split(QLatin1Char('\n'));
    static const QRegularExpression markerExpression(
        QStringLiteral(R"(<<-?\s*['\"]?([A-Za-z_][A-Za-z0-9_]*)['\"]?)"));
    for (qsizetype lineIndex = 0; lineIndex < lines.size(); ++lineIndex) {
        const auto match = markerExpression.match(lines.at(lineIndex));
        if (!match.hasMatch()) continue;
        const auto marker = match.captured(1);
        QString body;
        for (++lineIndex; lineIndex < lines.size(); ++lineIndex) {
            if (lines.at(lineIndex).trimmed() == marker) break;
            if (body.size() > 4 * 1024 * 1024) break;
            body += lines.at(lineIndex) + QLatin1Char('\n');
        }
        if (!body.isEmpty() && body.size() <= 4 * 1024 * 1024) result.append(body);
    }
    return result;
}

bool likelyOpenPgp(const QByteArray &data) {
    if (data.startsWith("-----BEGIN PGP PUBLIC KEY BLOCK-----")) return true;
    if (data.size() < 8) return false;
    const auto first = static_cast<unsigned char>(data.front());
    return (first & 0x80U) != 0U;
}

void appendSigningKey(QList<ExtractedSigningKey> &keys, const QByteArray &contents,
                      const QString &sourcePath, const QString &sourceFingerprint) {
    const auto duplicate = std::any_of(keys.cbegin(), keys.cend(), [&](const auto &key) {
        return key.contents == contents;
    });
    if (!duplicate) keys.append({contents, sourcePath, sourceFingerprint});
}

// Maintainer scripts are untrusted data. Scan their literal bytes for complete
// OpenPGP armor instead of attempting to interpret or execute the surrounding shell.
void appendArmoredSigningKeys(QList<ExtractedSigningKey> &keys,
                              const MaintainerScript &script) {
    constexpr QByteArrayView beginMarker{"-----BEGIN PGP PUBLIC KEY BLOCK-----"};
    constexpr QByteArrayView endMarker{"-----END PGP PUBLIC KEY BLOCK-----"};
    constexpr qsizetype maximumKeySize = 4 * 1024 * 1024;
    const auto bytes = script.contents.toUtf8();
    qsizetype searchFrom = 0;
    int keyNumber = 1;
    while (searchFrom < bytes.size()) {
        const auto begin = bytes.indexOf(beginMarker, searchFrom);
        if (begin < 0) break;
        const auto end = bytes.indexOf(endMarker, begin + beginMarker.size());
        if (end < 0) break;
        const auto length = end + endMarker.size() - begin;
        searchFrom = end + endMarker.size();
        if (length <= 0 || length > maximumKeySize) continue;

        auto armored = bytes.mid(begin, length);
        armored.append('\n');
        const auto source = QStringLiteral("%1:armored-openpgp-%2")
                                .arg(sourceLabel(script)).arg(keyNumber++);
        appendSigningKey(keys, armored, source,
                         fingerprint(source, QString::fromLatin1(armored)));
    }
}

void appendCandidates(QList<AptRepositoryCandidate> &destination,
                      const QList<AptRepositoryCandidate> &source) {
    for (const auto &candidate : source) {
        const auto duplicate = std::any_of(destination.cbegin(), destination.cend(), [&](const auto &existing) {
            return existing.uri == candidate.uri && existing.suite == candidate.suite &&
                   existing.components == candidate.components &&
                   existing.architectures == candidate.architectures;
        });
        if (!duplicate) destination.append(candidate);
    }
}

} // namespace

ScriptEvidence ScriptEvidenceAnalyzer::analyze(const QList<MaintainerScript> &scripts) {
    ScriptEvidence result;
    for (const auto &script : scripts) {
        const auto assignments = literalAssignments(script.contents);
        for (auto iterator = assignments.cbegin(); iterator != assignments.cend(); ++iterator) {
            const auto upperName = iterator.key().toUpper();
            if (!upperName.contains(QStringLiteral("KEY")) || iterator.value().size() < 64) continue;
            const auto decoded = QByteArray::fromBase64(iterator.value().toLatin1(),
                                                        QByteArray::AbortOnBase64DecodingErrors);
            if (decoded.isEmpty() || decoded.size() > 4 * 1024 * 1024 || !likelyOpenPgp(decoded)) continue;
            const auto source = QStringLiteral("%1:%2").arg(sourceLabel(script), iterator.key());
            appendSigningKey(result.signingKeys, decoded, source,
                             fingerprint(source, iterator.value()));
        }
        appendArmoredSigningKeys(result.signingKeys, script);
        appendRpmCandidates(result.rpmCandidates, script, assignments);

        QList<AptRepositoryCandidate> scriptCandidates;
        for (const auto &body : heredocBodies(script.contents)) {
            appendCandidates(scriptCandidates,
                             AptSourcesParser::parse(QByteArrayView(body.toUtf8()),
                                                     QStringLiteral("%1 heredoc").arg(sourceLabel(script))));
        }
        appendCandidates(scriptCandidates,
                         AptSourcesParser::parse(QByteArrayView(script.contents.toUtf8()),
                                                 sourceLabel(script)));
        appendCandidates(result.aptCandidates, scriptCandidates);

        const auto lower = script.contents.toLower();
        if (!scriptCandidates.isEmpty() || lower.contains(QStringLiteral("/etc/apt/")) ||
            lower.contains(QStringLiteral("sources.list.d")) || lower.contains(QStringLiteral("keyring"))) {
            addFinding(result.findings, script, QStringLiteral("apt-repository"),
                       QStringLiteral("Vendor APT repository and signing-key setup is handled by PacSmith's update checker."),
                       script.contents, ScriptDisposition::HandledByPacSmith,
                       QStringLiteral("Repository configuration is retained for update checks and is not installed as APT configuration on Arch."));
        }
        if (!result.rpmCandidates.isEmpty() &&
            (lower.contains(QStringLiteral("baseurl=")) ||
             lower.contains(QStringLiteral("repoconfig=")))) {
            addFinding(result.findings, script, QStringLiteral("rpm-repository"),
                       QStringLiteral("Vendor RPM repository and signing-key setup is handled by PacSmith's update checker."),
                       script.contents, ScriptDisposition::HandledByPacSmith,
                       QStringLiteral("Repository configuration is retained for signed RPM metadata checks and is not installed as Yum/DNF configuration on Arch."));
        }
        if (lower.contains(QStringLiteral("update-desktop-database"))) {
            addFinding(result.findings, script, QStringLiteral("desktop-database"),
                       QStringLiteral("Desktop database refresh is handled by Arch's ALPM hook."),
                       QStringLiteral("update-desktop-database"), ScriptDisposition::HandledByArch,
                       QStringLiteral("Arch packages trigger the desktop database hook from installed desktop files."));
        }
        if (lower.contains(QStringLiteral("update-mime-database")) ||
            lower.contains(QStringLiteral("gtk-update-icon-cache")) ||
            lower.contains(QStringLiteral("glib-compile-schemas"))) {
            addFinding(result.findings, script, QStringLiteral("arch-cache-hook"),
                       QStringLiteral("Cache/schema refresh is handled by an Arch ALPM hook."),
                       script.contents, ScriptDisposition::HandledByArch,
                       QStringLiteral("The owning Arch package supplies a transaction hook for this cache."));
        }
        if (lower.contains(QStringLiteral("systemctl daemon-reload")) ||
            lower.contains(QStringLiteral("systemd-sysusers")) || lower.contains(QStringLiteral("systemd-tmpfiles"))) {
            addFinding(result.findings, script, QStringLiteral("systemd-hook"),
                       QStringLiteral("Systemd metadata refresh is handled by Arch's systemd ALPM hooks."),
                       script.contents, ScriptDisposition::HandledByArch,
                       QStringLiteral("Arch's systemd package owns transaction hooks for units, sysusers, and tmpfiles."));
        }
        if (lower.contains(QStringLiteral("apparmor"))) {
            addFinding(result.findings, script, QStringLiteral("apparmor"),
                       QStringLiteral("AppArmor profile handling depends on the target system and requires an Arch-specific decision."),
                       script.contents, ScriptDisposition::LifecycleRequired,
                       QStringLiteral("AppArmor is available on Arch but may not be installed or enabled."));
        }
        if (result.findings.cend() == std::find_if(result.findings.cbegin(), result.findings.cend(),
                                                   [&](const auto &finding) {
                                                       return finding.scriptName == script.name;
                                                   })) {
            addFinding(result.findings, script, QStringLiteral("unclassified"),
                       QStringLiteral("No safe deterministic translation was found for this script."),
                       script.contents, ScriptDisposition::Unresolved,
                       QStringLiteral("The original imported package script remains data and needs user or AI resolution."));
        }
    }
    return result;
}

} // namespace pacsmith
