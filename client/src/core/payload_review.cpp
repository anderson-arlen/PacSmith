#include "core/payload_review.hpp"

#include <QCryptographicHash>

#include <algorithm>

namespace pacsmith {
namespace {

bool covers(const QString &parent, const QString &child) {
    return child == parent || child.startsWith(parent + QLatin1Char('/'));
}

const PayloadRule *applicableRule(const PackageRelease &project, const QString &entryPath) {
    const PayloadRule *result = nullptr;
    for (const auto &rule : project.payloadRules) {
        if (covers(rule.path, entryPath) && (result == nullptr || rule.path.size() > result->path.size())) {
            result = &rule;
        }
    }
    return result;
}

} // namespace

QString PayloadReview::fingerprint(const PackageRelease &project, const QString &path) {
    QList<const PayloadEntry *> entries;
    for (const auto &entry : project.payload) {
        if (covers(path, entry.path)) entries.append(&entry);
    }
    std::sort(entries.begin(), entries.end(), [](const auto *left, const auto *right) {
        return left->path < right->path;
    });
    QCryptographicHash hash(QCryptographicHash::Sha256);
    for (const auto *entry : entries) {
        const QStringList fields{entry->path, entry->type, entry->symlinkTarget,
                                 QString::number(entry->size), entry->contentSha256};
        for (const auto &field : fields) {
            hash.addData(field.toUtf8());
            hash.addData(QByteArrayView{"\0", 1});
        }
    }
    return entries.isEmpty() ? QString{} : QString::fromLatin1(hash.result().toHex());
}

PayloadReviewState PayloadReview::state(const PackageRelease &project, const PayloadEntry &entry) {
    const auto *rule = applicableRule(project, entry.path);
    if (!entry.requiresReview && rule == nullptr) {
        return {PayloadDisposition::Included, false, entry.path};
    }
    if (rule == nullptr) return {PayloadDisposition::Pending, true, entry.path};
    const auto currentFingerprint = fingerprint(project, rule->path);
    if (!rule->userDecision) {
        if (rule->reason == QStringLiteral("AI-reviewed payload decision") ||
            rule->reason == QStringLiteral("User-approved AI payload decision")) {
            const auto current = rule->excluded ? PayloadDisposition::Excluded
                                                : PayloadDisposition::Included;
            return {current, currentFingerprint.isEmpty() ||
                                 currentFingerprint != rule->acknowledgedFingerprint,
                    rule->path};
        }
        const bool stale = currentFingerprint.isEmpty() ||
                           rule->acknowledgedFingerprint.isEmpty() ||
                           currentFingerprint != rule->acknowledgedFingerprint;
        return {PayloadDisposition::ExcludedByDefault, stale, rule->path};
    }
    if (currentFingerprint.isEmpty() || currentFingerprint != rule->acknowledgedFingerprint) {
        return {rule->excluded ? PayloadDisposition::Excluded : PayloadDisposition::Included, true, rule->path};
    }
    return {rule->excluded ? PayloadDisposition::Excluded : PayloadDisposition::Included, false, rule->path};
}

void PayloadReview::decide(PackageRelease &project, const QString &path, const bool exclude) {
    auto iterator = std::find_if(project.payloadRules.begin(), project.payloadRules.end(),
                                 [&path](const auto &rule) { return rule.path == path; });
    if (iterator == project.payloadRules.end()) {
        project.payloadRules.append({path, exclude, QStringLiteral("User-reviewed payload decision"), true,
                                     fingerprint(project, path)});
        return;
    }
    iterator->excluded = exclude;
    iterator->reason = QStringLiteral("User-reviewed payload decision");
    iterator->userDecision = true;
    iterator->acknowledgedFingerprint = fingerprint(project, path);
}

void PayloadReview::bindDefaultExclusions(PackageRelease &project) {
    for (auto &rule : project.payloadRules) {
        if (rule.userDecision || !rule.excluded) continue;
        if (rule.reason == QStringLiteral("AI-reviewed payload decision") ||
            rule.reason == QStringLiteral("User-approved AI payload decision")) {
            continue;
        }
        const auto current = fingerprint(project, rule.path);
        if (current.isEmpty()) continue;
        rule.acknowledgedFingerprint = current;
    }
}

void PayloadReview::adoptFilledContentHash(PackageRelease &project, const QString &entryPath) {
    auto entry = std::find_if(project.payload.begin(), project.payload.end(),
                              [&entryPath](const auto &candidate) { return candidate.path == entryPath; });
    if (entry == project.payload.end() || entry->contentSha256.isEmpty()) return;
    const auto filled = entry->contentSha256;
    entry->contentSha256.clear();
    for (auto &rule : project.payloadRules) {
        if (rule.acknowledgedFingerprint.isEmpty() || !covers(rule.path, entryPath)) continue;
        const auto withoutHash = fingerprint(project, rule.path);
        if (withoutHash != rule.acknowledgedFingerprint) continue;
        entry->contentSha256 = filled;
        const auto withHash = fingerprint(project, rule.path);
        entry->contentSha256.clear();
        if (!withHash.isEmpty()) rule.acknowledgedFingerprint = withHash;
    }
    entry->contentSha256 = filled;
}

void PayloadReview::clearDecision(PackageRelease &project, const QString &path) {
    auto iterator = std::find_if(project.payloadRules.begin(), project.payloadRules.end(),
                                 [&path](const auto &rule) { return rule.path == path; });
    if (iterator == project.payloadRules.end()) return;
    if (iterator->reason == QStringLiteral("User-reviewed payload decision") ||
        iterator->reason == QStringLiteral("User-approved AI payload decision")) {
        project.payloadRules.erase(iterator);
    } else if (iterator->reason == QStringLiteral("AI-reviewed payload decision")) {
        project.payloadRules.erase(iterator);
    } else {
        iterator->userDecision = false;
        iterator->acknowledgedFingerprint.clear();
    }
}

} // namespace pacsmith
