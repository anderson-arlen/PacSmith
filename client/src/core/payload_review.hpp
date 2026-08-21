#pragma once

#include "core/model.hpp"

#include <QString>

namespace pacsmith {

enum class PayloadDisposition { Pending, Included, Excluded, ExcludedByDefault };

struct PayloadReviewState {
    PayloadDisposition disposition{PayloadDisposition::Pending};
    bool needsReview{true};
    QString decisionPath;
};

class PayloadReview final {
public:
    [[nodiscard]] static QString fingerprint(const PackageRelease &release, const QString &path);
    [[nodiscard]] static PayloadReviewState state(const PackageRelease &release, const PayloadEntry &entry);
    static void decide(PackageRelease &release, const QString &path, bool exclude);
    static void bindDefaultExclusions(PackageRelease &release);
    static void adoptFilledContentHash(PackageRelease &release, const QString &entryPath);
    static void clearDecision(PackageRelease &release, const QString &path);
};

} // namespace pacsmith
